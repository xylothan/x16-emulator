#!/usr/bin/env python3
"""Checks the MCP server, over the same pipes and sockets a real client uses.

    python testbench/test_mcp.py <x16emu> <rom.bin>

Each case launches a private emulator with -mcp, or -mcp <port> on a free port.
Every line it writes to stdout under stdio must be one JSON-RPC message: anything
else would break a real client.
"""

import json
import os
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time

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
    def __init__(self, extra=()):
        self.dir = tempfile.TemporaryDirectory()
        cmd = [EXE, "-rom", ROM, "-mcp", "-hidden", "-warp", "-zeroram"] + list(extra)
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

    def call(self, name, **args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})
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
                   "wait_for_stop", "perf_stats"}
        losing = {"reset", "set_registers", "write_memory", "save_state", "load_state"}
        moving = {"pause", "continue", "step", "run_frames", "set_breakpoint", "remove_breakpoint",
                  "type_text", "send_key", "joystick"}
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
                            ("a bad kind", {"address": 0, "kind": "read"}),
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

    def call(self, name, **args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})
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


def main():
    global EXE, ROM
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    EXE, ROM = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    for test in (test_protocol, test_tools, test_lifetime, test_with_dap,
                 test_run_control, test_state_tools, test_breakpoints, test_coexistence,
                 test_http, test_lease):
        try:
            test()
        except Exception as ex:   # one broken case must not hide the others
            case(f"{test.__name__} ran to completion", False, f"{type(ex).__name__}: {ex}")
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
