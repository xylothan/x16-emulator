#!/usr/bin/env python3
"""Checks the MCP server, over the same pipes and sockets a real client uses.

    python testbench/test_mcp.py <x16emu> <rom.bin>

Each case launches a private emulator with -mcp, or -mcp <port> on a free port.
Every line it writes to stdout under stdio must be one JSON-RPC message: anything
else would break a real client.
"""

import base64
import hashlib
import json
import os
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

passed = 0
failed = 0
EXE = ROM = ""


def case(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print(f"  FAIL {name}" + (f": {detail}" if detail else ""))


class Mcp:
    def __init__(self, extra=(), warp=True):
        self.dir = tempfile.TemporaryDirectory()
        cmd = [EXE, "-rom", ROM, "-mcp", "-hidden", "-zeroram"] + (["-warp"] if warp else []) + list(extra)
        self.p = subprocess.Popen(cmd, cwd=self.dir.name, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.lines = queue.Queue()
        self.noise = []
        self.held = {}
        self.next_id = 1
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.p.stdout:
            try:
                self.lines.put(json.loads(raw.decode("utf-8")))
            except ValueError:
                self.noise.append(raw)
        self.lines.put(None)

    def write(self, text):
        self.p.stdin.write(text.encode() + b"\n")
        self.p.stdin.flush()

    def recv(self, timeout=15):
        try:
            return self.lines.get(timeout=timeout)
        except queue.Empty:
            return None

    def send(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.write(json.dumps(msg))
        return rid

    def reply(self, rid, timeout=15):
        """The response to one request. Others that arrive first are kept."""
        if rid in self.held:
            return self.held.pop(rid)
        end = time.time() + timeout
        while time.time() < end:
            r = self.recv(max(0.05, end - time.time()))
            if r is None:
                return None
            if r.get("id") == rid:
                return r
            self.held[r.get("id")] = r
        return None

    def rpc(self, method, params=None, timeout=15):
        return self.reply(self.send(method, params), timeout)

    def start(self, name, **args):
        """Call a tool without waiting for its answer."""
        return self.send("tools/call", {"name": name, "arguments": args})

    def result(self, rid, timeout=15):
        return (self.reply(rid, timeout) or {}).get("result", {})

    def init(self):
        return self.rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                       "clientInfo": {"name": "test", "version": "1"}})

    def call(self, tool, **args):
        r = self.rpc("tools/call", {"name": tool, "arguments": args})
        return (r or {}).get("result", {})

    def exits(self, secs=10):
        try:
            self.p.wait(secs)
            return True
        except subprocess.TimeoutExpired:
            return False

    def __enter__(self):
        return self

    def __exit__(self, *a):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        if not self.exits(5):
            self.p.kill()
            self.p.wait(5)
        try:
            self.dir.cleanup()
        except OSError:
            pass


def data(result):
    return result.get("structuredContent", {})


def is_error(result):
    return result.get("isError") is True and bool(result.get("content"))


def test_protocol():
    print("protocol")
    with Mcp() as m:
        r = m.init()
        res = (r or {}).get("result", {})
        case("initialize echoes a supported version", res.get("protocolVersion") == "2025-11-25", str(r))
        case("it offers tools", "tools" in res.get("capabilities", {}), str(res))
        case("it names itself", res.get("serverInfo", {}).get("name") == "x16emu", str(res))
        case("it explains the machine", bool(res.get("instructions")))

        r = m.rpc("initialize", {"protocolVersion": "1999-01-01"})
        case("an unknown version gets the latest", r["result"]["protocolVersion"] == "2025-11-25", str(r))
        r = m.rpc("initialize", {"protocolVersion": "2025-06-18"})
        case("an older supported version is kept", r["result"]["protocolVersion"] == "2025-06-18", str(r))

        m.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}))
        r = m.rpc("ping")
        case("a notification gets no reply, and ping answers", r is not None and r.get("result") == {}, str(r))

        r = m.rpc("no/such/method")
        case("unknown method", (r or {}).get("error", {}).get("code") == -32601, str(r))

        m.write("this is not json")
        r = m.recv()
        case("a parse error", (r or {}).get("error", {}).get("code") == -32700 and r.get("id") is None, str(r))

        m.write('[{"jsonrpc":"2.0","id":99,"method":"ping"}]')
        r = m.recv()
        case("a batch is refused", (r or {}).get("error", {}).get("code") == -32600, str(r))

        m.write('{"jsonrpc":"1.0","id":98,"method":"ping"}')
        r = m.recv()
        case("the wrong version is refused", (r or {}).get("error", {}).get("code") == -32600, str(r))

        r = m.rpc("tools/call", {"name": "no_such_tool"})
        case("unknown tool", (r or {}).get("error", {}).get("code") == -32602, str(r))
        r = m.rpc("tools/call", {"name": "get_status", "arguments": [1]})
        case("arguments must be an object", (r or {}).get("error", {}).get("code") == -32602, str(r))

        m.call("get_status")
        case("stdout carried nothing but the protocol", not m.noise, str(m.noise[:2]))


def test_tools():
    print("tools")
    with Mcp() as m:
        m.init()
        r = m.rpc("tools/list")
        listed = (r or {}).get("result", {}).get("tools", [])
        names = {t["name"] for t in listed}
        reading = {"get_status", "get_registers", "read_memory", "disassemble", "list_breakpoints",
                   "wait_for_stop", "perf_stats", "get_screen", "screenshot",
                   "get_vera_state", "lookup_symbol", "list_symbols", "hash_memory", "get_stack"}
        losing = {"reset", "set_registers", "write_memory", "save_state", "load_state",
                  "dump_memory", "load_prg"}
        moving = {"pause", "continue", "step", "run_frames", "set_breakpoint", "remove_breakpoint",
                  "type_text", "send_key", "joystick", "load_symbols", "mouse", "set_warp"}
        case("every tool is listed", names == reading | losing | moving, str(names))
        for t in listed:
            s = t.get("inputSchema", {})
            notes = t.get("annotations", {})
            case(f"{t['name']} has an object schema", s.get("type") == "object", str(s))
            case(f"{t['name']} has a description", bool(t.get("description")))
            case(f"{t['name']} is marked read-only or not",
                 notes.get("readOnlyHint") is (t["name"] in reading), str(notes))
            if t["name"] not in reading:
                case(f"{t['name']} is marked destructive or not",
                     notes.get("destructiveHint") is (t["name"] in losing), str(notes))

        res = m.call("get_status")
        d = data(res)
        for key in ("paused", "frame", "stopId", "stopReason", "cycles"):
            case(f"get_status has {key}", key in d, str(d))
        case("a result has text too", res.get("content", [{}])[0].get("type") == "text", str(res)[:120])

        d = data(m.call("get_registers"))
        case("get_registers has the CPU", "pc" in d.get("cpu", d), str(d)[:160])
        case("every result carries the stop id", "stopId" in d, str(d)[:160])

        d = data(m.call("read_memory", address="$0000", length=8))
        case("read_memory length", d.get("length") == 8, str(d))
        case("read_memory hex", len(d.get("hex", "")) == 16, str(d))
        case("read_memory reports the space", d.get("space") == "cpu", str(d))
        d = data(m.call("read_memory", address=0x100, length=4))
        case("a numeric address", d.get("length") == 4, str(d))
        d = data(m.call("read_memory", address="1FFFE", length=8, space="vram"))
        case("VRAM stops at its end", d.get("length") == 2 and d.get("unreadableBytes") == 6, str(d))
        d = data(m.call("read_memory", address="$C000", length=1, bank=4))
        case("a banked read", d.get("length") == 1, str(d))

        for label, args in (("no address", {}),
                            ("a bad address", {"address": "banana"}),
                            ("too long", {"address": 0, "length": 4097}),
                            ("zero length", {"address": 0, "length": 0}),
                            ("a bad space", {"address": 0, "space": "rom"}),
                            ("a bank on VRAM", {"address": 0, "space": "vram", "bank": 1}),
                            ("a bank out of range", {"address": 0, "bank": 256}),
                            ("a prefix instead of space", {"address": "vram:1000"}),
                            ("an unknown argument", {"address": 0, "banana": 1})):
            case(f"read_memory refuses {label}", is_error(m.call("read_memory", **args)))

        res = m.call("disassemble", address="$C000", count=5)
        case("disassemble count", len(data(res).get("instructions", [])) == 5, str(res)[:200])
        case("disassemble text", "\n" in res["content"][0]["text"], str(res)[:200])
        case("disassemble refuses a bad count", is_error(m.call("disassemble", address=0, count=9999)))

        d = data(m.call("list_breakpoints"))
        case("list_breakpoints", "breakpoints" in d, str(d)[:160])


def test_lifetime():
    print("lifetime")
    with Mcp() as m:
        m.init()
        m.p.stdin.close()
        case("closing stdin ends the emulator", m.exits(15))


def test_with_dap():
    print("alongside DAP")
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    with Mcp(["-debugport", str(port)]) as m:
        m.init()
        deadline = time.time() + 20
        sock = None
        while time.time() < deadline and sock is None:
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            except OSError:
                time.sleep(0.05)
        case("DAP is up beside MCP", sock is not None)
        if sock is None:
            return
        body = json.dumps({"seq": 1, "type": "request", "command": "x16/status"}).encode()
        sock.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        sock.settimeout(10)
        got = b""
        try:
            while b'"x16/status"' not in got:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                got += chunk
        except socket.timeout:
            pass
        case("DAP answers", b'"x16/status"' in got, got[:120].decode(errors="replace"))
        case("MCP still answers", "frame" in data(m.call("get_status")))
        sock.close()
        time.sleep(0.2)
        case("MCP survives the DAP client leaving", "frame" in data(m.call("get_status")))


LOOP = 0x0900  # NOP, NOP, LDA #$05, STA $0910, JMP LOOP
LOOP_CODE = "EAEAA9058D10094C0009"


def plant_loop(m):
    """Pause, and leave the CPU at the top of a small loop in low RAM."""
    m.call("pause")
    m.call("write_memory", address=LOOP, hex=LOOP_CODE)
    # Interrupts masked, or the first step can land in a handler that was pending.
    status = data(m.call("get_registers"))["cpu"]["status"]
    m.call("set_registers", pc=LOOP, status=status | 0x04)


def test_run_control():
    print("run control")
    with Mcp() as m:
        m.init()
        time.sleep(0.5)
        d = data(m.call("pause"))
        case("pause stops it", d.get("paused") is True, str(d))
        first = d.get("stopId", 0)
        case("a stop is counted", first >= 1, str(d))
        d = data(m.call("pause"))
        case("pausing again is not another stop", d.get("stopId") == first, str(d))

        d = data(m.call("wait_for_stop"))
        case("a paused machine has already stopped", d.get("completed") is True and "pc" in d, str(d))

        plant_loop(m)
        d = data(m.call("step"))
        case("step completes", d.get("completed") is True, str(d))
        case("step moved one instruction", d.get("pc") == LOOP + 1, str(d))
        case("step is a new stop", d.get("stopId") == first + 1, str(d))
        for mode, pc in (("into", LOOP + 2), ("over", LOOP + 4)):
            d = data(m.call("step", mode=mode))
            case(f"step {mode}", d.get("pc") == pc and d.get("paused") is True, str(d))
        for label, args in (("a bad mode", {"mode": "sideways"}),
                            ("a bad granularity", {"granularity": "atom"}),
                            ("a timeout too long", {"timeoutMs": 30001})):
            case(f"step refuses {label}", is_error(m.call("step", **args)))

        d = data(m.call("continue"))
        case("continue resumes", d.get("paused") is False, str(d))
        case("step needs a paused machine", is_error(m.call("step")))
        case("run_frames needs a paused machine", is_error(m.call("run_frames")))

        since = d.get("stopId")
        d = data(m.call("wait_for_stop", sinceStopId=since, timeoutMs=100))
        case("a wait times out", d.get("completed") is False and d.get("reason") == "timeout", str(d))
        case("the machine keeps going after a timeout", d.get("paused") is False, str(d))

        rid = m.start("wait_for_stop", sinceStopId=since, timeoutMs=20000)
        case("only one call waits at a time",
             is_error(m.call("wait_for_stop", sinceStopId=since, timeoutMs=10)))
        d = data(m.call("pause"))
        got = data(m.result(rid))
        case("a pause ends a wait", got.get("completed") is True and got.get("stopId") == since + 1, str(got))
        d = data(m.call("wait_for_stop", sinceStopId=since))
        case("a stop before the wait is not missed", d.get("completed") is True, str(d))

        m.call("continue")
        rid = m.start("wait_for_stop", timeoutMs=20000)
        time.sleep(0.1)
        m.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/cancelled",
                            "params": {"requestId": rid, "reason": "test"}}))
        r = m.rpc("ping")
        case("the server answers after a cancel", r is not None and "result" in r, str(r))
        case("a cancelled call gets no response", m.reply(rid, 0.5) is None)
        d = data(m.call("get_status"))
        case("a cancel does not stop the machine", d.get("paused") is False, str(d))
        d = data(m.call("wait_for_stop", timeoutMs=10))
        case("a new wait can start after a cancel", d.get("reason") == "timeout", str(d))

        m.call("pause")
        before = data(m.call("get_status")).get("frame")
        m.call("write_memory", address=0x0920, hex="5A")
        d = data(m.call("run_frames", frames=3, hash=True,
                        read=[{"address": 0x0920, "length": 1},
                              {"address": "1FFFE", "length": 2, "space": "vram"}],
                        joysticks=[{"port": 1, "buttons": ["a"]}]))
        case("run_frames runs them", d.get("framesRun") == 3 and d.get("completed") is True, str(d))
        case("run_frames ends where it said", d.get("frame") == before + 3, str(d))
        case("run_frames stops", d.get("paused") is True and d.get("reason") == "frames", str(d))
        case("run_frames reads as hex", (d.get("reads") or [""])[0] == "5A", str(d))
        case("run_frames reads VRAM", len((d.get("reads") or ["", ""])[1]) == 4, str(d))
        case("run_frames hashes", bool(d.get("hash")), str(d))
        d = data(m.call("joystick", port=1))
        case("run_frames held the button", d.get("buttons") == ["a"], str(d))
        m.call("joystick", port=1, release=True)

        for label, args in (("a bad pad", {"joysticks": [{"port": 9}]}),
                            ("a misspelled pad field", {"joysticks": [{"port": 1, "button": ["a"]}]}),
                            ("a misspelled read field", {"read": [{"addr": 0}]}),
                            ("zero frames", {"frames": 0})):
            case(f"run_frames refuses {label}", is_error(m.call("run_frames", **args)))

        d = data(m.call("run_frames", frames=1000000, timeoutMs=50))
        case("a long run times out", d.get("completed") is False and d.get("paused") is False, str(d))
        rid = m.start("wait_for_stop", timeoutMs=20000)
        d = data(m.call("pause"))
        case("a run that timed out can be paused", d.get("paused") is True, str(d))
        case("and the wait sees it", data(m.result(rid)).get("completed") is True)
        case("nothing is stranded", data(m.call("run_frames", frames=1)).get("framesRun") == 1)

        d = data(m.call("reset"))
        case("reset answers", "stopId" in d, str(d))
        case("stdout stayed clean", not m.noise, str(m.noise[:2]))

    with Mcp() as m:
        m.init()
        m.call("pause")
        m.p.stdin.close()
        case("a paused machine still ends with its client", m.exits(15))


def test_state_tools():
    print("registers, memory, input and states")
    with Mcp() as m:
        m.init()
        time.sleep(0.5)
        m.call("pause")

        d = data(m.call("set_registers", pc="$0900", x=7, y="0x09"))
        case("set_registers reports what it set", set(d.get("registers", {})) == {"pc", "x", "y"}, str(d))
        cpu = data(m.call("get_registers")).get("cpu", {})
        case("the registers changed", cpu.get("pc") == 0x900 and cpu.get("x") & 0xFF == 7 and cpu.get("y") & 0xFF == 9, str(cpu))
        keep = cpu.get("a")
        case("a bad value changes nothing", is_error(m.call("set_registers", a=1, x="banana")) and
             data(m.call("get_registers"))["cpu"]["a"] == keep)
        case("set_registers needs a register", is_error(m.call("set_registers")))
        case("set_registers refuses an unknown one", is_error(m.call("set_registers", q=1)))

        d = data(m.call("write_memory", address="$0A00", hex="DEADBEEF"))
        case("write_memory counts", d.get("bytesWritten") == 4, str(d))
        case("write_memory lands", data(m.call("read_memory", address="$0A00", length=4)).get("hex") == "DEADBEEF")
        m.call("write_memory", address="1B000", hex="1234", space="vram")
        case("VRAM is written", data(m.call("read_memory", address="1B000", length=2, space="vram")).get("hex") == "1234")
        for label, args in (("odd digits", {"address": 0, "hex": "ABC"}),
                            ("no digits", {"address": 0, "hex": ""}),
                            ("not hex", {"address": 0, "hex": "ZZ"}),
                            ("no data", {"address": 0}),
                            ("too much", {"address": 0, "hex": "00" * 4097}),
                            ("the end of VRAM", {"address": "1FFFF", "hex": "0000", "space": "vram"})):
            case(f"write_memory refuses {label}", is_error(m.call("write_memory", **args)))
        d = data(m.call("read_memory", address=0, length=1, space="vram"))
        case("a refused write wrapped nothing", d.get("length") == 1, str(d))

        d = data(m.call("joystick", port=2, buttons=["up", "b"]))
        case("joystick holds", set(d.get("buttons", [])) == {"up", "b"} and d.get("enabled") is True, str(d))
        d = data(m.call("joystick", port=2, buttons=["up"]))
        case("the list is the whole held state", d.get("buttons") == ["up"], str(d))
        d = data(m.call("joystick", port=2, buttons=[]))
        case("an empty list is a pad with nothing held", d.get("buttons") == [] and d.get("enabled") is True, str(d))
        d = data(m.call("joystick", port=2, release=True))
        case("release empties the port", d.get("enabled") is False, str(d))
        for label, args in (("no port", {}), ("port 0", {"port": 0}), ("port 5", {"port": 5}),
                            ("both", {"port": 1, "buttons": [], "release": True}),
                            ("an unknown button", {"port": 1, "buttons": ["z"]})):
            case(f"joystick refuses {label}", is_error(m.call("joystick", **args)))

        case("send_key", "scancode" in data(m.call("send_key", key="Return")))
        case("send_key holds", data(m.call("send_key", key="A", action="down")).get("action") == "down")
        m.call("send_key", key="A", action="up")
        for label, args in (("no key", {}), ("a bad key", {"key": "Banana"}),
                            ("a bad action", {"key": "A", "action": "wiggle"}),
                            ("a bad modifier", {"key": "A", "modifiers": ["Banana"]})):
            case(f"send_key refuses {label}", is_error(m.call("send_key", **args)))
        case("type_text", data(m.call("type_text", text="PRINT 1\n")).get("length") == 8)
        case("type_text needs text", is_error(m.call("type_text")))
        case("perf_stats", "budget" in data(m.call("perf_stats")))

        m.call("run_frames", frames=1)
        d = data(m.call("save_state", slot="a"))
        case("save_state at a frame boundary", d.get("size", 0) > 0, str(d))
        frame = d.get("frame")
        m.call("run_frames", frames=5)
        d = data(m.call("load_state", slot="a"))
        case("load_state goes back", d.get("frame") == frame, str(d))
        m.call("step")
        res = m.call("save_state", slot="b")
        case("save_state says how to reach a boundary",
             is_error(res) and "frames" in res["content"][0]["text"], str(res)[:160])
        case("load_state of no such slot", is_error(m.call("load_state", slot="nope")))
        case("save_state takes no path", is_error(m.call("save_state", slot="a", path="x.bin")))
        case("load_state takes no path", is_error(m.call("load_state", path="x.bin")))
        case("no file was written", not os.listdir(m.dir.name), str(os.listdir(m.dir.name)))
        m.call("continue")
        case("load_state needs a paused machine", is_error(m.call("load_state", slot="a")))


def owners_at(m, pc):
    for b in data(m.call("list_breakpoints")).get("breakpoints", []):
        if b.get("pc") == pc:
            return b.get("owners", [])
    return []


def test_breakpoints():
    print("breakpoints")
    with Mcp() as m:
        m.init()
        time.sleep(0.5)
        plant_loop(m)

        d = data(m.call("set_breakpoint", address=LOOP + 2))
        case("set_breakpoint names it", d.get("kind") == "exec" and d.get("address") == "$0902", str(d))
        case("list_breakpoints shows MCP", "mcp" in owners_at(m, LOOP + 2), str(owners_at(m, LOOP + 2)))
        since = data(m.call("continue")).get("stopId")
        d = data(m.call("wait_for_stop", sinceStopId=since))
        case("it stops there", d.get("completed") is True and d.get("pc") == LOOP + 2, str(d))
        case("and says why", d.get("stopReason") == "breakpoint", str(d))
        d = data(m.call("remove_breakpoint", address=LOOP + 2))
        case("remove_breakpoint", d.get("removed") is True and d.get("stillArmed") is False, str(d))
        case("it is gone", owners_at(m, LOOP + 2) == [])
        case("removing it twice is refused", is_error(m.call("remove_breakpoint", address=LOOP + 2)))

        m.call("set_registers", pc=LOOP, a=0)
        m.call("set_breakpoint", address=LOOP + 4, condition="A == $05")
        since = data(m.call("continue")).get("stopId")
        d = data(m.call("wait_for_stop", sinceStopId=since))
        case("a condition holds", d.get("pc") == LOOP + 4, str(d))
        d = data(m.call("set_breakpoint", address=LOOP + 4))
        case("this client can change its own condition", d.get("kind") == "exec", str(d))
        m.call("remove_breakpoint", address=LOOP + 4)

        m.call("set_registers", pc=LOOP)
        d = data(m.call("set_breakpoint", address=0x0910, kind="write", condition="== $05"))
        case("a write watch", d.get("kind") == "write" and d.get("length") == 1, str(d))
        since = data(m.call("continue")).get("stopId")
        d = data(m.call("wait_for_stop", sinceStopId=since))
        case("the write stops it", d.get("completed") is True and d.get("stopReason") == "data breakpoint", str(d))
        d = data(m.call("remove_breakpoint", address=0x0910, kind="write"))
        case("the watch comes off", d.get("removed") is True and d.get("stillArmed") is False, str(d))

        d = data(m.call("set_breakpoint", address=0xC000, bank=4))
        case("a banked breakpoint", d.get("address") == "04:C000", str(d))
        case("it is removed by the same key", data(m.call("remove_breakpoint", address="04:C000")).get("removed") is True)
        for label, args in (("a bad address", {"address": "banana"}),
                            ("a bad kind", {"address": 0, "kind": "touch"}),
                            ("a bad condition", {"address": 0x900, "condition": "A ?? 3"}),
                            ("a bad hit count", {"address": 0x900, "hitCondition": "often"}),
                            ("a bank outside a window", {"address": 0x0900, "bank": 3}),
                            ("VRAM", {"address": "vram:1000"}),
                            ("a bank on a watch", {"address": 0x900, "kind": "write", "bank": 1}),
                            ("a range off the end", {"address": 0xFFFF, "kind": "write", "length": 2}),
                            ("a filter on a range", {"address": 0x900, "kind": "write", "length": 2,
                                                     "condition": "== 1"})):
            case(f"set_breakpoint refuses {label}", is_error(m.call("set_breakpoint", **args)))

    with Mcp(["-bp", "0950"]) as m:
        m.init()
        time.sleep(0.3)
        m.call("pause")
        m.call("set_breakpoint", address=0x0950)
        case("two owners share it", "mcp" in owners_at(m, 0x0950) and len(owners_at(m, 0x0950)) == 2,
             str(owners_at(m, 0x0950)))
        res = m.call("set_breakpoint", address=0x0950, condition="A == 1")
        case("a condition on a shared breakpoint is refused", is_error(res), str(res)[:160])
        d = data(m.call("remove_breakpoint", address=0x0950))
        case("removing leaves a -bp armed", d.get("stillArmed") is True, str(d))
        owners = owners_at(m, 0x0950)
        case("and only MCP's claim went", owners and "mcp" not in owners, str(owners))


class Dap:
    def __init__(self, port):
        self.sock = None
        deadline = time.time() + 20
        while time.time() < deadline and self.sock is None:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            except OSError:
                time.sleep(0.05)
        self.sock.settimeout(0.2)
        self.buf = b""
        self.seq = 0
        self.events = []
        self.answers = {}

    def _pump(self):
        try:
            chunk = self.sock.recv(65536)
            if chunk:
                self.buf += chunk
        except (socket.timeout, OSError):
            pass
        while True:
            end = self.buf.find(b"\r\n\r\n")
            if end < 0:
                return
            length = int(self.buf[:end].split(b":")[1])
            if len(self.buf) < end + 4 + length:
                return
            msg = json.loads(self.buf[end + 4:end + 4 + length])
            self.buf = self.buf[end + 4 + length:]
            if msg.get("type") == "event":
                self.events.append(msg)
            elif msg.get("type") == "response":
                self.answers[msg["request_seq"]] = msg

    def send(self, command, arguments=None):
        self.seq += 1
        msg = {"seq": self.seq, "type": "request", "command": command}
        if arguments is not None:
            msg["arguments"] = arguments
        body = json.dumps(msg).encode()
        self.sock.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        return self.seq

    def answer(self, seq, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            if seq in self.answers:
                return self.answers.pop(seq)
            self._pump()
        return None

    def request(self, command, arguments=None, timeout=15):
        return self.answer(self.send(command, arguments), timeout)

    def event(self, name, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            for i, e in enumerate(self.events):
                if e.get("event") == name:
                    return self.events.pop(i)
            self._pump()
        return None

    def session(self, kind="attach"):
        self.request("initialize", {"adapterID": "test", "linesStartAt1": True, "columnsStartAt1": True})
        self.request(kind, {})
        self.request("configurationDone")

    def close(self):
        self.sock.close()


def free_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def test_coexistence():
    print("beside a DAP client")

    with Mcp(["-debugport", str(free_port())]) as m:
        port = int(m.p.args[m.p.args.index("-debugport") + 1])
        m.init()
        time.sleep(0.5)

        dap = Dap(port)
        dap.session()
        plant_loop(m)
        dap.event("stopped")
        m.call("set_breakpoint", address=LOOP + 2)
        dap.close()
        time.sleep(0.4)
        case("an MCP pause survives a DAP client dropping", data(m.call("get_status")).get("paused") is True)
        case("MCP breakpoints survive it", "mcp" in owners_at(m, LOOP + 2))
        m.call("remove_breakpoint", address=LOOP + 2)

        dap = Dap(port)
        dap.session()
        dap.request("disconnect", {})
        dap.close()
        time.sleep(0.4)
        case("an MCP pause survives a DAP disconnect", data(m.call("get_status")).get("paused") is True)

        dap = Dap(port)
        dap.session()
        dap.events.clear()
        m.call("continue")
        case("an MCP resume reaches DAP", dap.event("continued") is not None)
        m.call("pause")
        e = dap.event("stopped")
        case("an MCP stop reaches DAP", e is not None, str(e))
        m.call("step")
        case("an MCP step shows as continued", dap.event("continued") is not None)
        case("and as stopped", dap.event("stopped") is not None)
        dap.events.clear()
        m.call("run_frames", frames=2)
        case("an MCP frame run shows as continued", dap.event("continued") is not None)
        case("and as stopped", dap.event("stopped") is not None)

        rid = m.start("run_frames", frames=100000000, timeoutMs=20000)
        time.sleep(0.2)
        r = dap.request("continue", {"threadId": 1})
        case("DAP continue is answered", bool(r and r.get("success")), str(r))
        d = data(m.result(rid))
        case("a DAP continue ends an MCP run as cancelled",
             d.get("reason") == "cancelled" and d.get("completed") is False, str(d))
        case("and the machine is running", d.get("paused") is False, str(d))

        m.call("pause")
        dap.events.clear()
        seq = dap.send("x16/step", {"frames": 100000000})
        time.sleep(0.2)
        m.call("continue")
        r = dap.answer(seq)
        case("an MCP continue ends a DAP run as cancelled",
             bool(r) and r.get("body", {}).get("reason") == "cancelled", str(r))

        m.call("pause")
        rid = m.start("run_frames", frames=30, timeoutMs=20000)
        dap.close()
        d = data(m.result(rid))
        case("a DAP client leaving does not lose an MCP reply",
             d.get("framesRun") == 30 and d.get("completed") is True, str(d))

        for kind, how in (("launch", "disconnect"), ("launch", "terminate")):
            dap = Dap(port)
            dap.session(kind)
            r = dap.request(how, {})
            case(f"{kind} then {how} answers", bool(r and r.get("success")), str(r))
            dap.close()
            time.sleep(0.4)
            case(f"{kind} then {how} leaves the emulator up for MCP",
                 m.p.poll() is None and "frame" in data(m.call("get_status")))

        m.call("run_frames", frames=1)
        m.call("save_state", slot="same")
        over_mcp = data(m.call("run_frames", frames=10, hash=True)).get("hash")
        m.call("load_state", slot="same")
        dap = Dap(port)
        dap.session()
        r = dap.request("x16/step", {"frames": 10, "hash": True})
        over_dap = (r or {}).get("body", {}).get("hash")
        case("the same frames hash the same over both", bool(over_mcp) and over_mcp == over_dap,
             f"{over_mcp} vs {over_dap}")
        dap.close()


def read_response(sock):
    """One HTTP response: (status, headers with lowercase names, body)."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            return None, {}, b""
        buf += chunk
    head, _, body = buf.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    need = int(headers.get("content-length", 0))
    while len(body) < need:
        chunk = sock.recv(65536)
        if not chunk:
            break
        body += chunk
    return status, headers, body[:need]


def post_bytes(port, body, extra="", content_type="application/json", path="/mcp", host=None):
    host = host if host is not None else f"127.0.0.1:{port}"
    return (f"POST {path} HTTP/1.1\r\nHost: {host}\r\nContent-Type: {content_type}\r\n{extra}"
            f"Content-Length: {len(body)}\r\n\r\n").encode() + body


def rpc_body(rid, method, params=None):
    msg = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        msg["params"] = params
    return json.dumps(msg).encode()


def tool_body(rid, name, **args):
    return rpc_body(rid, "tools/call", {"name": name, "arguments": args})


class HttpMcp:
    def __init__(self, extra=(), env=None):
        self.port = free_port()
        self.dir = tempfile.TemporaryDirectory()
        full_env = dict(os.environ)
        full_env.update(env or {})
        cmd = [EXE, "-rom", ROM, "-mcp", str(self.port), "-hidden", "-warp", "-zeroram"] + list(extra)
        self.log = os.path.join(self.dir.name, "emulator.log")
        with open(self.log, "wb") as log:
            self.p = subprocess.Popen(cmd, cwd=self.dir.name, stdin=subprocess.DEVNULL,
                                      stdout=log, stderr=subprocess.STDOUT, env=full_env)
        self.next_id = 1
        self.sock = self.connect()

    def connect(self):
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=15)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                return s
            except OSError:
                time.sleep(0.05)
        with open(self.log, "rb") as log:
            tail = log.read()[-400:].decode("utf-8", "replace")
        raise RuntimeError(f"the HTTP port never opened (exit {self.p.poll()}): {tail}")

    def raw(self, data):
        """One request on its own connection."""
        s = self.connect()
        try:
            s.sendall(data)
            return read_response(s)
        finally:
            s.close()

    def rpc(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        for attempt in range(2):
            try:
                self.sock.sendall(post_bytes(self.port, rpc_body(rid, method, params)))
                status, _, body = read_response(self.sock)
                if status is not None:
                    return json.loads(body) if body else None
            except OSError:
                pass
            # An idle connection may have been closed; a client just opens another.
            self.sock.close()
            self.sock = self.connect()
        return None

    def init(self):
        return self.rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                       "clientInfo": {"name": "test", "version": "1"}})

    def call(self, tool, **args):
        r = self.rpc("tools/call", {"name": tool, "arguments": args})
        return (r or {}).get("result", {})

    def __enter__(self):
        return self

    def __exit__(self, *a):
        try:
            self.sock.close()
        except OSError:
            pass
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait(5)
        try:
            self.dir.cleanup()
        except OSError:
            pass


def test_http():
    print("HTTP")
    with HttpMcp() as m:
        port = m.port
        r = m.init()
        res = (r or {}).get("result", {})
        case("initialize over HTTP", res.get("protocolVersion") == "2025-11-25", str(r))
        names = {t["name"] for t in (m.rpc("tools/list") or {}).get("result", {}).get("tools", [])}
        case("the same tools", {"pause", "run_frames", "set_breakpoint"} <= names, str(names))
        case("a tool call", "frame" in data(m.call("get_status")))
        case("it outlives stdin closing, since stdin is not its transport", m.p.poll() is None)

        status, headers, body = m.raw(post_bytes(port, tool_body(1, "get_status")))
        case("a reply is 200 JSON", status == 200 and headers.get("content-type") == "application/json",
             f"{status} {headers}")
        case("a reply is not cached", headers.get("cache-control") == "no-store", str(headers))
        note = json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}).encode()
        status, headers, body = m.raw(post_bytes(port, note))
        case("a notification is 202 with no body", status == 202 and body == b"", f"{status} {body!r}")
        status, _, body = m.raw(post_bytes(port, b"this is not json"))
        case("a parse error is a 400", status == 400 and json.loads(body)["error"]["code"] == -32700,
             f"{status} {body!r}")
        status, _, body = m.raw(post_bytes(port, b'{"jsonrpc":"2.0","id":1,"method":"ping"}\x00junk'))
        case("a NUL in the body is refused", status == 400, f"{status} {body!r}")
        status, _, body = m.raw(post_bytes(port, rpc_body(7, "no/such")))
        case("an unknown method is still a 200", status == 200 and json.loads(body)["error"]["code"] == -32601,
             f"{status} {body!r}")

        bare = f"Host: 127.0.0.1:{port}\r\n"
        refusals = (
            ("GET", f"GET /mcp HTTP/1.1\r\n{bare}\r\n".encode(), 405),
            ("DELETE", f"DELETE /mcp HTTP/1.1\r\n{bare}\r\n".encode(), 405),
            ("OPTIONS", f"OPTIONS /mcp HTTP/1.1\r\n{bare}Origin: http://x\r\n\r\n".encode(), 403),
            ("OPTIONS anywhere", f"OPTIONS /x HTTP/1.1\r\n{bare}\r\n".encode(), 403),
            ("an Origin", post_bytes(port, b"{}", "Origin: http://example.com\r\n"), 403),
            ("a foreign Host", post_bytes(port, b"{}", host="evil.example"), 403),
            ("another port in Host", post_bytes(port, b"{}", host=f"127.0.0.1:{port + 1}"), 403),
            ("text/plain", post_bytes(port, b"{}", content_type="text/plain"), 415),
            ("an unsupported revision", post_bytes(port, b"{}", "MCP-Protocol-Version: 2026-07-28\r\n"), 400),
            ("a body over 64 KB", post_bytes(port, b"x" * 65537), 413),
            ("another path", post_bytes(port, b"{}", path="/other"), 404),
            ("chunked", f"POST /mcp HTTP/1.1\r\n{bare}Content-Type: application/json\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n".encode(), 501),
        )
        for label, raw, want in refusals:
            status, headers, body = m.raw(raw)
            case(f"{label} is {want}", status == want, f"{status} {body!r}")
            case(f"{label} sends no CORS headers",
                 not [h for h in headers if h.startswith("access-control-")], str(headers))
            case(f"{label} closes", headers.get("connection") == "close", str(headers))
        status, headers, _ = m.raw(f"GET /mcp HTTP/1.1\r\n{bare}\r\n".encode())
        case("a 405 says what is allowed", headers.get("allow") == "POST", str(headers))
        status, _, body = m.raw(post_bytes(port, b"{}", "MCP-Protocol-Version: 2026-07-28\r\n"))
        err = json.loads(body).get("error", {}) if body else {}
        case("a newer revision gets a plain error to fall back on",
             err.get("code") == -32600 and json.loads(body).get("id") is None, str(body))
        for version in ("2025-11-25", "2025-06-18", "2025-03-26"):
            status, _, _ = m.raw(post_bytes(port, rpc_body(1, "ping"), f"MCP-Protocol-Version: {version}\r\n"))
            case(f"revision {version} is accepted", status == 200, str(status))

        shaped = post_bytes(port, tool_body(1, "pause"), "Origin: https://example.com\r\n",
                            content_type="text/plain")
        status, _, _ = m.raw(shaped)
        case("a browser-shaped POST is refused", status == 403, str(status))
        case("and did nothing", data(m.call("get_status")).get("paused") is False)

        s = m.connect()
        whole = post_bytes(port, rpc_body(5, "ping"))
        s.sendall(whole[:30])
        time.sleep(0.05)
        s.sendall(whole[30:-3])
        time.sleep(0.05)
        s.sendall(whole[-3:])
        status, _, body = read_response(s)
        case("a request arriving in pieces", status == 200 and json.loads(body).get("id") == 5, f"{status} {body!r}")
        s.sendall(post_bytes(port, rpc_body(6, "ping")))
        status, _, body = read_response(s)
        case("the connection is reused", status == 200 and json.loads(body).get("id") == 6, f"{status} {body!r}")
        s.close()

        s = m.connect()
        body_bytes = rpc_body(8, "ping")
        s.sendall(f"POST /mcp HTTP/1.1\r\n{bare}Content-Type: application/json\r\nExpect: 100-continue\r\n"
                  f"Content-Length: {len(body_bytes)}\r\n\r\n".encode())
        first = s.recv(1024)
        case("Expect: 100-continue", first.startswith(b"HTTP/1.1 100"), repr(first))
        s.sendall(body_bytes)
        status, _, body = read_response(s)
        case("and the request then runs", status == 200 and json.loads(body).get("id") == 8, f"{status} {body!r}")
        s.close()

        s = m.connect()
        s.sendall(post_bytes(port, rpc_body(11, "ping")) + post_bytes(port, rpc_body(12, "ping")))
        got = b""
        deadline = time.time() + 10
        while got.count(b'"result"') < 2 and time.time() < deadline:
            got += s.recv(65536)
        case("requests sent ahead are answered in order",
             0 <= got.find(b'"id":11') < got.find(b'"id":12'), repr(got[:200]))
        s.close()

        held = [m.connect() for _ in range(3)]   # with m.sock, all four slots
        for h in held:
            h.sendall(post_bytes(port, rpc_body(1, "ping")))
            read_response(h)
        spare = m.connect()
        status, _, _ = read_response(spare)
        case("a fifth connection is turned away", status == 503, str(status))
        spare.close()
        for h in held:
            h.close()
        time.sleep(0.1)
        case("the slots come back", "frame" in data(m.call("get_status")))

        waiting = m.connect()
        waiting.sendall(post_bytes(port, tool_body(21, "wait_for_stop", timeoutMs=20000)))
        time.sleep(0.2)
        d = data(m.call("pause"))
        case("another connection can pause", d.get("paused") is True, str(d))
        status, _, body = read_response(waiting)
        got = json.loads(body) if body else {}
        case("a wait is answered on its own connection",
             status == 200 and got.get("id") == 21 and data(got.get("result", {})).get("completed") is True,
             f"{status} {body!r}")
        waiting.close()

        m.call("continue")
        waiting = m.connect()
        waiting.sendall(post_bytes(port, tool_body(22, "wait_for_stop", timeoutMs=20000)))
        time.sleep(0.2)
        waiting.close()
        time.sleep(0.2)
        d = data(m.call("wait_for_stop", timeoutMs=10))
        case("a client hanging up cancels its wait", d.get("reason") == "timeout", str(d))
        case("and the machine carries on", d.get("paused") is False, str(d))

        waiting = m.connect()
        waiting.sendall(post_bytes(port, tool_body(23, "wait_for_stop", timeoutMs=20000)))
        time.sleep(0.2)
        cancel = json.dumps({"jsonrpc": "2.0", "method": "notifications/cancelled",
                             "params": {"requestId": 23}}).encode()
        status, _, _ = m.raw(post_bytes(port, cancel))
        case("a cancel over HTTP is accepted", status == 202, str(status))
        waiting.settimeout(5)
        try:
            ended = waiting.recv(1024) == b""
        except OSError:
            ended = True
        case("the cancelled request gets no response", ended)
        waiting.close()
        case("the machine is still running", data(m.call("get_status")).get("paused") is False)

        flood = m.connect()
        flood.settimeout(0.2)
        request = post_bytes(port, rpc_body(30, "tools/list"))
        sent = 0
        try:
            for _ in range(400):
                flood.sendall(request)
                sent += 1
        except OSError:
            pass
        before = data(m.call("get_status")).get("frame", 0)
        time.sleep(0.5)
        after = data(m.call("get_status")).get("frame", 0)
        case("a client that stops reading does not stall emulation", sent > 0 and after > before,
             f"sent {sent}, frames {before} to {after}")
        flood.close()

    with HttpMcp(env={"X16_MCP_IDLE_MS": "300"}) as m:
        m.init()
        s = m.connect()
        s.settimeout(5)
        try:
            closed = s.recv(16) == b""
        except OSError:
            closed = True
        case("an idle connection is closed", closed)
        s.close()
        case("a client just reconnects", "frame" in data(m.call("get_status")))

        waiting = m.connect()
        waiting.sendall(post_bytes(m.port, tool_body(40, "wait_for_stop", timeoutMs=1000)))
        status, _, body = read_response(waiting)
        case("a wait is not idle", status == 200 and data(json.loads(body)["result"]).get("reason") == "timeout",
             f"{status} {body!r}")
        waiting.close()

    with HttpMcp() as first:
        second = subprocess.Popen([EXE, "-rom", ROM, "-mcp", str(first.port), "-hidden"], cwd=first.dir.name,
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            code = second.wait(15)
        except subprocess.TimeoutExpired:
            second.kill()
            second.wait(5)
            code = None
        case("a port in use is an error", code not in (None, 0), str(code))
    for bad in ("banana", "0", "70000"):
        p = subprocess.run([EXE, "-rom", ROM, "-mcp", bad, "-hidden"], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        case(f"-mcp {bad} is refused", p.returncode != 0, str(p.returncode))


def test_lease():
    print("lease")
    lease = {"X16_MCP_LEASE_MS": "700"}

    dap_port = free_port()
    with HttpMcp(["-debugport", str(dap_port)], env=lease) as m:
        m.init()
        time.sleep(0.4)
        plant_loop(m)
        m.call("set_breakpoint", address=LOOP + 2)
        for _ in range(8):   # calling keeps it for longer than one lease
            time.sleep(0.2)
            m.call("get_status")
        case("calls renew the lease", "mcp" in owners_at(m, LOOP + 2))

        time.sleep(1.2)
        dap = Dap(dap_port)
        r = dap.request("x16/status")
        case("an expired lease resumes a headless machine",
             (r or {}).get("body", {}).get("paused") is False, str(r))
        r = dap.request("x16/listBreakpoints")
        left = [b for b in (r or {}).get("body", {}).get("breakpoints", []) if "mcp" in b.get("owners", [])]
        case("and releases its breakpoints", not left, str(left))

        dap.session()
        r = dap.request("setInstructionBreakpoints",
                        {"breakpoints": [{"instructionReference": "0x0950"}]})
        case("a DAP breakpoint goes in", (r or {}).get("body", {}).get("breakpoints", [{}])[0].get("verified") is True,
             str(r))
        m.call("pause")
        m.call("set_breakpoint", address=0x0960)
        dap.events.clear()
        time.sleep(1.2)
        r = dap.request("x16/status")
        case("with a DAP client attached, a stop is left for it",
             (r or {}).get("body", {}).get("paused") is True, str(r))
        r = dap.request("x16/listBreakpoints")
        pcs = {b.get("pc") for b in (r or {}).get("body", {}).get("breakpoints", [])}
        case("DAP breakpoints survive a lease expiring", 0x0950 in pcs, str(pcs))
        case("while MCP's went", 0x0960 not in pcs, str(pcs))
        d = data(m.call("continue"))
        case("a client can come back after expiry", d.get("paused") is False, str(d))
        dap.close()

    dap_port = free_port()
    with HttpMcp(["-debugport", str(dap_port)], env=lease) as m:
        m.init()
        m.call("get_status")
        dap = Dap(dap_port)
        dap.session("launch")
        dap.request("disconnect", {})
        dap.close()
        time.sleep(0.3)
        case("a live lease keeps a launch session's Stop from quitting", m.p.poll() is None)

        time.sleep(1.0)
        dap = Dap(dap_port)
        dap.session("launch")
        dap.request("disconnect", {})
        dap.close()
        try:
            m.p.wait(10)
            ended = True
        except subprocess.TimeoutExpired:
            ended = False
        case("with the lease gone, Stop ends a launched machine again", ended)


def png_rgb(raw):
    """Width, height and rows of RGB bytes, from an 8-bit non-interlaced PNG."""
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(raw):
        n = struct.unpack(">I", raw[pos:pos + 4])[0]
        kind, body = raw[pos + 4:pos + 8], raw[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if (depth, ctype, interlace) != (8, 2, 0):
                raise ValueError("unexpected PNG format")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    out = zlib.decompress(idat)
    stride, rows, prev, p = w * 3, [], bytearray(w * 3), 0
    for _ in range(h):
        f, line = out[p], bytearray(out[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (line[i] + pred) & 255
        rows.append(line)
        prev = line
    return w, h, rows


def image_of(result):
    """The PNG bytes in a tool result, or None."""
    for item in result.get("content", []):
        if item.get("type") == "image" and item.get("mimeType") == "image/png":
            return base64.b64decode(item["data"])
    return None


def read_byte(m, address, space="cpu"):
    return int(data(m.call("read_memory", address=address, length=1, space=space)).get("hex", "00"), 16)


def cell_matches_font(m, rows, col, row):
    """Whether one 8x8 text cell of the picture is the glyph VRAM says is there."""
    mapbase = read_byte(m, 0x9F35) << 9
    tilebase = (read_byte(m, 0x9F36) & 0xFC) << 9
    width = 32 << ((read_byte(m, 0x9F34) >> 4) & 3)
    code = read_byte(m, mapbase + 2 * (row * width + col), "vram")
    colours = {True: set(), False: set()}
    for y in range(8):
        bits = read_byte(m, tilebase + code * 8 + y, "vram")
        for x in range(8):
            start = (col * 8 + x) * 3
            colours[bool(bits & (0x80 >> x))].add(bytes(rows[row * 8 + y][start:start + 3]))
    return len(colours[True]) == 1 and len(colours[False]) == 1 and colours[True] != colours[False]


def test_vision():
    print("screen")

    port = free_port()
    with Mcp(["-debugport", str(port)], warp=False) as m:
        m.init()
        at = None
        end = time.time() + 40
        while at is None and time.time() < end:
            lines = data(m.call("get_screen")).get("lines") or []
            at = next((i for i, l in enumerate(lines) if l.startswith("READY.")), None)
            if at is None:
                time.sleep(0.25)
        case("get_screen reads the BASIC prompt", at is not None)
        if at is None:
            return

        dap = Dap(port)
        dap.session()
        m.call("pause")
        m.call("run_frames", frames=1)

        before = dap.request("x16/status", {"hash": True, "screenHash": True})["body"]
        screen = m.call("get_screen")
        shot = m.call("screenshot")
        over_dap = dap.request("x16/screenshot")
        dap_screen = dap.request("x16/screen")
        after = dap.request("x16/status", {"hash": True, "screenHash": True})["body"]
        case("looking at the screen changes no machine state", before.get("hash") == after.get("hash"))
        case("nor the picture", before.get("screenHash") == after.get("screenHash"))

        s = data(screen)
        case("the screen says what it decoded",
             s.get("layer") == 1 and s.get("charset") == "petscii-upper" and
             s.get("rows") == len(s.get("lines", [])) and s.get("columns", 0) > 0, str({k: s.get(k) for k in ("layer", "charset", "rows", "columns")}))
        case("the text content is the lines", "READY." in screen["content"][0]["text"])
        case("the same text over DAP", dap_screen.get("body", {}).get("lines") == s.get("lines"))
        case("no line ends in a space", not any(l.endswith(" ") for l in s.get("lines", [])))

        meta = data(shot)
        png = image_of(shot)
        case("screenshot is an image", png is not None)
        case("the image is not repeated in structuredContent", "data" not in meta)
        case("a frame boundary is reported", meta.get("atFrameBoundary") is True, str(meta))
        case("warp is reported", meta.get("warp") is False)
        case("the result carries the machine state", meta.get("paused") is True and "stopId" in meta)

        body = (over_dap or {}).get("body", {})
        dap_png = base64.b64decode(body.get("data", ""))
        case("x16/screenshot is the same image", dap_png == png)
        case("x16/screenshot says it is a PNG", body.get("mimeType") == "image/png")
        case("x16/screenshot reports the boundary", body.get("atFrameBoundary") is True)

        w, h, rows = png_rgb(png)
        case("the image is 640 by 480", (w, h) == (640, 480))
        case("80 columns on the boot screen", s.get("columns") == 80, str(s.get("columns")))
        for col in range(6):
            case(f"the picture shows READY. at column {col}", cell_matches_font(m, rows, col, at))

        m.call("step")
        case("a stepped machine is not at a boundary",
             data(m.call("screenshot")).get("atFrameBoundary") is False)
        m.call("run_frames", frames=1)

        case("an unknown charset is refused", is_error(m.call("get_screen", charset="ebcdic")))
        case("an unknown layer is refused", is_error(m.call("get_screen", layer=2)))
        case("a layer that is off is refused", is_error(m.call("get_screen", layer=0)))
        case("screenshot takes no arguments", is_error(m.call("screenshot", scale=2)))

        lower = data(m.call("get_screen", charset="petscii-lower")).get("lines", [])
        case("the lower-case set reads the same cells", lower[at].startswith("ready."), lower[at] if at < len(lower) else "")

        config = read_byte(m, 0x9F34)
        m.call("write_memory", address=0x9F34, hex="%02X" % (config | 1))
        case("a screen with no text layer is refused", is_error(m.call("get_screen")))
        case("so is asking for that layer", is_error(m.call("get_screen", layer=1)))
        case("a screenshot still works", image_of(m.call("screenshot")) is not None)
        m.call("write_memory", address=0x9F34, hex="%02X" % config)
        case("text is back with the layer", any(l.startswith("READY.") for l in data(m.call("get_screen")).get("lines", [])))
        dap.close()

    with Mcp() as m:
        m.init()
        time.sleep(0.5)
        case("warp is reported when on", data(m.call("screenshot")).get("warp") is True)

    for mode in ([], [str(free_port())]):
        p = subprocess.run([EXE, "-rom", ROM, "-mcp"] + mode + ["-testbench"], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=20)
        case(f"-mcp {'with a port ' if mode else ''}refuses -testbench",
             p.returncode == 1 and b"-testbench" in p.stderr, repr(p.stderr[-120:]))

    with HttpMcp() as h:
        h.init()
        time.sleep(0.5)
        png = image_of(h.call("screenshot"))
        case("a screenshot arrives whole over HTTP", png is not None and png_rgb(png)[:2] == (640, 480))


def set_vera(m, reg, *values):
    """Write VERA registers from $9F20 + reg, one byte at a time."""
    for i, v in enumerate(values):
        m.call("write_memory", address=0x9F20 + reg + i, hex="%02X" % v)


def test_vera_state():
    print("VERA state")

    port = free_port()
    with Mcp(["-debugport", str(port)]) as m:
        m.init()
        time.sleep(0.5)
        dap = Dap(port)
        dap.session()
        plant_loop(m)

        m.call("write_memory", address=0x12340, hex="AB", space="vram")
        m.call("write_memory", address=0x00080, hex="5A", space="vram")
        set_vera(m, 5, 0x00)                  # ADDRSEL 0
        set_vera(m, 0, 0x40, 0x23, 0x27)      # $12340, nibble, nibble step, code 4
        set_vera(m, 5, 0x01)                  # ADDRSEL 1
        set_vera(m, 0, 0x80, 0x00, 0x18)      # $00080, code 3
        set_vera(m, 6, 0x85)                  # IEN 5, line bit 8
        set_vera(m, 8, 0x42)

        set_vera(m, 5, 2 << 1)
        set_vera(m, 9, 0xC4, 0x42, 0x81, 0x19)
        set_vera(m, 5, 3 << 1)
        set_vera(m, 9, 0x34, 0x12, 0x00, 0x41)
        set_vera(m, 5, 4 << 1)
        set_vera(m, 9, 0x78, 0x83, 0x56, 0x02)
        set_vera(m, 5, 5 << 1)
        set_vera(m, 9, 0xAB, 0xCD)
        set_vera(m, 5, 6 << 1)
        set_vera(m, 9, 0x11, 0x22, 0x33, 0x44)
        set_vera(m, 5, 2 << 1)
        set_vera(m, 0xC, 0x59)                # accumulate
        set_vera(m, 5, (6 << 1) | 1)          # leave DCSEL and ADDRSEL somewhere else

        before = dap.request("x16/status", {"hash": True})["body"].get("hash")
        v = data(m.call("get_vera_state"))
        over_dap = dap.request("x16/vera")["body"]
        after = dap.request("x16/status", {"hash": True})["body"].get("hash")
        case("reading VERA state changes no machine state", before == after and before is not None)
        case("the same state over DAP", all(over_dap.get(k) == v.get(k) for k in over_dap), str(over_dap)[:200])

        ports = v.get("ports", [{}, {}])
        case("port 0 address", ports[0].get("address") == 0x12340, str(ports[0]))
        case("port 0 step", ports[0].get("incrementCode") == 4 and ports[0].get("increment") == 2)
        case("port 0 prefetch", ports[0].get("prefetch") == 0xAB)
        case("port 0 nibble", ports[0].get("nibble") is True and ports[0].get("nibbleIncrement") is True)
        case("port 1 address", ports[1].get("address") == 0x00080, str(ports[1]))
        case("port 1 steps backward", ports[1].get("incrementCode") == 3 and ports[1].get("increment") == -1)
        case("port 1 prefetch", ports[1].get("prefetch") == 0x5A)
        case("port 1 nibble", ports[1].get("nibble") is False and ports[1].get("nibbleIncrement") is False)
        case("selectors", v.get("addrsel") == 1 and v.get("dcsel") == 6)
        case("interrupts", v.get("ien") == 5 and v.get("irqLine") == 0x142, str((v.get("ien"), v.get("irqLine"))))
        case("the scanline is on the screen", 0 <= v.get("scanline", -1) < 525)

        fx = v.get("fx", {})
        want = {
            "addr1Mode": 0, "fourBitMode": True, "hop16": False, "cacheByteCycling": False,
            "cacheFill": False, "cacheWrite": True, "transparentWrites": True,
            "affineTileBase": 0x8000, "affineClip": True, "twoBitPoly": False,
            "affineMapBase": 0x10000, "affineMapSize": 8,
            "cacheIncrementMode": True, "cacheNibbleIndex": False, "cacheByteIndex": 2,
            "multiplier": True, "subtract": False,
            "xIncrement16_16": 0x91A00, "yIncrement16_16": -2064384,
            "xPosition16_16": 0x0378AB80, "yPosition16_16": 0x0256CD00,
            "cache": [0x11, 0x22, 0x33, 0x44], "accumulator": 0x2211 * 0x4433,
        }
        for k, expected in want.items():
            case(f"FX {k}", fx.get(k) == expected, f"{fx.get(k)!r} != {expected!r}")
        dap.close()


def test_stop_details():
    print("stop details")

    port = free_port()
    with Mcp(["-debugport", str(port)]) as m:
        m.init()
        time.sleep(0.5)
        dap = Dap(port)
        dap.session()
        plant_loop(m)
        case("no stop details before a watch fires", "stop" not in data(m.call("get_status")))

        # lda #$5A / sta $0600 / lda #$77 / sta $A000 / bra *
        m.call("write_memory", address=0x0810, hex="A95A8D0006A9778D00A080FE")
        m.call("set_registers", pc=0x0810, ramBank=3)
        m.call("set_breakpoint", address=0x0600, kind="write")
        m.call("set_breakpoint", address=0xA000, kind="write")
        last = data(m.call("get_status")).get("stopId")
        dap.events.clear()
        m.call("continue")
        r = data(m.call("wait_for_stop", sinceStopId=last))
        s = r.get("stop", {})
        case("a watch stop says so", r.get("stopReason") == "data breakpoint", str(r.get("stopReason")))
        case("which address", s.get("address") == 0x0600, str(s))
        case("what was written", s.get("value") == 0x5A)
        case("how", s.get("access") == "write" and s.get("space") == "cpu")
        case("by which instruction", s.get("accessingPc") == 0x0812, str(s.get("accessingPc")))
        case("no bank where none applies", s.get("bank") is None and s.get("accessingBank") is None)
        case("no data port", s.get("dataPort") is None)
        case("the PC has moved past it", r.get("pc") == 0x0815, str(r.get("pc")))
        ev = dap.event("stopped")
        while ev and ev["body"].get("reason") != "data breakpoint":   # the pause that set it up
            ev = dap.event("stopped")
        case("the stopped event carries it", (ev or {}).get("body", {}).get("stop") == s, str(ev)[:200])
        status = dap.request("x16/status")["body"]
        case("x16/status carries it", status.get("stop") == s)

        last = data(m.call("get_status")).get("stopId")
        m.call("continue")
        s = data(m.call("wait_for_stop", sinceStopId=last)).get("stop", {})
        case("a banked address reports its bank", s.get("address") == 0xA000 and s.get("bank") == 3, str(s))
        case("the next watch replaces the last", s.get("value") == 0x77)

        step = data(m.call("step"))
        case("a step clears it", "stop" not in step, str(step.get("stop")))
        case("and it stays clear", "stop" not in data(m.call("get_status")))
        dap.close()


def run_planted(m, code):
    """Run these opcodes from $0810 until a watch fires or they finish."""
    end = 0x0810 + len(code) // 2
    m.call("write_memory", address=0x0810, hex=code + "80FE")
    m.call("set_registers", pc=0x0810)
    m.call("set_breakpoint", address=end)
    last = data(m.call("get_status")).get("stopId")
    m.call("continue")
    r = data(m.call("wait_for_stop", sinceStopId=last))
    m.call("remove_breakpoint", address=end)
    return r


def watch_fired(r):
    return r.get("stopReason") == "data breakpoint"


def vram_hits(m, address):
    for w in data(m.call("list_breakpoints")).get("watchpoints", []):
        if w.get("space") == "vram" and w.get("addr") == address:
            return w.get("hits")
    return None


def test_vram_watch():
    print("VRAM watches")

    STORE_P0 = "A95A8D239F"          # lda #$5A / sta $9F23
    port = free_port()
    with Mcp(["-debugport", str(port)]) as m:
        m.init()
        time.sleep(0.5)
        dap = Dap(port)
        dap.session()
        plant_loop(m)

        # What gets refused.
        case("a VRAM exec breakpoint is refused",
             is_error(m.call("set_breakpoint", address=0x4000, space="vram")))
        case("past the end of VRAM is refused",
             is_error(m.call("set_breakpoint", address=0x20000, space="vram", kind="write")))
        case("a range past the end is refused",
             is_error(m.call("set_breakpoint", address=0x1FFFF, space="vram", kind="write", length=2)))
        case("VRAM has no banks",
             is_error(m.call("set_breakpoint", address=0x4000, space="vram", kind="write", bank=1)))
        case("a vram: prefix is refused, as elsewhere",
             is_error(m.call("set_breakpoint", address="vram:4000", kind="write")))
        case("an unknown space is refused",
             is_error(m.call("set_breakpoint", address=0x4000, space="rom", kind="write")))

        # A plain store through each port.
        d = data(m.call("set_breakpoint", address=0x04000, space="vram", kind="write"))
        case("a VRAM watch is set", d.get("space") == "vram" and d.get("address") == "$04000", str(d))
        set_vera(m, 0x05, 0)
        set_vera(m, 0, 0x00, 0x40, 0x10)
        r = run_planted(m, STORE_P0)
        s = r.get("stop", {})
        case("a store through port 0 fires", watch_fired(r), str(r.get("stopReason")))
        case("it names VRAM", s.get("space") == "vram" and s.get("access") == "write", str(s))
        case("and the address", s.get("address") == 0x04000, str(s))
        case("and the value", s.get("value") == 0x5A)
        case("and the port", s.get("dataPort") == 0)
        case("and the instruction", s.get("accessingPc") == 0x0812, str(s.get("accessingPc")))
        case("VRAM has no bank to report", s.get("bank") is None)
        case("the byte is there", read_byte(m, 0x04000, "vram") == 0x5A)
        case("a hit is counted", vram_hits(m, 0x04000) == 1, str(vram_hits(m, 0x04000)))

        set_vera(m, 0x05, 1)
        set_vera(m, 0, 0x00, 0x40, 0x10)
        set_vera(m, 0x05, 0)
        r = run_planted(m, "A9338D249F")     # lda #$33 / sta $9F24
        s = r.get("stop", {})
        case("a store through port 1 fires", watch_fired(r) and s.get("dataPort") == 1, str(s))
        case("with its value", s.get("value") == 0x33)

        # Writes that must not fire.
        before = vram_hits(m, 0x04000)
        set_vera(m, 0, 0x00, 0x40, 0x00)
        m.call("write_memory", address=0x9F23, hex="77")
        m.call("write_memory", address=0x04000, space="vram", hex="55")
        case("a debugger write does not count", vram_hits(m, 0x04000) == before)
        set_vera(m, 0, 0x01, 0x40, 0x10)
        r = run_planted(m, STORE_P0)
        case("the next byte does not fire", not watch_fired(r), str(r.get("stopReason")))
        r = run_planted(m, "A95A8D0040")     # sta $4000
        case("a CPU store to the same number does not fire", not watch_fired(r), str(r.get("stopReason")))
        m.call("set_breakpoint", address=0x4000, kind="write")
        set_vera(m, 0, 0x00, 0x40, 0x10)
        r = run_planted(m, STORE_P0)
        case("a VRAM store is not a CPU watch either", r.get("stop", {}).get("space") == "vram", str(r.get("stop")))
        r = run_planted(m, "A95A8D0040")
        case("and the CPU watch still works", r.get("stop", {}).get("space") == "cpu", str(r.get("stop")))
        d = data(m.call("remove_breakpoint", address=0x4000, kind="write"))
        case("removing the CPU watch leaves the VRAM one", d.get("stillArmed") is False and
             vram_hits(m, 0x04000) is not None, str(d))

        d = data(m.call("remove_breakpoint", address=0x04000, space="vram", kind="write"))
        case("a VRAM watch comes off", d.get("removed") is True and d.get("stillArmed") is False, str(d))
        set_vera(m, 0, 0x00, 0x40, 0x10)
        r = run_planted(m, STORE_P0)
        case("and no longer fires", not watch_fired(r), str(r.get("stopReason")))

        # Range and value filter.
        m.call("set_breakpoint", address=0x05000, space="vram", kind="write", length=4)
        set_vera(m, 0, 0x03, 0x50, 0x10)
        r = run_planted(m, STORE_P0)
        case("a range fires on its last byte", r.get("stop", {}).get("address") == 0x05003, str(r.get("stop")))
        set_vera(m, 0, 0x04, 0x50, 0x10)
        r = run_planted(m, STORE_P0)
        case("and not past it", not watch_fired(r), str(r.get("stopReason")))
        m.call("remove_breakpoint", address=0x05000, space="vram", kind="write")

        m.call("set_breakpoint", address=0x05100, space="vram", kind="write", condition="== $5A")
        set_vera(m, 0, 0x00, 0x51, 0x00)
        r = run_planted(m, "A9128D239F")
        case("a value filter holds off", not watch_fired(r), str(r.get("stopReason")))
        r = run_planted(m, STORE_P0)
        case("and fires on the value", watch_fired(r), str(r.get("stopReason")))
        m.call("remove_breakpoint", address=0x05100, space="vram", kind="write")

        # FX: cache writes, masks and transparency. DCSEL 2 holds FX_CTRL, 6 the cache.
        set_vera(m, 0x05, 6 << 1)
        set_vera(m, 0x09, 0x11, 0x22, 0x33, 0x44)
        set_vera(m, 0x05, 2 << 1)
        set_vera(m, 0x09, 0x40)
        m.call("set_breakpoint", address=0x06001, space="vram", kind="write")
        m.call("set_breakpoint", address=0x06003, space="vram", kind="write")
        set_vera(m, 0, 0x00, 0x60, 0x00)
        r = run_planted(m, "A9008D239F")     # mask 0: all four bytes
        s = r.get("stop", {})
        case("an FX cache write fires", watch_fired(r), str(r.get("stopReason")))
        case("on the first watched byte", s.get("address") == 0x06001 and s.get("value") == 0x22, str(s))
        case("all four bytes still landed", read_byte(m, 0x06003, "vram") == 0x44)
        set_vera(m, 0, 0x00, 0x60, 0x00)
        r = run_planted(m, "A9CC8D239F")     # mask 3 on bytes 1 and 3
        case("a masked byte does not fire", not watch_fired(r), str(r.get("stop")))
        m.call("remove_breakpoint", address=0x06001, space="vram", kind="write")
        m.call("remove_breakpoint", address=0x06003, space="vram", kind="write")

        set_vera(m, 0x09, 0x80)              # transparent writes
        m.call("set_breakpoint", address=0x06100, space="vram", kind="write")
        set_vera(m, 0, 0x00, 0x61, 0x00)
        r = run_planted(m, "A9008D239F")
        case("a transparent zero does not fire", not watch_fired(r), str(r.get("stop")))
        r = run_planted(m, STORE_P0)
        case("a non-zero byte does", watch_fired(r), str(r.get("stopReason")))
        m.call("remove_breakpoint", address=0x06100, space="vram", kind="write")
        m.call("set_breakpoint", address=0x1F9C0, space="vram", kind="write")
        set_vera(m, 0, 0xC0, 0xF9, 0x01)
        r = run_planted(m, "A9008D239F")
        case("a transparent zero still reaching the PSG fires", watch_fired(r) and
             r.get("stop", {}).get("value") == 0, str(r.get("stop")))
        m.call("remove_breakpoint", address=0x1F9C0, space="vram", kind="write")

        # The 2-bit poke is a separate path through the port write.
        set_vera(m, 0x09, 0x00)
        set_vera(m, 0x05, 0x01)
        set_vera(m, 0, 0x00, 0x62, 0x00)
        set_vera(m, 0x05, (2 << 1) | 1)
        set_vera(m, 0x09, 0x06, 0x01)
        m.call("set_breakpoint", address=0x06201, space="vram", kind="write")
        r = run_planted(m, "A9018D209FA9008D249F")   # sta $9F20 picks the byte, sta $9F24 pokes
        s = r.get("stop", {})
        case("a 2-bit poke fires", watch_fired(r) and s.get("address") == 0x06201, str(s))
        case("through port 1", s.get("dataPort") == 1, str(s))
        m.call("remove_breakpoint", address=0x06201, space="vram", kind="write")
        set_vera(m, 0x09, 0x00, 0x00)
        set_vera(m, 0x05, 0)

        # DAP sees the same watch.
        info = dap.request("dataBreakpointInfo", {"name": "vram:07000:2"})["body"]
        case("DAP offers a VRAM range", info.get("dataId") == "vram:07000:2", str(info))
        case("a bad VRAM name has no dataId",
             dap.request("dataBreakpointInfo", {"name": "vram:20000"})["body"].get("dataId") is None)
        set_r = dap.request("setDataBreakpoints", {"breakpoints": [{"dataId": info["dataId"]}]})
        rows = set_r["body"]["breakpoints"]
        case("DAP arms it", rows and rows[0].get("verified") is True, str(rows))
        dap.events.clear()
        set_vera(m, 0, 0x01, 0x70, 0x10)
        r = run_planted(m, STORE_P0)
        case("it fires", watch_fired(r) and r.get("stop", {}).get("address") == 0x07001, str(r.get("stop")))
        ev = dap.event("stopped")
        while ev and ev["body"].get("reason") != "data breakpoint":
            ev = dap.event("stopped")
        case("the stopped event names the breakpoint",
             (ev or {}).get("body", {}).get("hitBreakpointIds") == [rows[0].get("id")], str(ev)[:200])
        case("and the VRAM access", (ev or {}).get("body", {}).get("stop", {}).get("space") == "vram")
        dap.request("setDataBreakpoints", {"breakpoints": []})
        wps = data(m.call("list_breakpoints")).get("watchpoints", [])
        case("clearing DAP's list takes it off", not wps, str(wps))
        dap.close()


def watch_hits(m, address):
    for w in data(m.call("list_breakpoints")).get("watchpoints", []):
        if w.get("space") == "cpu" and w.get("addr") == address:
            return w.get("hits")
    return None


def test_read_watch():
    print("read watches")

    LOAD = "AD0006"           # lda $0600
    STORE = "A95A8D0006"      # lda #$5A / sta $0600
    port = free_port()
    with Mcp(["-debugport", str(port)]) as m:
        m.init()
        time.sleep(0.5)
        dap = Dap(port)
        dap.session()
        plant_loop(m)
        m.call("write_memory", address=0x0600, hex="A5")

        d = data(m.call("set_breakpoint", address=0x0600, kind="read"))
        case("a read watch is set", d.get("kind") == "read" and d.get("space") == "cpu", str(d))
        wps = data(m.call("list_breakpoints")).get("watchpoints", [])
        case("listed as read, fetches excluded",
             len(wps) == 1 and wps[0].get("access") == "read" and wps[0].get("excludeFetch") is True, str(wps))
        r = run_planted(m, LOAD)
        s = r.get("stop", {})
        case("a load fires it", watch_fired(r), str(r.get("stopReason")))
        case("it says read", s.get("access") == "read" and s.get("space") == "cpu", str(s))
        case("and not a fetch", s.get("fetch") is False, str(s))
        case("with the address and value", s.get("address") == 0x0600 and s.get("value") == 0xA5, str(s))
        case("by that instruction", s.get("accessingPc") == 0x0810, str(s.get("accessingPc")))
        case("the instruction finished", r.get("pc") == 0x0813 and
             data(m.call("get_registers"))["cpu"].get("a") == 0xA5, str(r.get("pc")))

        r = run_planted(m, STORE)
        case("a store does not fire a read watch", not watch_fired(r), str(r.get("stop")))
        before = watch_hits(m, 0x0600)
        m.call("read_memory", address=0x0600, count=4)
        m.call("disassemble", address=0x0600, count=2)
        case("a debugger read does not count", watch_hits(m, 0x0600) == before)
        case("a watch of another kind stays put",
             is_error(m.call("remove_breakpoint", address=0x0600, kind="write")))
        case("a different kind at the same address is refused",
             is_error(m.call("set_breakpoint", address=0x0600, kind="write")))
        m.call("remove_breakpoint", address=0x0600, kind="read")
        r = run_planted(m, LOAD)
        case("removed, it no longer fires", not watch_fired(r), str(r.get("stopReason")))

        # A write watch is untouched by reads.
        m.call("set_breakpoint", address=0x0600, kind="write")
        r = run_planted(m, LOAD)
        case("a load does not fire a write watch", not watch_fired(r), str(r.get("stopReason")))
        m.call("remove_breakpoint", address=0x0600, kind="write")

        # Read-write catches both, and a read-modify-write stops on its read.
        m.call("set_breakpoint", address=0x0600, kind="access")
        r = run_planted(m, LOAD)
        case("access fires on a load", r.get("stop", {}).get("access") == "read", str(r.get("stop")))
        r = run_planted(m, STORE)
        case("and on a store", r.get("stop", {}).get("access") == "write", str(r.get("stop")))
        m.call("write_memory", address=0x0600, hex="10")
        r = run_planted(m, "EE0006")     # inc $0600
        case("a read-modify-write stops on its read",
             r.get("stop", {}).get("access") == "read" and r.get("stop", {}).get("value") == 0x10, str(r.get("stop")))
        case("and still completes", read_byte(m, 0x0600) == 0x11)
        m.call("remove_breakpoint", address=0x0600, kind="access")

        # Fetches. $0811 is the operand of the first instruction.
        m.call("set_breakpoint", address=0x0811, kind="read")
        r = run_planted(m, "A900")       # lda #$00: its operand is the watched byte
        case("an immediate operand is a fetch", not watch_fired(r), str(r.get("stop")))
        r = run_planted(m, "AD0006")     # its operand byte is fetched as an address
        case("an address operand is a fetch", not watch_fired(r), str(r.get("stop")))
        r = run_planted(m, "AD1108")     # lda $0811 reads its own operand as data
        s = r.get("stop", {})
        case("code reading its own operand is a data read", watch_fired(r) and s.get("fetch") is False, str(s))
        case("with the operand's value", s.get("value") == 0x11, str(s))
        m.call("remove_breakpoint", address=0x0811, kind="read")
        m.call("set_breakpoint", address=0x0811, kind="read", excludeFetch=False)
        r = run_planted(m, "A900")
        s = r.get("stop", {})
        case("with fetches included, it fires", watch_fired(r) and s.get("fetch") is True, str(s))
        m.call("remove_breakpoint", address=0x0811, kind="read")

        # Value filter.
        m.call("write_memory", address=0x0600, hex="12")
        m.call("set_breakpoint", address=0x0600, kind="read", condition="== $5A")
        r = run_planted(m, LOAD)
        case("a value filter holds off", not watch_fired(r), str(r.get("stopReason")))
        m.call("write_memory", address=0x0600, hex="5A")
        r = run_planted(m, LOAD)
        case("and fires on the value", watch_fired(r), str(r.get("stopReason")))
        m.call("remove_breakpoint", address=0x0600, kind="read")

        # What gets refused.
        case("VRAM read is refused",
             is_error(m.call("set_breakpoint", address=0x4000, space="vram", kind="read")))
        case("excludeFetch needs a read kind",
             is_error(m.call("set_breakpoint", address=0x0600, kind="write", excludeFetch=True)))
        case("an unknown kind is refused",
             is_error(m.call("set_breakpoint", address=0x0600, kind="touch")))

        # DAP.
        info = dap.request("dataBreakpointInfo", {"name": "$0600"})["body"]
        case("DAP offers read and readWrite",
             set(info.get("accessTypes", [])) == {"read", "write", "readWrite"}, str(info))
        vinfo = dap.request("dataBreakpointInfo", {"name": "vram:04000"})["body"]
        case("VRAM offers write only", vinfo.get("accessTypes") == ["write"], str(vinfo))
        rows = dap.request("setDataBreakpoints", {"breakpoints": [
            {"dataId": info["dataId"], "accessType": "read"}]})["body"]["breakpoints"]
        case("DAP arms a read watch", rows and rows[0].get("verified") is True, str(rows))
        dap.events.clear()
        r = run_planted(m, LOAD)
        case("it fires", r.get("stop", {}).get("access") == "read", str(r.get("stop")))
        ev = dap.event("stopped")
        while ev and ev["body"].get("reason") != "data breakpoint":
            ev = dap.event("stopped")
        case("the stopped event names it",
             (ev or {}).get("body", {}).get("hitBreakpointIds") == [rows[0].get("id")], str(ev)[:200])
        dap.request("setDataBreakpoints", {"breakpoints": []})
        dap.close()


def test_symbols():
    print("symbols")

    labels = ("al 000810 .start\n"
              "al 000600 .fade\n"
              "al 00A000 .banked\n"
              "al 000700 .twice\n"
              "al 000710 .twice\n"
              "al 000900 .__CODE_RUN__\n")
    other = "al 000620 .moved\n"
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "prog.lbl")
        with open(path, "w") as f:
            f.write(labels)
        empty = os.path.join(tmp, "empty.lbl")
        with open(empty, "w") as f:
            f.write("nothing useful here\n")

        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", tmp]) as m:
            m.init()
            time.sleep(0.5)
            plant_loop(m)

            case("a name means nothing before a load", is_error(m.call("read_memory", address="fade")))
            case("a missing file is refused",
                 is_error(m.call("load_symbols", path=os.path.join(tmp, "nope.lbl"))))
            case("a file with no labels is refused", is_error(m.call("load_symbols", path=empty)))

            d = data(m.call("load_symbols", path=path, bank=3))
            case("labels load", d.get("labels") == 5 and d.get("bank") == 3, str(d))
            d = data(m.call("list_symbols"))
            names = [s["name"] for s in d.get("symbols", [])]
            case("listed in address order, segment symbols dropped",
                 names == ["fade", "twice", "twice", "start", "banked"], str(names))
            d = data(m.call("list_symbols", filter="AN"))
            case("a filter narrows it", [s["name"] for s in d.get("symbols", [])] == ["banked"], str(d))
            d = data(m.call("list_symbols", start=1, count=2))
            case("paged", len(d.get("symbols", [])) == 2 and d.get("total") == 5, str(d))

            d = data(m.call("lookup_symbol", name="FADE"))
            hit = (d.get("matches") or [{}])[0]
            case("a name is found, whatever its case", hit.get("address") == 0x0600 and hit.get("bank") is None, str(d))
            d = data(m.call("lookup_symbol", name="banked"))
            case("with its bank", (d.get("matches") or [{}])[0].get("bank") == 3, str(d))
            d = data(m.call("lookup_symbol", name="twice"))
            case("every place a name resolves to", len(d.get("matches", [])) == 2, str(d))
            d = data(m.call("lookup_symbol", name="start+4"))
            case("an expression", (d.get("matches") or [{}])[0].get("address") == 0x0814, str(d))
            d = data(m.call("lookup_symbol", address=0x0814))
            case("an address finds its label", d.get("name") == "start" and d.get("offset") == 4, str(d))
            case("neither is refused", is_error(m.call("lookup_symbol")))
            case("both are refused", is_error(m.call("lookup_symbol", name="start", address=1)))

            # A hex-looking word is the label, and $ is the number.
            m.call("write_memory", address=0x0600, hex="5A")
            d = data(m.call("read_memory", address="fade", length=1))
            case("fade is the label", d.get("address") == "0x0600" and d.get("hex") == "5A", str(d))
            d = data(m.call("read_memory", address="$fade", length=1))
            case("$fade is the number", d.get("address") == "0xFADE", str(d))
            d = data(m.call("read_memory", address="fade-$10", length=1))
            case("an offset applies", d.get("address") == "0x05F0", str(d))
            r = m.call("read_memory", address="beef")
            case("an unknown hex word says how to write a number",
                 is_error(r) and "$beef" in json.dumps(r), json.dumps(r)[:160])
            r = m.call("read_memory", address="twice")
            case("an ambiguous name is refused and lists where",
                 is_error(r) and "$0700" in json.dumps(r) and "$0710" in json.dumps(r), json.dumps(r)[:200])
            case("VRAM still reads bare hex",
                 not is_error(m.call("read_memory", address="1B000", length=1, space="vram")))

            # Banked symbols stay in their bank.
            m.call("set_registers", ramBank=1)
            case("a write to an unmapped bank is refused",
                 is_error(m.call("write_memory", address="banked", hex="01")))
            case("so is disassembling one", is_error(m.call("disassemble", address="banked")))
            m.call("set_registers", ramBank=3)
            case("mapped, it works", not is_error(m.call("write_memory", address="banked", hex="42")))
            m.call("set_registers", ramBank=1)
            m.call("write_memory", address=0xA000, hex="11")
            case("a read by name goes to the symbol's bank, not the mapped one",
                 data(m.call("read_memory", address="banked", length=1)).get("hex") == "42")
            case("while the number reads the mapped bank",
                 data(m.call("read_memory", address=0xA000, length=1)).get("hex") == "11")
            m.call("set_breakpoint", address="banked")
            bps = data(m.call("list_breakpoints")).get("breakpoints", [])
            case("an exec breakpoint is pinned to it",
                 any(b.get("pc", b.get("addr")) == 0xA000 and b.get("x16Bank") == 3 for b in bps), str(bps))
            m.call("remove_breakpoint", address="banked")
            case("a watch on a banked symbol is refused",
                 is_error(m.call("set_breakpoint", address="banked", kind="write")))

            # Breakpoints by name fire.
            m.call("write_memory", address=0x0810, hex="EAEA80FE")
            m.call("set_registers", pc=0x0810)
            m.call("set_breakpoint", address="start+1")
            last = data(m.call("get_status")).get("stopId")
            m.call("continue")
            r = data(m.call("wait_for_stop", sinceStopId=last))
            case("a breakpoint by name stops there", r.get("pc") == 0x0811, str(r.get("pc")))
            m.call("remove_breakpoint", address="start+1")

            # Disassembly picks up the names without a .dbg.
            r = m.call("disassemble", address=0x0810, count=2)
            rows = data(r).get("instructions", [])
            case("disassembly names the labelled row",
                 len(rows) == 2 and rows[0].get("symbol") == "start" and "symbol" not in rows[1], str(rows)[:240])
            case("and its text shows it", "start:" in json.dumps(r.get("content", "")), str(r.get("content"))[:200])
            m.call("set_registers", ramBank=1)
            rows = data(m.call("disassemble", address=0xA000, count=1)).get("instructions", [])
            case("another bank's label is not shown", rows and "symbol" not in rows[0], str(rows)[:200])
            m.call("set_registers", ramBank=3)
            rows = data(m.call("disassemble", address=0xA000, count=1)).get("instructions", [])
            case("its own bank's is", rows and rows[0].get("symbol") == "banked", str(rows)[:200])
            m.call("set_registers", ramBank=1)

            with open(path, "w") as f:
                f.write(other)
            d = data(m.call("load_symbols", path=path))
            case("loading again replaces that file's labels", d.get("labels") == 1, str(d))
            case("the old names are gone", is_error(m.call("read_memory", address="fade")))
            case("and the new one works", not is_error(m.call("read_memory", address="moved")))

            dap = Dap(port)
            dap.session()
            with open(path, "w") as f:
                f.write(labels)
            r = dap.request("x16/loadSymbols", {"path": path})
            case("DAP loads labels", r.get("success") and r["body"].get("labels") == 5, str(r)[:200])
            r = dap.request("readMemory", {"memoryReference": "sym:fade", "count": 1})
            case("DAP reads through sym:", r.get("success") and r["body"].get("address") in ("0x600", "0x0600"), str(r)[:200])
            r = dap.request("readMemory", {"memoryReference": "fade", "count": 1})
            case("a bare token stays a number or an error in DAP",
                 not (r.get("success") and r["body"].get("address") in ("0x600", "0x0600")), str(r)[:200])
            r = dap.request("x16/lookupSymbol", {"name": "start"})
            case("DAP looks up a name", r.get("success") and r["body"]["matches"][0]["address"] == 0x0810, str(r)[:200])
            r = dap.request("x16/listSymbols", {"filter": "fade"})
            case("DAP lists", r.get("success") and r["body"].get("total") == 1, str(r)[:200])
            dap.close()


def wait_basic(m, secs=20):
    end = time.time() + secs
    while time.time() < end:
        if any(l.startswith("READY.") for l in data(m.call("get_screen")).get("lines", [])):
            return True
        time.sleep(0.1)
    return False


def mem_bytes(m, **where):
    return bytes.fromhex(data(m.call("read_memory", **where)).get("hex", "").replace(" ", ""))


def test_hash_memory():
    print("hash_memory")
    with Mcp() as m:
        m.init()
        time.sleep(0.5)
        plant_loop(m)
        pattern = bytes((i * 7 + 3) & 0xFF for i in range(200))
        m.call("write_memory", address=0x0A00, hex=pattern.hex())
        # Around the SHA-256 padding edges, and past one block.
        for n in (1, 55, 56, 57, 63, 64, 65, 119, 120, 200):
            d = data(m.call("hash_memory", address=0x0A00, length=n))
            want = mem_bytes(m, address=0x0A00, length=n)
            case(f"{n} bytes hash as the host does",
                 len(want) == n and d.get("crc32") == "%08x" % zlib.crc32(want) and
                 d.get("sha256") == hashlib.sha256(want).hexdigest(), str(d))
        d = data(m.call("hash_memory", address="1B000", length=64, space="vram"))
        want = mem_bytes(m, address="1B000", length=64, space="vram")
        case("VRAM hashes too", d.get("sha256") == hashlib.sha256(want).hexdigest() and d.get("space") == "vram", str(d))
        d = data(m.call("hash_memory", address=0xC000, length=32, bank=4))
        want = mem_bytes(m, address=0xC000, length=32, bank=4)
        case("a chosen bank", d.get("sha256") == hashlib.sha256(want).hexdigest(), str(d))

        # No side effects, on the register that has the most of them.
        set_vera(m, 0x9F20, 0x00, 0x00, 0x10)
        before = data(m.call("get_vera_state"))
        m.call("set_breakpoint", address=0x9F23, kind="read")
        stop = data(m.call("get_status")).get("stopId")
        m.call("hash_memory", address=0x9F20, length=16)
        after = data(m.call("get_vera_state"))
        case("hashing I/O leaves VERA alone", before == after)
        case("and fires no read watch", data(m.call("get_status")).get("stopId") == stop)
        m.call("remove_breakpoint", address=0x9F23)

        case("a length is required", is_error(m.call("hash_memory", address=0x0A00)))
        case("past the end is refused", is_error(m.call("hash_memory", address="1FFF0", length=32, space="vram")))
        case("too long is refused", is_error(m.call("hash_memory", address=0, length=0x100001)))


def test_dump_memory():
    print("dump_memory")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        root = os.path.join(tmp, "root")
        outside = root + "2"            # shares the prefix on purpose
        os.mkdir(root)
        os.mkdir(outside)
        os.mkdir(os.path.join(root, "sub"))
        with Mcp(["-fsroot", root]) as m:
            m.init()
            time.sleep(0.5)
            plant_loop(m)
            body = bytes(range(64))
            m.call("write_memory", address=0x0A00, hex=body.hex())

            d = data(m.call("dump_memory", address=0x0A00, length=64, path="out.bin"))
            target = os.path.join(root, "out.bin")
            case("a plain name lands in the root", os.path.isfile(target) and open(target, "rb").read() == body, str(d))
            case("and the reply says where", os.path.normcase(d.get("path", "").replace("/", os.sep)) == os.path.normcase(target), str(d))
            m.call("write_memory", address=0x0A00, hex="FF")
            r = m.call("dump_memory", address=0x0A00, length=64, path="out.bin")
            case("an existing file is refused", is_error(r) and open(target, "rb").read() == body)
            r = m.call("dump_memory", address=0x0A00, length=64, path="out.bin", overwrite=True)
            case("unless overwrite is given", not is_error(r) and open(target, "rb").read()[0] == 0xFF)
            m.call("dump_memory", address=0x0A00, length=4, path=os.path.join("sub", "x.bin"))
            case("a folder inside is fine", os.path.isfile(os.path.join(root, "sub", "x.bin")))
            m.call("dump_memory", address=0x0A00, length=4, path=os.path.join(root, "abs.bin"))
            case("an absolute path inside is fine", os.path.isfile(os.path.join(root, "abs.bin")))

            for label, bad in (("..", os.path.join("..", "esc.bin")),
                               ("a nested ..", os.path.join("sub", "..", "..", "esc.bin")),
                               ("an absolute path outside", os.path.join(tmp, "esc.bin")),
                               ("a folder sharing the prefix", os.path.join(outside, "esc.bin")),
                               ("a missing folder", os.path.join("nope", "x.bin")),
                               ("the root itself", "."),
                               ("a trailing separator", "sub" + os.sep)):
                case(f"{label} is refused", is_error(m.call("dump_memory", address=0x0A00, length=4, path=bad)))
            case("nothing escaped", not os.path.exists(os.path.join(tmp, "esc.bin")) and not os.listdir(outside))

            if os.name == "nt":
                for bad in ("NUL", "con.txt", "out.bin:hidden", "C:relative.bin", "\\\\server\\share\\x.bin"):
                    case(f"{bad} is refused", is_error(m.call("dump_memory", address=0x0A00, length=4, path=bad)))
            else:
                os.symlink(tmp, os.path.join(root, "up"))
                os.symlink(os.path.join(tmp, "dangling.bin"), os.path.join(root, "link.bin"))
                case("a folder link out is refused",
                     is_error(m.call("dump_memory", address=0x0A00, length=4, path=os.path.join("up", "esc.bin"))))
                case("a file link is refused, even dangling",
                     is_error(m.call("dump_memory", address=0x0A00, length=4, path="link.bin", overwrite=True)) and
                     not os.path.exists(os.path.join(tmp, "dangling.bin")))

    drive = os.path.abspath(os.sep)
    with Mcp(["-fsroot", drive]) as m:
        m.init()
        time.sleep(0.5)
        case("a drive root is refused", is_error(m.call("dump_memory", address=0x0A00, length=4, path="x16-dump-test.bin")))
        case("and writes nothing", not os.path.exists(os.path.join(drive, "x16-dump-test.bin")))


def warp_on(m):
    return json.dumps(data(m.call("perf_stats"))).find('"warpMode": true') >= 0


def test_warp():
    print("set_warp")
    with Mcp(["-pastewarp"], warp=False) as m:
        m.init()
        case("basic is up", wait_basic(m, 40))
        case("starts off", not warp_on(m))
        case("on", data(m.call("set_warp", enabled=True)).get("enabled") is True and warp_on(m))
        case("on again stays on", data(m.call("set_warp", enabled=True)).get("enabled") is True and warp_on(m))
        m.call("type_text", text="A=1\r")
        time.sleep(1.0)
        case("the end of a paste leaves a client's warp on", warp_on(m))
        case("off", data(m.call("set_warp", enabled=False)).get("enabled") is False and not warp_on(m))
        m.call("type_text", text="A=2\r")
        time.sleep(2.0)
        case("off stays off after a paste", not warp_on(m))
        case("enabled is required", is_error(m.call("set_warp")))
        case("and must be a boolean", is_error(m.call("set_warp", enabled=1)))


def test_mouse():
    print("mouse")
    with Mcp() as m:
        m.init()
        case("basic is up", wait_basic(m))
        d = data(m.call("mouse"))
        case("reports with the KERNAL mouse off", d.get("kernalMouse") is False and d.get("buttons") == [], str(d))
        m.call("type_text", text="MOUSE 1\r")
        time.sleep(0.6)
        sample = "POKE1024,MX/4:POKE1025,MY/4:POKE1026,MB\r"
        m.call("type_text", text=sample)
        time.sleep(0.6)
        x0, y0, b0 = mem_bytes(m, address=0x0400, length=3)
        d = data(m.call("mouse", dx=80, dy=40, buttons=["left"]))
        case("the reply has the state", d.get("buttons") == ["left"] and d.get("kernalMouse") is True, str(d))
        time.sleep(0.4)
        m.call("type_text", text=sample)
        time.sleep(0.6)
        x1, y1, b1 = mem_bytes(m, address=0x0400, length=3)
        case("the program sees it move right", x1 > x0, f"{x0} -> {x1}")
        case("and down", y1 > y0, f"{y0} -> {y1}")
        case("and the button", b0 == 0 and b1 == 1, f"{b0} -> {b1}")
        d = data(m.call("mouse", dx=1))
        case("a move alone keeps the buttons", d.get("buttons") == ["left"], str(d))
        m.call("mouse", buttons=[])
        time.sleep(0.4)
        m.call("type_text", text=sample)
        time.sleep(0.6)
        case("an empty list releases", mem_bytes(m, address=0x0402, length=1)[0] == 0)
        case("a big move is refused", is_error(m.call("mouse", dx=300)))
        case("an unknown button is refused", is_error(m.call("mouse", buttons=["fourth"])))


K_PRG_DBG = (
    'version\tmajor=2,minor=0\n'
    'info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=1,type=0\n'
    'file\tid=0,name="prog.s",size=10,mtime=0x00000000,mod=0\n'
    'seg\tid=0,name="CODE",start=0x000A00,size=0x0010,addrsize=absolute,type=ro,oname="prog.prg"\n'
    'span\tid=0,seg=0,start=0,size=16,type=0\n'
    'line\tid=0,file=0,line=1,span=0\n'
    'sym\tid=0,name="_hello",addrsize=absolute,size=3,scope=0,def=0,val=0x000A00,type=lab\n'
    'mod\tid=0,name="prog.o",file=0\n')


def test_load_prg():
    print("load_prg")
    with tempfile.TemporaryDirectory() as tmp:
        def prg(name, at, body):
            p = os.path.join(tmp, name)
            with open(p, "wb") as f:
                f.write(struct.pack("<H", at) + body)
            return p
        body = bytes(0x40 + i for i in range(16))
        path = prg("prog.prg", 0x0A00, body)
        with open(os.path.join(tmp, "prog.dbg"), "w") as f:
            f.write(K_PRG_DBG)
        plain = prg("plain.prg", 0x0B00, b"\x11\x22\x33")

        with Mcp(["-fsroot", tmp]) as m:
            m.init()
            time.sleep(0.5)
            case("refused while running", is_error(m.call("load_prg", path=path)))
            plant_loop(m)
            before = data(m.call("get_registers"))["cpu"]["pc"]
            d = data(m.call("load_prg", path=path))
            case("loads at its address", d.get("address") == "0x0A00" and d.get("length") == 16, str(d))
            case("the bytes land", mem_bytes(m, address=0x0A00, length=16) == body)
            case("its debug info came with it", d.get("debugInfo") is True, str(d))
            r = data(m.call("lookup_symbol", name="_hello"))
            case("so the name resolves", (r.get("matches") or [{}])[0].get("address") == 0x0A00, str(r))
            case("the machine has not moved", data(m.call("get_registers"))["cpu"]["pc"] == before)

            d = data(m.call("load_prg", path=plain))
            case("a program with no .dbg still loads",
                 d.get("debugInfo") is False and mem_bytes(m, address=0x0B00, length=3) == b"\x11\x22\x33", str(d))
            d = data(m.call("load_prg", path=plain, address="$0C00"))
            case("an address overrides the header",
                 d.get("address") == "0x0C00" and mem_bytes(m, address=0x0C00, length=3) == b"\x11\x22\x33", str(d))

            banked = prg("banked.prg", 0xA000, b"\x5A\xA5")
            m.call("set_registers", ramBank=2)
            d = data(m.call("load_prg", path=banked))
            case("the banked window, in the mapped bank", d.get("bank") == 2, str(d))
            case("lands there", mem_bytes(m, address=0xA000, length=2, bank=2) == b"\x5A\xA5")
            m.call("set_registers", ramBank=1)

            io_before = data(m.call("get_vera_state"))
            for label, p in (("over I/O", prg("io.prg", 0x9EFF, b"\x01\x02\x03")),
                             ("in I/O", prg("io2.prg", 0x9F20, b"\x01")),
                             ("in ROM", prg("rom.prg", 0xC000, b"\x01")),
                             ("past the bank window", prg("wrap.prg", 0xBFFF, b"\x01\x02")),
                             ("over the bank registers", prg("zp.prg", 0x0000, b"\x01"))):
                case(f"{label} is refused", is_error(m.call("load_prg", path=p)))
            case("and VERA was not touched", data(m.call("get_vera_state")) == io_before)
            short = os.path.join(tmp, "short.prg")
            with open(short, "wb") as f:
                f.write(b"\x00")
            case("a file too short is refused", is_error(m.call("load_prg", path=short)))
            case("a missing file is refused", is_error(m.call("load_prg", path=os.path.join(tmp, "no.prg"))))


def test_client_format():
    print("a DAP client's line and path format")
    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "prog.s").replace("\\", "/")
        with open(source, "w") as f:
            f.write("; one\n; two\n  nop\n")
        path = os.path.join(tmp, "prog.prg")
        with open(path, "wb") as f:
            f.write(struct.pack("<H", 0x0A00) + b"\xEA" * 16)
        with open(os.path.join(tmp, "prog.dbg"), "w") as f:
            f.write(K_PRG_DBG.replace('name="prog.s"', f'name="{source}"')
                    .replace("line=1,span=0", "line=3,span=0"))
        uri = "file:///" + source.lstrip("/")

        def begin(dap, editor):
            dap.request("initialize", {"adapterID": "test", "linesStartAt1": not editor,
                                       "columnsStartAt1": not editor,
                                       "pathFormat": "uri" if editor else "path"})
            dap.request("attach", {})
            dap.request("configurationDone")
            r = dap.request("setBreakpoints", {"source": {"path": uri if editor else source},
                                               "breakpoints": [{"line": 2 if editor else 3}]})
            return r["body"]["breakpoints"][0]

        def views(m):
            listed = [b for b in data(m.call("list_breakpoints")).get("breakpoints", [])
                      if "dapId" in b]
            row = data(m.call("disassemble", address=0x0A00, count=1)).get("instructions", [{}])[0]
            return ([b.get("line") for b in listed], [b.get("file") for b in listed],
                    row.get("line"), (row.get("location") or {}).get("path"))

        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", tmp]) as m:
            m.init()
            time.sleep(0.5)
            plant_loop(m)
            m.call("load_prg", path=path)

            dap = Dap(port)
            case("a default client's breakpoint is verified", begin(dap, False).get("verified") is True)
            plain = views(m)
            case("MCP reports the source line and path",
                 plain == ([3], [source], 3, source), str(plain))

            def at_source(read):
                # Back on the loop afterwards: past these NOPs is zeroed RAM, a BRK.
                m.call("pause")
                m.call("set_registers", pc=0x0A00)
                top = read()
                m.call("set_registers", pc=LOOP)
                return top

            def mcp_top():
                return data(m.call("get_stack", levels=1))["stackFrames"][0]

            frame = at_source(mcp_top)
            case("get_stack reports the source line and path",
                 (frame.get("line"), frame.get("file")) == (3, source), str(frame))
            dap.close()
            time.sleep(0.3)

            dap = Dap(port)
            bp = begin(dap, True)
            case("a 0-based, URI client's breakpoint is verified", bp.get("verified") is True, str(bp))
            case("and is answered in its format", bp.get("line") == 2, str(bp))
            seen = views(m)
            case("MCP results do not take that format", seen == plain, f"{seen} != {plain}")
            again = at_source(mcp_top)
            case("get_stack does not take it either", again == frame, f"{again} != {frame}")
            own = at_source(lambda: dap.request("x16/callStack", {"levels": 1})["body"]["stackFrames"][0])
            case("the client's own call stack keeps its format",
                 own.get("line") == 2 and own.get("file", "").startswith("file://"), str(own))

            own = dap.request("x16/listBreakpoints")["body"]["breakpoints"]
            own = [b for b in own if "dapId" in b]
            case("the client's own listing keeps its format",
                 [b.get("line") for b in own] == [2], str(own))
            row = dap.request("disassemble", {"memoryReference": "0x0A00",
                                              "instructionCount": 1})["body"]["instructions"][0]
            case("and so does its own disassembly",
                 row.get("line") == 2 and row.get("location", {}).get("path", "").startswith("file://"),
                 str(row))
            dap.close()

        # A load made through MCP can verify the editor's breakpoint. That event is the
        # editor's, even though an MCP request is what caused it.
        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", tmp]) as m:
            m.init()
            time.sleep(0.5)
            plant_loop(m)
            dap = Dap(port)
            bp = begin(dap, True)
            case("before the load it is pending", bp.get("verified") is False, str(bp))
            m.call("load_prg", path=path)
            ev = dap.event("breakpoint")
            got = ((ev or {}).get("body") or {}).get("breakpoint", {})
            case("an event caused by MCP is in the editor's format",
                 got.get("verified") is True and got.get("line") == 2, str(ev))
            dap.close()


def test_call_stack():
    print("call stack")
    # $0A00 calls $0A10, which calls $0A20, which spins.
    code = ("20100A" + "EA" * 13 + "20200A60" + "EA" * 12 + "EA4C200A")
    with tempfile.TemporaryDirectory() as tmp:
        labels = os.path.join(tmp, "stack.lbl")
        with open(labels, "w") as f:
            f.write("al 000A00 .outer\nal 000A03 .after\nal 000A10 .inner\n")
        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", tmp]) as m:
            m.init()
            time.sleep(0.5)
            m.call("pause")
            m.call("write_memory", address=0x0A00, hex=code)
            m.call("load_symbols", path=labels)
            status = data(m.call("get_registers"))["cpu"]["status"]
            m.call("set_registers", pc=0x0A00, sp=0x01FF, status=status | 0x04)
            m.call("set_breakpoint", address=0x0A20)
            since = data(m.call("get_status"))["stopId"]
            m.call("continue")
            m.call("wait_for_stop", sinceStopId=since)

            before = data(m.call("get_registers")), data(m.call("get_vera_state"))
            d = data(m.call("get_stack"))
            after = data(m.call("get_registers")), data(m.call("get_vera_state"))
            case("reading the stack changes nothing", before == after)
            frames = d.get("stackFrames", [])
            case("it says it is a guess", d.get("heuristic") is True, str(d)[:200])
            case("it counts the frames", d.get("totalFrames") == 3, str(d)[:200])
            case("innermost first", [f.get("address") for f in frames] == [0x0A20, 0x0A13, 0x0A03],
                 str(frames))
            case("frames are numbered", [f.get("index") for f in frames] == [0, 1, 2], str(frames))
            case("the first is the PC and the rest are returns",
                 [f.get("kind") for f in frames] == ["current", "return", "return"], str(frames))
            case("a return is named by the label it is inside",
                 [(f.get("symbol"), f.get("symbolOffset")) for f in frames[1:]] ==
                 [("inner", 3), ("outer", 3)], str(frames))
            case("a caller gives its call site",
                 [f.get("callSite") for f in frames] == [None, 0x0A10, 0x0A00], str(frames))

            part = data(m.call("get_stack", startFrame=1, levels=1))
            case("part of the stack", [f.get("address") for f in part.get("stackFrames", [])] == [0x0A13]
                 and part.get("totalFrames") == 3, str(part))
            past = data(m.call("get_stack", startFrame=9))
            case("past the end is empty", past.get("stackFrames") == [] and past.get("totalFrames") == 3,
                 str(past))
            for label, args in (("a negative start", {"startFrame": -1}),
                                ("a fraction", {"levels": 1.5}),
                                ("too many", {"levels": 65}),
                                ("text", {"startFrame": "1"})):
                case(f"get_stack refuses {label}", is_error(m.call("get_stack", **args)))
            case("get_stack refuses an unknown argument", is_error(m.call("get_stack", banana=1)))

            dap = Dap(port)
            r = dap.request("initialize", {"adapterID": "test", "linesStartAt1": True,
                                           "columnsStartAt1": True})
            case("DAP offers delayed stack loading",
                 r["body"].get("supportsDelayedStackTraceLoading") is True, str(r)[:200])
            dap.request("attach", {})
            dap.request("configurationDone")
            shown = dap.request("stackTrace", {"threadId": 1})["body"]
            case("DAP unwinds the same frames",
                 [int(f["instructionPointerReference"], 16) & 0xFFFF for f in shown["stackFrames"]] ==
                 [f["address"] for f in frames], str(shown)[:300])
            page = dap.request("stackTrace", {"threadId": 1, "startFrame": 1, "levels": 1})["body"]
            case("a DAP page keeps its frame id and the total",
                 [f["id"] for f in page["stackFrames"]] == [2] and page["totalFrames"] == 3, str(page)[:200])
            page = dap.request("stackTrace", {"threadId": 1, "startFrame": 2, "levels": 1e12})["body"]
            case("a huge DAP level count is all that remain",
                 [f["id"] for f in page["stackFrames"]] == [3], str(page)[:200])
            page = dap.request("stackTrace", {"threadId": 1, "startFrame": 1e12})["body"]
            case("a huge DAP start is past the end", page["stackFrames"] == [], str(page)[:200])
            flat = dap.request("x16/callStack")["body"]["stackFrames"]
            case("DAP gets the flat stack too", [f["address"] for f in flat] == [f["address"] for f in frames],
                 str(flat))
            case("DAP names a caller by its call too",
                 [f["name"] for f in shown["stackFrames"][1:]] == ["inner+3 ($0A13)", "outer+3 ($0A03)"],
                 str(shown)[:300])
            dap.close()

        # A JSR on one line, returning to the first instruction of the next routine.
        with open(os.path.join(tmp, "site.prg"), "wb") as f:
            f.write(struct.pack("<H", 0x0A00) + bytes.fromhex("20030AEA4C030A") + b"\xEA" * 9)
        with open(os.path.join(tmp, "site.dbg"), "w") as f:
            f.write('version\tmajor=2,minor=0\n'
                    'info\tcsym=0,file=1,lib=0,line=2,mod=1,scope=1,seg=1,span=2,sym=2,type=0\n'
                    'file\tid=0,name="site.s",size=10,mtime=0x00000000,mod=0\n'
                    'seg\tid=0,name="CODE",start=0x000A00,size=0x0010,addrsize=absolute,type=ro,'
                    'oname="site.prg"\n'
                    'span\tid=0,seg=0,start=0,size=3,type=0\n'
                    'span\tid=1,seg=0,start=3,size=13,type=0\n'
                    'line\tid=0,file=0,line=5,span=0\n'
                    'line\tid=1,file=0,line=9,span=1\n'
                    'sym\tid=0,name="_first",addrsize=absolute,size=3,scope=0,def=0,val=0x000A00,type=lab\n'
                    'sym\tid=1,name="_second",addrsize=absolute,size=3,scope=0,def=0,val=0x000A03,type=lab\n'
                    'mod\tid=0,name="site.o",file=0\n')
        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", tmp]) as m:
            m.init()
            time.sleep(0.5)
            m.call("pause")
            m.call("load_prg", path=os.path.join(tmp, "site.prg"))
            status = data(m.call("get_registers"))["cpu"]["status"]
            m.call("set_registers", pc=0x0A00, sp=0x01FF, status=status | 0x04)
            m.call("set_breakpoint", address=0x0A03)
            since = data(m.call("get_status"))["stopId"]
            m.call("continue")
            m.call("wait_for_stop", sinceStopId=since)

            frames = data(m.call("get_stack")).get("stackFrames", [])
            view = [(f.get("address"), f.get("symbol"), f.get("symbolOffset"), f.get("line")) for f in frames]
            case("the current frame is its own line",
                 view[:1] == [(0x0A03, "_second", 0, 9)], str(view))
            case("a caller is the line and routine of its JSR, not of its return",
                 view[1:] == [(0x0A03, "_first", 3, 5)], str(view))
            dap = Dap(port)
            dap.request("initialize", {"adapterID": "test", "linesStartAt1": True, "columnsStartAt1": True})
            dap.request("attach", {})
            dap.request("configurationDone")
            shown = dap.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
            case("DAP reports the same line and routine",
                 [(f["name"], f["line"]) for f in shown] ==
                 [("_second ($0A03)", 9), ("_first+3 ($0A03)", 5)], str(shown)[:300])
            case("and keeps the return address as the instruction pointer",
                 int(shown[1]["instructionPointerReference"], 16) == 0x0A03, str(shown)[:300])
            dap.close()


def test_read_paths():
    print("where MCP may read")
    label_text = "al 000820 .inside\n"
    prg_bytes = struct.pack("<H", 0x0A00) + b"\xEA\xEA"

    def put(path, content):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(content.encode() if isinstance(content, str) else content)
        return path

    def link(target, name):
        try:
            os.symlink(target, name)
            return True
        except (OSError, NotImplementedError):
            return False    # creating links needs a privilege on Windows

    with tempfile.TemporaryDirectory() as base:
        root = os.path.join(base, "root")
        outside = os.path.join(base, "outside")
        sibling = os.path.join(base, "root2")
        inner = put(os.path.join(root, "sub", "in.lbl"), label_text)
        program = put(os.path.join(root, "prog.prg"), prg_bytes)
        secret = put(os.path.join(outside, "secret.lbl"), label_text)
        secret_prg = put(os.path.join(outside, "secret.prg"), prg_bytes)
        near = put(os.path.join(sibling, "near.lbl"), label_text)
        linked = os.path.join(root, "linked.lbl")
        has_links = link(secret, linked)
        dbg_program = put(os.path.join(root, "guarded", "prog.prg"), prg_bytes)
        has_dbg_link = link(put(os.path.join(outside, "x.dbg"), K_PRG_DBG),
                            os.path.join(root, "guarded", "prog.dbg"))

        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", root]) as m:
            m.init()
            time.sleep(0.5)
            plant_loop(m)

            case("an absolute path inside loads",
                 data(m.call("load_symbols", path=inner)).get("labels") == 1)
            case("a relative path is from the root",
                 data(m.call("load_symbols", path=os.path.join("sub", "in.lbl"))).get("labels") == 1)
            case("a program inside loads", data(m.call("load_prg", path=program)).get("length") == 2)

            bad = (("an absolute path outside", secret),
                   ("..", os.path.join("..", "outside", "secret.lbl")),
                   ("a nested ..", os.path.join("sub", "..", "..", "outside", "secret.lbl")),
                   ("a folder that only starts the same", near),
                   ("a folder", os.path.join(root, "sub")))
            for label, p in bad:
                case(f"load_symbols refuses {label}", is_error(m.call("load_symbols", path=p)))
            case("load_prg refuses a file outside", is_error(m.call("load_prg", path=secret_prg)))
            r = m.call("load_symbols", path=secret)
            missing = m.call("load_symbols", path=os.path.join(outside, "absent.lbl"))
            case("outside and missing read the same",
                 r.get("content") == missing.get("content"), f"{r} != {missing}")
            if has_links:
                case("a link leading outside is refused", is_error(m.call("load_symbols", path=linked)))
            if has_dbg_link:
                case("a .dbg link leading outside refuses the load",
                     is_error(m.call("load_prg", path=dbg_program)))
            if os.name == "nt":
                for name in ("CON", "NUL", "COM1.lbl", "in.lbl:stream", "\\\\server\\share\\a.lbl"):
                    case(f"{name} is refused", is_error(m.call("load_symbols", path=name)))

            dap = Dap(port)
            dap.session()
            r = dap.request("x16/loadSymbols", {"path": secret})
            case("a DAP client is not restricted", (r or {}).get("success") is True, str(r))
            dap.close()

        # No -fsroot: the working folder is what the machine itself can see.
        with Mcp() as m:
            m.init()
            time.sleep(0.5)
            put(os.path.join(m.dir.name, "here.lbl"), label_text)
            case("without -fsroot the working folder is readable",
                 data(m.call("load_symbols", path="here.lbl")).get("labels") == 1)
            case("and a folder elsewhere is not", is_error(m.call("load_symbols", path=secret)))

        # The folder of the program the user launched with.
        with Mcp(["-prg", secret_prg]) as m:
            m.init()
            time.sleep(0.5)
            case("the -prg folder is readable",
                 data(m.call("load_symbols", path=secret)).get("labels") == 1)
            case("but not the folder next to it", is_error(m.call("load_symbols", path=near)))


def main():
    global EXE, ROM
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    EXE, ROM = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    for test in (test_protocol, test_tools, test_lifetime, test_with_dap,
                 test_run_control, test_state_tools, test_breakpoints, test_coexistence,
                 test_http, test_lease, test_vision, test_vera_state, test_stop_details,
                 test_vram_watch, test_read_watch, test_symbols,
                 test_hash_memory, test_dump_memory, test_warp, test_mouse, test_load_prg,
                 test_client_format, test_call_stack, test_read_paths):
        try:
            test()
        except Exception as ex:   # one broken case must not hide the others
            case(f"{test.__name__} ran to completion", False, f"{type(ex).__name__}: {ex}")
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
