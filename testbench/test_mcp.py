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
    def __init__(self, extra=(), warp=True, cwd=None):
        self.dir = tempfile.TemporaryDirectory()
        cmd = [EXE, "-rom", ROM, "-mcp", "-hidden", "-zeroram"] + (["-warp"] if warp else []) + list(extra)
        self.p = subprocess.Popen(cmd, cwd=cwd or self.dir.name, stdin=subprocess.PIPE,
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


READING_TOOLS = {"get_status", "get_registers", "read_memory", "disassemble", "list_breakpoints",
                 "wait_for_stop", "get_screen", "screenshot",
                 "get_vera_state", "lookup_symbol", "list_symbols", "hash_memory", "search_memory", "get_stack",
                 "get_audio_state", "get_io_state", "read_io_trace", "get_sprites", "get_palette", "get_tilemap",
                 "list_sources", "read_source", "source_location", "host_perf",
                 "get_sdcard", "evaluate", "read_log"}
LOSING_TOOLS = {"reset", "set_registers", "write_memory", "save_state", "load_state",
                "dump_memory", "load_prg", "perf_config", "perf_history", "profile_capture",
                "sprite_mux_config"}
MOVING_TOOLS = {"pause", "continue", "step", "run_frames", "run_to", "set_breakpoint", "remove_breakpoint",
                "enable_breakpoint", "type_text", "send_key", "joystick", "load_symbols", "mouse",
                "set_warp", "perf_stats", "sprite_mux_stats", "set_stop_on", "set_voice_mute",
                "set_io_capture", "trigger_interrupt"}


def test_tools():
    print("tools")
    with Mcp() as m:
        m.init()
        r = m.rpc("tools/list")
        listed = (r or {}).get("result", {}).get("tools", [])
        names = {t["name"] for t in listed}
        reading, losing, moving = READING_TOOLS, LOSING_TOOLS, MOVING_TOOLS
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


def search_all(m, **args):
    """Every match of a search, following `next`, and how many calls it took."""
    found, calls = [], 0
    while True:
        d = data(m.call("search_memory", **args))
        calls += 1
        if "matches" not in d:
            return None, calls
        found += [(x["address"], x["bank"]) for x in d["matches"]]
        if not d.get("more"):
            return found, calls
        args = {**args, **d["next"]}


def test_search_memory():
    print("search_memory")
    port = free_port()
    with Mcp(["-debugport", str(port)]) as m:
        m.init()
        time.sleep(0.5)
        dap = Dap(port)
        dap.session()
        plant_loop(m)

        m.call("write_memory", address=0x0A00, hex="DE AD BE EF".replace(" ", ""))
        d = data(m.call("search_memory", hex="DE AD BE EF", start="$0A00", end="$0AFF"))
        case("a CPU match", [(x["address"], x["bank"]) for x in d.get("matches", [])] == [("0x0A00", None)], str(d))
        case("it is complete", d.get("more") is False and "next" not in d and d.get("count") == 1, str(d))
        d = data(m.call("search_memory", text="READY", space="rom", bank=4, limit=1))
        hit = (d.get("matches") or [{}])[0]
        at = int(hit.get("address", "0"), 16)
        case("a text match in one ROM bank", hit.get("bank") == 4 and
             mem_bytes(m, address=at, length=5, bank=4) == b"READY", str(d))

        m.call("write_memory", address=0x0B00, hex="77777777")
        d = data(m.call("search_memory", hex="7777", start=0x0B00, end=0x0B03))
        case("overlapping matches are all reported",
             [x["address"] for x in d.get("matches", [])] == ["0x0B00", "0x0B01", "0x0B02"], str(d))
        d = data(m.call("search_memory", hex="7777", start=0x0B00, end=0x0B03, limit=2))
        case("the limit says there is more", d.get("more") is True and d.get("stoppedBy") == "limit" and
             d.get("next") == {"start": "0x0B02"}, str(d))
        d = data(m.call("search_memory", hex="7777", start=d["next"]["start"], end=0x0B03, limit=2))
        case("and continuing keeps the overlap",
             [x["address"] for x in d.get("matches", [])] == ["0x0B02"] and d.get("more") is False, str(d))
        d = data(m.call("search_memory", hex="77777777", start=0x0B00, end=0x0B02))
        case("a match must fit inside the range", d.get("matches") == [], str(d))

        # $BFFF/$C000 in the CPU map, as now banked.
        regs = data(m.call("get_registers"))["cpu"]
        ram_bank, rom_bank = regs["ramBank"], regs["romBank"]
        last = mem_bytes(m, address=0xC000, length=2, bank=rom_bank)
        m.call("write_memory", address=0xBFFE, hex="5AA5")
        pat = "5AA5" + last.hex()
        d = data(m.call("search_memory", hex=pat, start=0xBF00, end=0xC0FF))
        hit = (d.get("matches") or [{}])[0]
        case("a match across $BFFF/$C000", hit.get("address") == "0xBFFE" and hit.get("bank") == ram_bank and
             hit.get("crossesBank") is True, str(d))
        d = data(m.call("search_memory", hex="5AA5", start=0xBF00, end=0xBFFF))
        hit = (d.get("matches") or [{}])[0]
        case("one inside the bank is not marked", hit.get("address") == "0xBFFE" and
             hit.get("crossesBank") is False, str(d))

        # From the end of one RAM bank into the next.
        other = (ram_bank + 1) % 8 or 1
        m.call("set_registers", ramBank=other)
        m.call("write_memory", address=0xBFFE, hex="C3D2")
        m.call("set_registers", ramBank=other + 1)
        m.call("write_memory", address=0xA000, hex="E1F0")
        m.call("set_registers", ramBank=ram_bank)
        found, calls = search_all(m, hex="C3D2E1F0", space="ram")
        case("a match across two banks, in all banks", found == [("0xBFFE", other)], str(found))
        case("all banks takes more than one call", calls > 1, str(calls))
        d = data(m.call("search_memory", hex="C3D2E1F0", space="ram", bank=other))
        case("one bank does not run into the next", d.get("matches") == [] and d.get("more") is False, str(d))
        d = data(m.call("search_memory", hex="C3D2", space="ram", bank=other))
        case("one bank finds its own bytes", [(x["address"], x["bank"]) for x in d.get("matches", [])] ==
             [("0xBFFE", other)], str(d))
        d = data(m.call("search_memory", hex="C3D2E1F0", space="ram", start="$BFFE", startBank=other, limit=1))
        hit = (d.get("matches") or [{}])[0]
        case("continuing from a bank", hit.get("bank") == other and hit.get("crossesBank") is True, str(d))

        # The budget, on a search that matches nothing.
        d = data(m.call("search_memory", hex="FE ED FA CE 01", space="ram"))
        case("the budget stops a long search", d.get("more") is True and d.get("stoppedBy") == "budget" and
             d.get("matches") == [] and "startBank" in d.get("next", {}), str(d))
        found, calls = search_all(m, hex="FE ED FA CE 01", space="ram")
        case("and it runs to the end", found == [] and calls >= 2, f"{found} in {calls}")

        m.call("write_memory", address="1F000", hex="0102030405", space="vram")
        d = data(m.call("search_memory", hex="0102030405", space="vram", start="0x1EFFF"))
        hit = (d.get("matches") or [{}])[0]
        case("a VRAM match", hit.get("address") == "0x1F000" and hit.get("bank") is None, str(d))

        # No side effects, with the I/O page in the range.
        set_vera(m, 0x00, 0x00, 0x00, 0x10)
        hash_of = lambda: dap.request("x16/status", {"hash": True})["body"]["hash"]
        first = hash_of()
        case("the machine hash is stable", first == hash_of())
        vera = data(m.call("get_vera_state"))
        stop = data(m.call("get_status")).get("stopId")
        m.call("set_breakpoint", address=0x9F23, kind="read")
        for args in ({"hex": "FE ED FA CE 01"},
                     {"hex": "00", "limit": 1024},
                     {"hex": "FE ED FA CE 01", "start": 0x9F00, "end": 0x9FFF},
                     {"text": "READY", "space": "rom"},
                     {"hex": "FE ED FA CE 01", "space": "vram"}):
            search_all(m, **args)
        found, _ = search_all(m, hex="FE ED FA CE 01", space="ram")
        m.call("remove_breakpoint", address=0x9F23, kind="read")
        case("searching changes no machine state", hash_of() == first)
        case("nor VERA's data port", data(m.call("get_vera_state")) == vera)
        case("and fires no read watch", data(m.call("get_status")).get("stopId") == stop)

        for label, args in (("no pattern", {}),
                            ("both patterns", {"hex": "00", "text": "A"}),
                            ("an empty hex", {"hex": ""}),
                            ("blank hex", {"hex": "  "}),
                            ("an empty text", {"text": ""}),
                            ("bad hex", {"hex": "XY"}),
                            ("an odd digit", {"hex": "ABC"}),
                            ("a space inside a byte", {"hex": "A B"}),
                            ("too long a pattern", {"hex": "00" * 65}),
                            ("non-ASCII text", {"text": "caf\u00e9"}),
                            ("a bad space", {"hex": "00", "space": "disk"}),
                            ("a bank on the CPU map", {"hex": "00", "bank": 1}),
                            ("a bank on VRAM", {"hex": "00", "space": "vram", "bank": 1}),
                            ("a RAM bank not fitted", {"hex": "00", "space": "ram", "bank": 64}),
                            ("a ROM bank past the last", {"hex": "00", "space": "rom", "bank": 32}),
                            ("a start bank not fitted", {"hex": "00", "space": "ram", "startBank": 64}),
                            ("a start bank with one bank", {"hex": "00", "space": "ram", "bank": 1, "startBank": 1}),
                            ("an end across all banks", {"hex": "00", "space": "ram", "end": 0xA100}),
                            ("a start outside the window", {"hex": "00", "space": "ram", "bank": 1, "start": 0x9FFF}),
                            ("an end outside the window", {"hex": "00", "space": "rom", "bank": 1, "end": 0xBFFF}),
                            ("a CPU address past 64K", {"hex": "00", "start": 0x10000}),
                            ("a VRAM address past the end", {"hex": "00", "space": "vram", "end": 0x20000}),
                            ("start after end", {"hex": "00", "start": 0x2000, "end": 0x1000}),
                            ("a bare address", {"hex": "00", "start": "C000"}),
                            ("a negative address", {"hex": "00", "start": -1}),
                            ("a limit of zero", {"hex": "00", "limit": 0}),
                            ("too high a limit", {"hex": "00", "limit": 1025}),
                            ("an unknown argument", {"hex": "00", "banana": 1})):
            case(f"search_memory refuses {label}", is_error(m.call("search_memory", **args)))
        r = dap.request("x16/searchMemory", {"hex": "DEADBEEF", "start": 0x0A00, "end": 0x0A10})
        case("the DAP request answers too", (r or {}).get("body", {}).get("count") == 1, str(r)[:160])
        r = dap.request("x16/searchMemory", {"hex": "ZZ"})
        case("and refuses the same way", (r or {}).get("success") is False, str(r)[:160])
        dap.close()

    with Mcp(["-mcp-readonly"]) as m:
        m.init()
        time.sleep(0.4)
        d = data(m.call("search_memory", hex="00", start=0, end=15, limit=1))
        case("it works under -mcp-readonly", d.get("count") == 1, str(d)[:160])


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


def test_source_fifo():
    print("debug info naming a FIFO")
    if not hasattr(os, "mkfifo"):
        print("  skipped: no FIFOs on this platform")
        return

    def dbg(name):
        return ('version\tmajor=2,minor=0\n'
                'info\tcsym=0,file=2,lib=0,line=2,mod=1,scope=1,seg=1,span=2,sym=0,type=0\n'
                'file\tid=0,name="gen.c",size=10,mtime=0x00000000,mod=0\n'
                f'file\tid=1,name="{name}",size=10,mtime=0x00000000,mod=0\n'
                'seg\tid=0,name="CODE",start=0x000A00,size=0x0010,addrsize=absolute,type=ro,oname="gen.prg"\n'
                'span\tid=0,seg=0,start=0,size=8,type=0\n'
                'span\tid=1,seg=0,start=8,size=8,type=0\n'
                'line\tid=0,file=0,line=1,type=1,count=1,span=0\n'
                'line\tid=1,file=1,line=1,span=1\n'
                'mod\tid=0,name="gen.o",file=0\n')

    # (label, name recorded in the .dbg, what sits at <dir>/gen.s, DAP offers it)
    for label, recorded, kind, offered in (("FIFO at the recorded path", None, "fifo", False),
                                           ("FIFO beside the .dbg", "elsewhere/gen.s", "fifo", False),
                                           ("regular file", "elsewhere/gen.s", "file", True),
                                           ("nothing", "elsewhere/gen.s", None, False)):
        with tempfile.TemporaryDirectory() as tmp:
            target = os.path.join(tmp, "gen.s")
            if kind == "fifo":
                try:
                    os.mkfifo(target)
                except OSError:
                    print("  skipped: this filesystem has no FIFOs")
                    return
            elif kind == "file":
                with open(target, "w") as f:
                    f.write("; kept\n")
            prg = os.path.join(tmp, "gen.prg")
            with open(prg, "wb") as f:
                f.write(struct.pack("<H", 0x0A00) + b"\xEA" * 16)
            with open(os.path.join(tmp, "gen.dbg"), "w") as f:
                f.write(dbg(recorded or target))

            port = free_port()
            with Mcp(["-debugport", str(port), "-fsroot", tmp]) as m:
                m.init()
                time.sleep(0.5)
                m.call("pause")
                d = data(m.result(m.start("load_prg", path=prg), timeout=8))
                case(f"{label}: the load answers", d.get("debugInfo") is True, str(d))
                if d.get("debugInfo") is not True:
                    continue
                dap = Dap(port)
                dap.request("initialize", {"adapterID": "test", "linesStartAt1": True, "columnsStartAt1": True})
                dap.request("attach", {})
                dap.request("configurationDone")
                shown = {s["name"] for s in dap.request("loadedSources")["body"]["sources"]}
                dap.close()
                case(f"{label}: offered is {offered}", shown == ({"gen.c", "gen.s"} if offered else {"gen.c"}),
                     str(shown))


def fifo_reader_blocked(path):
    """Whether something sits in open() on this FIFO. Releases it."""
    blocked = False
    for _ in range(5):
        try:
            fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            return blocked
        blocked = True
        os.write(fd, b"\n")
        os.close(fd)
        time.sleep(0.1)
    return blocked


def test_dbg_fifo():
    print("a FIFO where debug info belongs")
    if not hasattr(os, "mkfifo"):
        print("  skipped: no FIFOs on this platform")
        return
    body = b"\xEA" * 16

    def guest_load(m):
        stop_on_wait_booted(m)
        m.call("type_text", text='LOAD"PROG.PRG",8,1\r')
        end = time.time() + 8
        while time.time() < end:
            r = m.reply(m.start("read_memory", address=0x0A00, length=16), 3)
            if r is None:
                return False
            if bytes.fromhex(data(r.get("result", {})).get("hex", "").replace(" ", "")) == body:
                return True
            time.sleep(0.1)
        return False

    with tempfile.TemporaryDirectory() as tmp:
        prg = os.path.join(tmp, "prog.prg")
        dbg = os.path.join(tmp, "prog.dbg")
        with open(prg, "wb") as f:
            f.write(struct.pack("<H", 0x0A00) + body)
        with open(dbg, "w") as f:
            f.write(K_PRG_DBG)
        with Mcp(["-fsroot", tmp]) as m:
            m.init()
            case("guest LOAD with a regular .dbg lands", guest_load(m))
            r = data(m.call("lookup_symbol", name="_hello"))
            case("and its debug info loads", (r.get("matches") or [{}])[0].get("address") == 0x0A00, str(r))

        os.remove(dbg)
        try:
            os.mkfifo(dbg)
        except OSError:
            print("  skipped: this filesystem has no FIFOs")
            return

        with Mcp(["-fsroot", tmp]) as m:
            m.init()
            landed = guest_load(m)
            case("guest LOAD never opens the FIFO", not fifo_reader_blocked(dbg))
            case("and the program lands", landed)

        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", tmp]) as m:
            m.init()
            time.sleep(0.5)
            m.call("pause")
            r = m.call("load_prg", path=prg)
            words = " ".join(c.get("text", "") for c in r.get("content", []))
            case("load_prg refuses it", is_error(r), str(r))
            case("without calling it outside the folders", "outside" not in words, words)

            dap = Dap(port)
            dap.request("initialize", {"adapterID": "test", "linesStartAt1": True, "columnsStartAt1": True})
            for label, command, args, ok in (("x16/loadPrg", "x16/loadPrg", {"path": prg}, True),
                                             ("x16/loadSymbols", "x16/loadSymbols", {"path": dbg}, False),
                                             ("attach", "attach", {"dbgFile": dbg}, True),
                                             ("launch", "launch", {"dbgFile": dbg}, True)):
                answer = dap.request(command, args, timeout=3)
                case(f"{label} never opens the FIFO", not fifo_reader_blocked(dbg))
                case(f"{label} answers", (answer or {}).get("success") is ok, str(answer))
            dap.close()


class WinPipe:
    """A named pipe server that records whether a client connected."""

    def __init__(self):
        import ctypes
        from ctypes import wintypes as w

        class Overlapped(ctypes.Structure):
            _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                        ("Offset", w.DWORD), ("OffsetHigh", w.DWORD), ("hEvent", w.HANDLE)]

        k = self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.CreateNamedPipeW.restype = k.CreateEventW.restype = w.HANDLE
        k.CreateNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.LPVOID]
        k.CreateEventW.argtypes = [w.LPVOID, w.BOOL, w.BOOL, w.LPCWSTR]
        k.ConnectNamedPipe.argtypes = [w.HANDLE, ctypes.POINTER(Overlapped)]
        k.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
        k.CloseHandle.argtypes = [w.HANDLE]
        self.name = f"x16emu-test-{os.getpid()}-{time.monotonic_ns()}"
        PIPE_ACCESS_DUPLEX_OVERLAPPED = 0x40000003
        self.pipe = k.CreateNamedPipeW("\\\\.\\pipe\\" + self.name, PIPE_ACCESS_DUPLEX_OVERLAPPED,
                                       0, 1, 4096, 4096, 0, None)
        self.overlapped = Overlapped()
        self.overlapped.hEvent = k.CreateEventW(None, True, False, None)
        k.ConnectNamedPipe(self.pipe, ctypes.byref(self.overlapped))

    def connected(self):
        return self.k.WaitForSingleObject(self.overlapped.hEvent, 0) == 0

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.k.CloseHandle(self.overlapped.hEvent)
        self.k.CloseHandle(self.pipe)


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


def source_dbg(forged, bankb_size=4):
    files = ["solo.s", "a/main.s", "b/main.s", forged, "caf\xe9.s", "bytes.s", "pipe.s",
             "linked.s", "big.s", "missing.s"]
    spans = [(0, 0, 2), (0, 2, 3), (1, 0, 4), (2, 0, 4), (0, 5, 1), (0, 6, 1)]
    lines = [(0, 2, 0), (0, 3, 1), (0, 7, 2), (0, 7, 3), (1, 1, 4), (2, 1, 5)]
    for i in range(3, len(files)):
        lines.append((i, 1, len(spans)))
        spans.append((0, 4 + i, 1))
    syms = ['sym\tid=0,name="_hello",addrsize=absolute,size=3,scope=0,def=0,val=0x000A02,type=lab\n']
    text = ('version\tmajor=2,minor=0\n'
            f'info\tcsym=0,file={len(files)},lib=0,line={len(lines)},mod=1,scope=1,seg=3,'
            f'span={len(spans)},sym={len(syms)},type=0\n')
    for i, name in enumerate(files):
        text += f'file\tid={i},name="{name}",size=10,mtime=0x00000000,mod=0\n'
    text += ('seg\tid=0,name="CODE",start=0x000A00,size=0x0010,addrsize=absolute,type=ro,oname="src.prg"\n'
             'seg\tid=1,name="BANKA",start=0x00A000,size=0x0004,addrsize=absolute,type=ro,oname="src.prg"\n'
             f'seg\tid=2,name="BANKB",start=0x00A000,size=0x{bankb_size:04X},addrsize=absolute,type=ro,'
             'oname="src.prg"\n')
    for i, (seg, start, size) in enumerate(spans):
        text += f'span\tid={i},seg={seg},start={start},size={size},type=0\n'
    for i, (file, line, span) in enumerate(lines):
        text += f'line\tid={i},file={file},line={line},span={span}\n'
    return (text + "".join(syms) + 'mod\tid=0,name="src.o",file=0\n').encode("latin-1")


def test_sources():
    print("source lookup")

    def put(path, content):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(content.encode() if isinstance(content, str) else content)
        return path

    def said(result):
        return result.get("content", [{}])[0].get("text", "")

    prg_bytes = struct.pack("<H", 0x0A00) + bytes.fromhex("EAEA4C020A") + b"\xEA" * 11

    with tempfile.TemporaryDirectory() as base:
        root = os.path.join(base, "root")
        secret = put(os.path.join(base, "outside", "secret.s"), "secret\n")
        forged = secret.replace("\\", "/")
        program = put(os.path.join(root, "src.prg"), prg_bytes)
        put(os.path.join(root, "src.dbg"), source_dbg(forged))
        put(os.path.join(root, "solo.s"), b"one\r\ntwo\r\nthree\nfour")
        put(os.path.join(root, "a", "main.s"), "in a\n")
        put(os.path.join(root, "b", "main.s"), "in b\n")
        put(os.path.join(root, "bytes.s"), b"ok\xff\n\x00z\n")
        put(os.path.join(root, "big.s"), b"x" * (4 * 1024 * 1024 + 1))
        has_fifo = hasattr(os, "mkfifo")
        if has_fifo:
            os.mkfifo(os.path.join(root, "pipe.s"))
        try:
            os.symlink(secret, os.path.join(root, "linked.s"))
            has_link = True
        except (OSError, NotImplementedError):
            has_link = False

        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", root]) as m:
            m.init()
            time.sleep(0.5)
            m.call("pause")
            case("no debug info, no sources", data(m.call("list_sources")).get("sources") == [])
            case("the program loads", data(m.call("load_prg", path=program)).get("length") == 16)

            paths = {s.get("path") for s in data(m.call("list_sources")).get("sources", [])}
            case("list_sources names the files", {"solo.s", "a/main.s", "b/main.s", forged} <= paths,
                 str(paths))
            case("a name that is not UTF-8 stays valid JSON", "caf\ufffd.s" in paths, str(paths))

            d = data(m.call("read_source", file="solo.s"))
            case("reads lines, with CRLF and no final newline",
                 d.get("lines") == ["one", "two", "three", "four"] and d.get("totalLines") == 4 and
                 d.get("startLine") == 1, str(d)[:200])
            d = data(m.call("read_source", file="solo.s", startLine=2, lineCount=2))
            case("reads a range", d.get("lines") == ["two", "three"] and d.get("totalLines") == 4, str(d)[:200])
            d = data(m.call("read_source", file="solo.s", startLine=9))
            case("past the end is empty", d.get("lines") == [] and d.get("totalLines") == 4, str(d)[:200])
            case("a folder picks a file",
                 data(m.call("read_source", file="b/main.s")).get("lines") == ["in b"])
            res = m.call("read_source", file="main.s")
            case("an ambiguous name is refused and lists both",
                 is_error(res) and "a/main.s" in said(res) and "b/main.s" in said(res), str(res)[:200])
            case("a file outside the debug info is refused", is_error(m.call("read_source", file="nope.s")))
            d = data(m.call("read_source", file="bytes.s"))
            case("invalid bytes and NUL become U+FFFD", d.get("lines") == ["ok\ufffd", "\ufffdz"], str(d)[:200])
            case("a file over the limit is refused", "4 MiB" in said(m.call("read_source", file="big.s")))
            for label, args in (("line 0", {"file": "solo.s", "startLine": 0}),
                                ("too many lines", {"file": "solo.s", "lineCount": 2001}),
                                ("no file", {})):
                case(f"read_source refuses {label}", is_error(m.call("read_source", **args)))

            missing = m.call("read_source", file="missing.s")
            refused = m.call("read_source", file=forged)
            case("a name outside the roots is refused", is_error(refused), str(refused)[:200])
            case("refused and missing read the same", said(refused) == said(missing), said(refused))
            case("the refusal names no location",
                 "outside" not in said(refused) and base not in said(refused), said(refused))
            if has_link:
                case("a link out reads as missing",
                     said(m.call("read_source", file="linked.s")) == said(missing))
            if has_fifo:
                case("a pipe reads as missing and does not block",
                     said(m.call("read_source", file="pipe.s")) == said(missing))

            at = data(m.call("source_location", address=0x0A03)).get("locations", [])
            case("an address gives its line",
                 [(x.get("file"), x.get("line"), x.get("bankKnown")) for x in at] == [("solo.s", 3, True)], str(at))
            at = data(m.call("source_location", address="_hello")).get("locations", [])
            case("a symbol gives its line", [x.get("line") for x in at] == [3], str(at))
            case("an address with no code is empty",
                 data(m.call("source_location", address=0x0400)).get("locations") == [])
            at = data(m.call("source_location", file="solo.s", line=3)).get("locations", [])
            case("a line gives its address",
                 [(x.get("address"), x.get("bank"), x.get("segment")) for x in at] == [(0x0A02, None, "CODE")],
                 str(at))
            case("a line with no code is empty",
                 data(m.call("source_location", file="solo.s", line=4)).get("locations") == [])
            case("an unknown file is empty",
                 data(m.call("source_location", file="nope.s", line=1)).get("locations") == [])
            at = data(m.call("source_location", file="solo.s", line=7)).get("locations", [])
            case("segments at one address are both listed",
                 sorted((x.get("address"), x.get("bank"), x.get("segment")) for x in at) ==
                 [(0xA000, None, "BANKA"), (0xA000, None, "BANKB")], str(at))
            case("source_location refuses an ambiguous file",
                 is_error(m.call("source_location", file="main.s", line=1)))
            for label, args in (("nothing", {}), ("both forms", {"address": 0x0A00, "file": "solo.s", "line": 3}),
                                ("a file alone", {"file": "solo.s"}), ("a line alone", {"line": 3}),
                                ("line 0", {"file": "solo.s", "line": 0}),
                                ("a VRAM address", {"address": "vram:100"}),
                                ("a far address", {"address": 0x10000})):
                case(f"source_location refuses {label}", is_error(m.call("source_location", **args)))

            res = m.call("set_breakpoint", file="solo.s", line=7)
            case("a line in two places is refused, naming them",
                 is_error(res) and "BANKA" in said(res) and "BANKB" in said(res), said(res))
            case("nothing was armed", data(m.call("list_breakpoints")).get("breakpoints") == [],
                 str(data(m.call("list_breakpoints")))[:200])
            d = data(m.call("set_breakpoint", file="solo.s", line=7, bank=4))
            same = data(m.call("set_breakpoint", address=0xA000, bank=4))
            case("a bank picks one", d.get("address") == same.get("address") and d.get("file") == "solo.s" and
                 d.get("line") == 7, str(d))
            d = data(m.call("remove_breakpoint", file="solo.s", line=7, bank=4))
            case("removed by its line", d.get("stillArmed") is False, str(d))
            for label, args in (("an address too", {"address": 0x0A00, "file": "solo.s", "line": 3}),
                                ("a file alone", {"file": "solo.s"}), ("a line alone", {"line": 3}),
                                ("a watch", {"file": "solo.s", "line": 3, "kind": "write"}),
                                ("VRAM", {"file": "solo.s", "line": 3, "space": "vram"}),
                                ("no code", {"file": "solo.s", "line": 4}),
                                ("an ambiguous file", {"file": "main.s", "line": 1}),
                                ("nothing", {})):
                case(f"set_breakpoint refuses {label}", is_error(m.call("set_breakpoint", **args)))
            case("remove_breakpoint refuses no target", is_error(m.call("remove_breakpoint")))

            d = data(m.call("set_breakpoint", file="solo.s", line=3))
            case("a line sets a breakpoint", d.get("kind") == "exec" and d.get("line") == 3, str(d))
            status = data(m.call("get_registers"))["cpu"]["status"]
            m.call("set_registers", pc=0x0A00, sp=0x01FF, status=status | 0x04)
            since = data(m.call("get_status"))["stopId"]
            m.call("continue")
            m.call("wait_for_stop", sinceStopId=since)
            case("it stops there", data(m.call("get_registers"))["cpu"]["pc"] == 0x0A02)
            m.call("remove_breakpoint", file="solo.s", line=3)

            dap = Dap(port)
            dap.request("initialize", {"adapterID": "test", "linesStartAt1": False,
                                       "columnsStartAt1": True, "pathFormat": "uri"})
            dap.request("attach", {})
            dap.request("configurationDone")
            at = data(m.call("source_location", file="solo.s", line=3)).get("locations", [])
            case("MCP stays 1-based with a 0-based editor attached",
                 [(x.get("file"), x.get("line")) for x in at] == [("solo.s", 3)], str(at))
            d = data(m.call("list_sources"))
            case("MCP keeps native paths with a URI editor attached",
                 forged in {s.get("path") for s in d.get("sources", [])}, str(d)[:300])
            at = dap.request("x16/sourceLocation", {"file": "solo.s", "line": 2})["body"]["locations"]
            case("DAP uses its own line base", [(x["address"], x["line"]) for x in at] == [(0x0A02, 2)], str(at))
            at = dap.request("x16/sourceLocation", {"address": "0x0A07"})["body"]["locations"]
            case("DAP gets a URI", at and at[0]["file"].startswith("file://"), str(at))
            back = dap.request("x16/sourceLocation", {"file": at[0]["file"], "line": 0})["body"]["locations"]
            case("and a URI is accepted", [x["address"] for x in back] == [0x0A07], str(back))
            refused = dap.request("mcp/readSource", {"file": "solo.s"})
            case("read_source is not a DAP request", refused.get("success") is False, str(refused)[:200])
            dap.close()

        put(os.path.join(root, "banka.dbg"), source_dbg(forged, bankb_size=6))
        banka = put(os.path.join(root, "banka.prg"), struct.pack("<H", 0xA000) + b"\xEA" * 4)
        with Mcp(["-fsroot", root]) as m:
            m.init()
            time.sleep(0.5)
            m.call("pause")
            m.call("write_memory", address=0, hex="05")
            m.call("load_prg", path=banka)
            at = data(m.call("source_location", file="solo.s", line=7)).get("locations", [])
            case("a known bank is reported",
                 sorted((x.get("segment"), x.get("bank")) for x in at) == [("BANKA", 5), ("BANKB", None)], str(at))
            at = data(m.call("source_location", file="solo.s", line=7, bank=5)).get("locations", [])
            case("a bank selects its segment", [x.get("segment") for x in at] == ["BANKA"], str(at))
            at = data(m.call("source_location", file="solo.s", line=7, bank=6)).get("locations", [])
            case("otherwise the one with no known bank", [x.get("segment") for x in at] == ["BANKB"], str(at))

        generated = ('version\tmajor=2,minor=0\n'
                     'info\tcsym=0,file=2,lib=0,line=2,mod=1,scope=1,seg=1,span=2,sym=0,type=0\n'
                     'file\tid=0,name="gen.c",size=10,mtime=0x00000000,mod=0\n'
                     'file\tid=1,name="gen.s",size=10,mtime=0x00000000,mod=0\n'
                     'seg\tid=0,name="CODE",start=0x000A00,size=0x0010,addrsize=absolute,type=ro,oname="gen.prg"\n'
                     'span\tid=0,seg=0,start=0,size=8,type=0\n'
                     'span\tid=1,seg=0,start=0,size=2,type=0\n'
                     'line\tid=0,file=0,line=1,type=1,count=1,span=0\n'
                     'line\tid=1,file=1,line=1,span=1\n'
                     'mod\tid=0,name="gen.o",file=0\n')
        put(os.path.join(root, "gen.dbg"), generated)
        gen_prg = put(os.path.join(root, "gen.prg"), prg_bytes)
        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", root]) as m:
            m.init()
            time.sleep(0.5)
            m.call("pause")
            m.call("load_prg", path=gen_prg)
            listed = {s.get("path") for s in data(m.call("list_sources")).get("sources", [])}
            case("MCP lists a generated file whether or not it is on disk", listed == {"gen.c", "gen.s"}, str(listed))
            dap = Dap(port)
            dap.request("initialize", {"adapterID": "test", "linesStartAt1": True, "columnsStartAt1": True})
            dap.request("attach", {})
            dap.request("configurationDone")
            shown = {s["path"] for s in dap.request("loadedSources")["body"]["sources"]}
            case("DAP still leaves it out", shown == {"gen.c"}, str(shown))
            dap.close()

        work = os.path.join(base, "work")
        put(os.path.join(work, "app.prg"), prg_bytes)
        put(os.path.join(work, "app.dbg"), K_PRG_DBG.replace("prog.s", "app.s").replace("prog.prg", "app.prg"))
        put(os.path.join(work, "app.s"), "from the program's folder\n")
        put(os.path.join(root, "app.s"), "a different file in the root\n")
        with Mcp(["-fsroot", root, "-prg", "app.prg"], cwd=work) as m:
            m.init()
            deadline = time.time() + 10
            while time.time() < deadline and not data(m.call("list_sources")).get("sources"):
                time.sleep(0.1)
            d = data(m.call("read_source", file="app.s"))
            case("a relative debug name is from the working folder",
                 d.get("lines") == ["from the program's folder"], str(d)[:200])


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
                for tool, spelling, extra in (("load_symbols", "\\??\\pipe\\{}", {}),
                                              ("load_prg", "\\??\\pipe\\{}", {}),
                                              ("load_symbols", "\\??\\GLOBAL\\pipe\\{}", {}),
                                              ("load_symbols", "\\??\\GLOBALROOT\\Device\\NamedPipe\\{}", {}),
                                              ("dump_memory", "\\??\\pipe\\{}\\out.bin",
                                               {"address": 0x0A00, "length": 1})):
                    with WinPipe() as pipe:
                        r = m.call(tool, path=spelling.format(pipe.name), **extra)
                        case(f"{tool} refuses {spelling}", is_error(r), str(r))
                        case(f"{tool} does not connect to {spelling}", not pipe.connected())

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


def wait_capture(m, state="done", secs=30):
    d = {}
    end = time.time() + secs
    while time.time() < end:
        d = data(m.call("profile_capture", top=5))
        if d.get("state") == state:
            break
        time.sleep(0.05)
    return d


def folder_link(target, name):
    try:
        os.symlink(target, name, target_is_directory=True)
        return True
    except (OSError, NotImplementedError):
        pass
    if os.name != "nt":
        return False
    return subprocess.run(["cmd", "/c", "mklink", "/J", name, target], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def test_perf_tools():
    print("performance tools")
    with Mcp() as m:
        m.init()
        d = data(m.call("perf_config"))
        case("perf_config with no arguments reports", d.get("budget", {}).get("enabled") is False, str(d)[:160])
        d = data(m.call("host_perf"))
        case("host_perf", "sections" in d and "frames" in d, str(d)[:160])
        case("host_perf takes no arguments", is_error(m.call("host_perf", reset=True)))
        d = data(m.call("profile_capture"))
        case("profile_capture starts idle", d.get("state") == "idle", str(d)[:160])
        for label, args in (("a low targetFps", {"targetFps": 0}),
                            ("a text targetFps", {"targetFps": "60"}),
                            ("an unknown idleMode", {"idleMode": "banana"}),
                            ("a short history", {"historyFrames": 1}),
                            ("an empty zone", {"zones": [{"start": 9, "end": 9}]}),
                            ("a misspelled zone key", {"zones": [{"start": 1, "end": 9, "bnak": 1}]}),
                            ("too many zones", {"zones": [{"start": 1, "end": 2}] * 17}),
                            ("overrunEvents", {"overrunEvents": True}),
                            ("a text enabled", {"enabled": "yes"})):
            case(f"perf_config refuses {label}", is_error(m.call("perf_config", **args)))
        for label, args in (("a text reset", {"reset": "yes"}),
                            ("a caller with no end", {"caller": {"start": 1}}),
                            ("a caller going backwards", {"caller": {"start": 9, "end": 1}}),
                            ("a caller with an extra key", {"caller": {"start": 1, "end": 2, "x": 1}}),
                            ("an empty dump", {"dump": ""})):
            case(f"perf_history refuses {label}", is_error(m.call("perf_history", **args)))
        for label, args in (("a misspelled start key", {"start": {"frame": 5}}),
                            ("zero frames", {"start": {"frames": 0}}),
                            ("zero seconds", {"start": {"seconds": 0}}),
                            ("an unknown recorder", {"start": {"recorders": ["banana"]}}),
                            ("start with stop", {"start": {"frames": 5}, "stop": True}),
                            ("start with export", {"start": {"frames": 5}, "export": {"json": "a.json"}}),
                            ("an unknown export", {"export": {"csv": "a.csv"}}),
                            ("export with nothing to export", {"export": {"json": "a.json"}})):
            case(f"profile_capture refuses {label}", is_error(m.call("profile_capture", **args)))
        case("nothing started", data(m.call("profile_capture")).get("state") == "idle")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        root = os.path.join(tmp, "root")
        outside = os.path.join(tmp, "outside")
        sibling = root + "2"
        for folder in (root, outside, sibling, os.path.join(root, "sub")):
            os.mkdir(folder)
        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", root]) as m:
            m.init()
            time.sleep(0.5)
            plant_loop(m)
            dap = Dap(port)
            dap.request("initialize", {"adapterID": "test", "linesStartAt1": False,
                                       "columnsStartAt1": False, "pathFormat": "uri"})
            dap.request("attach", {})
            dap.request("configurationDone")
            m.call("continue")

            # perf_config
            d = data(m.call("perf_config", enabled=True, targetFps=30, idleMode="markers",
                            zones=[{"name": "loop", "start": LOOP, "end": LOOP + 10}]))
            b = d.get("budget", {})
            case("perf_config applies", b.get("enabled") is True and b.get("targetFps") == 30 and
                 b.get("idleMode") == "markers", str(b))
            names = [z.get("name") for z in data(m.call("perf_stats")).get("zones", [])]
            case("a zone goes in", "loop" in names, str(names))
            r = dap.request("x16/perfConfig", {})
            case("MCP's switch leaves the editor's overrun events off",
                 (r or {}).get("body", {}).get("overrunEvents") is False, str(r)[:200])
            m.call("perf_config", targetFps=60, idleMode="auto", zones=[])
            dap.request("x16/perfConfig", {"enabled": True, "overrunEvents": False})
            d = data(m.call("perf_config", enabled=False))
            case("MCP's off leaves DAP's claim", d.get("budget", {}).get("enabled") is True, str(d)[:160])
            r = dap.request("x16/perfConfig", {"enabled": False})
            case("and DAP's off, with MCP's gone, stops it",
                 (r or {}).get("body", {}).get("budget", {}).get("enabled") is False, str(r)[:200])
            m.call("perf_config", enabled=True)
            r = dap.request("x16/perfConfig", {"enabled": False})
            case("DAP's off leaves MCP's claim",
                 (r or {}).get("body", {}).get("budget", {}).get("enabled") is True, str(r)[:200])
            m.call("perf_config", enabled=False)

            # perf_history
            d = data(m.call("perf_history", enabled=True, caller={"start": LOOP, "end": LOOP + 10}))
            case("perf_history arms", d.get("enabled") is True, str(d)[:160])
            end = time.time() + 15
            while time.time() < end and data(m.call("perf_history")).get("frames", 0) < 3:
                time.sleep(0.02)
            case("and counts frames", data(m.call("perf_history")).get("frames", 0) >= 3)
            hist = os.path.join(root, "hist.bin")
            d = data(m.call("perf_history", dump="hist.bin"))
            case("dump writes inside the root", d.get("dumped", 0) > 0 and os.path.isfile(hist) and
                 open(hist, "rb").read(4) == b"PHST", str(d)[:160])
            case("and says where", os.path.normcase(d.get("dumpPath", "").replace("/", os.sep)) ==
                 os.path.normcase(hist), str(d)[:160])
            before = open(hist, "rb").read()
            r = m.call("perf_history", dump="hist.bin")
            case("an existing dump is refused", is_error(r) and open(hist, "rb").read() == before)
            case("unless overwrite is given", not is_error(m.call("perf_history", dump="hist.bin", overwrite=True)))

            # A refused file stops the whole request.
            counted = data(m.call("perf_history")).get("frames", 0)
            r = m.call("perf_history", reset=True, dump=os.path.join("..", "esc.bin"))
            case("a refused dump does not reset", is_error(r) and
                 data(m.call("perf_history")).get("frames", 0) >= counted)

            # profile_capture: JSR $0A10, JMP $0A00; NOP, RTS. The tree needs a call.
            m.call("pause")
            m.call("write_memory", address=0x0A00, hex="20100A4C000A")
            m.call("write_memory", address=0x0A10, hex="EA60")
            m.call("set_registers", pc=0x0A00)
            m.call("continue")
            d = data(m.call("profile_capture", start={"frames": 30, "recorders": ["hist", "tree", "waterfall"]}))
            case("a capture answers at once, still running", d.get("state") == "running", str(d)[:200])
            case("with no report yet", "report" not in d, str(d)[:200])
            case("a second start is refused", is_error(m.call("profile_capture", start={"frames": 5})))
            for label, args in (("targetFps", {"targetFps": 20}), ("reset", {"reset": True}),
                                ("zones", {"zones": []}), ("historyFrames", {"historyFrames": 300})):
                case(f"perf_config {label} is refused during a capture",
                     is_error(m.call("perf_config", **args)))
            for label, args in (("enabled", {"enabled": False}), ("reset", {"reset": True}),
                                ("caller", {"caller": {"start": 0, "end": 0}})):
                case(f"perf_history {label} is refused during a capture",
                     is_error(m.call("perf_history", **args)))
            d = wait_capture(m)
            case("polling reaches done", d.get("state") == "done" and d.get("ended") == "frame limit", str(d)[:200])
            case("with a report", d.get("report", {}).get("rowCount", 0) > 0, str(d)[:200])

            d = data(m.call("profile_capture", top=1, export={"folded": "a.folded", "json": "a.json",
                                                              "trace": "a.trace", "run": "a.run"}))
            done = d.get("exported", {})
            for key, name in (("folded", "a.folded"), ("json", "a.json"), ("trace", "a.trace"), ("run", "a.run")):
                target = os.path.join(root, name)
                case(f"export {key} writes inside the root", done.get(key, -1) >= 0 and os.path.isfile(target), str(done))
                case(f"and says where {key} went",
                     os.path.normcase(done.get("paths", {}).get(key, "").replace("/", os.sep)) ==
                     os.path.normcase(target), str(done))
            before = open(os.path.join(root, "a.json"), "rb").read()
            r = m.call("profile_capture", export={"json": "a.json"})
            case("an existing export is refused", is_error(r) and open(os.path.join(root, "a.json"), "rb").read() == before)
            r = m.call("profile_capture", export={"json": "a.json"}, overwrite=True)
            case("unless overwrite is given", data(r).get("exported", {}).get("json", -1) >= 0, str(r)[:200])

            d = data(m.call("profile_capture", load="a.run", top=1))
            case("a saved run loads", d.get("report", {}).get("frames") == 30, str(d)[:200])
            case("its path is native with a URI client attached",
                 os.path.normcase(d.get("loadedRun", "").replace("/", os.sep)) ==
                 os.path.normcase(os.path.join(root, "a.run")), str(d.get("loadedRun")))

            # A refused file leaves a running capture running.
            m.call("profile_capture", start={"frames": 100000000})
            r = m.call("profile_capture", stop=True, export={"json": os.path.join("..", "esc.json")})
            case("a refused export does not stop the capture", is_error(r) and
                 data(m.call("profile_capture")).get("state") == "running")
            d = data(m.call("profile_capture", stop=True, top=0))
            case("this client can stop its own", d.get("state") == "done" and d.get("ended") == "stopped", str(d)[:200])

            # DAP's capture is not MCP's to end.
            dap.request("x16/capture", {"start": {"frames": 100000000}})
            r = m.call("profile_capture", stop=True)
            case("another client's capture is not stopped", is_error(r) and
                 dap.request("x16/capture", {"top": 0})["body"]["state"] == "running", str(r)[:200])
            r = dap.request("x16/capture", {"stop": True, "top": 0})
            case("DAP stops its own", (r or {}).get("body", {}).get("state") == "done", str(r)[:200])

            # Every path argument, every way out.
            secret = os.path.join(outside, "secret.run")
            with open(os.path.join(root, "a.run"), "rb") as src, open(secret, "wb") as dst:
                dst.write(src.read())
            victim = os.path.join(outside, "victim.bin")
            with open(victim, "wb") as f:
                f.write(b"keep")
            up = os.path.join(root, "up")
            has_folder_link = folder_link(outside, up)
            file_link = os.path.join(root, "file.lnk")
            try:
                os.symlink(victim, file_link)
                has_file_link = True
            except (OSError, NotImplementedError):
                has_file_link = False

            writers = (("dump", lambda path, **o: m.call("perf_history", dump=path, **o)),
                       ("folded", lambda path, **o: m.call("profile_capture", export={"folded": path}, **o)),
                       ("json", lambda path, **o: m.call("profile_capture", export={"json": path}, **o)),
                       ("trace", lambda path, **o: m.call("profile_capture", export={"trace": path}, **o)),
                       ("run", lambda path, **o: m.call("profile_capture", export={"run": path}, **o)))
            for arg, write in writers:
                for label, bad in (("..", os.path.join("..", "esc.bin")),
                                   ("a nested ..", os.path.join("sub", "..", "..", "esc.bin")),
                                   ("an absolute path outside", os.path.join(outside, "esc.bin")),
                                   ("a folder sharing the prefix", os.path.join(sibling, "esc.bin")),
                                   ("a missing folder", os.path.join("nope", "esc.bin"))):
                    case(f"{arg} refuses {label}", is_error(write(bad, overwrite=True)))
                if has_folder_link:
                    case(f"{arg} refuses a folder link out",
                         is_error(write(os.path.join("up", "esc.bin"), overwrite=True)))
                if has_file_link:
                    case(f"{arg} refuses a file link", is_error(write("file.lnk", overwrite=True)))
                if os.name == "nt":
                    for bad in ("NUL", "con.txt", "a.bin:hidden"):
                        case(f"{arg} refuses {bad}", is_error(write(bad, overwrite=True)))
            case("nothing escaped", not os.path.exists(os.path.join(tmp, "esc.bin")) and
                 not os.path.exists(os.path.join(outside, "esc.bin")) and not os.listdir(sibling))
            case("and the file outside is intact", open(victim, "rb").read() == b"keep")

            for label, bad in (("..", os.path.join("..", "outside", "secret.run")),
                               ("a nested ..", os.path.join("sub", "..", "..", "outside", "secret.run")),
                               ("an absolute path outside", secret)):
                case(f"load refuses {label}", is_error(m.call("profile_capture", load=bad)))
            if has_folder_link:
                case("load refuses a folder link out",
                     is_error(m.call("profile_capture", load=os.path.join("up", "secret.run"))))
            if has_file_link:
                run_link = os.path.join(root, "run.lnk")
                os.symlink(secret, run_link)
                case("load refuses a file link", is_error(m.call("profile_capture", load="run.lnk")))
            r = m.call("profile_capture", load=secret)
            missing = m.call("profile_capture", load=os.path.join(outside, "absent.run"))
            case("load: outside and missing read the same",
                 r.get("content") == missing.get("content"), f"{r} != {missing}")

            # The same requests from DAP reach the same places.
            target = os.path.join(outside, "dap.bin")
            r = dap.request("x16/perfHist", {"dump": target})
            case("DAP's dump is not confined", (r or {}).get("body", {}).get("dumped", -1) > 0 and
                 os.path.isfile(target), str(r)[:200])
            r = dap.request("x16/capture", {"load": secret, "top": 0})
            case("DAP's load is not confined", (r or {}).get("success") is True and
                 r["body"].get("loadedRun") == secret, str(r)[:200])
            paths = {k: os.path.join(outside, f"dap.{k}") for k in ("folded", "json", "trace", "run")}
            r = dap.request("x16/capture", {"export": paths, "top": 0})
            done = (r or {}).get("body", {}).get("exported", {})
            for key, path in paths.items():
                case(f"DAP's {key} export is not confined", done.get(key, -1) >= 0 and os.path.isfile(path), str(done))
            r = dap.request("x16/capture", {"export": {"json": paths["json"]}, "top": 0})
            case("and DAP still overwrites", (r or {}).get("body", {}).get("exported", {}).get("json", -1) >= 0, str(r)[:200])
            dap.close()

    # What a client claimed ends with its lease; what another holds does not.
    port = free_port()
    with HttpMcp(["-debugport", str(port)], env={"X16_MCP_LEASE_MS": "700"}) as m:
        m.init()
        time.sleep(0.4)
        m.call("perf_config", enabled=True)
        d = data(m.call("profile_capture", start={"frames": 100000000}))
        case("a capture on a lease is running", d.get("state") == "running", str(d)[:160])
        time.sleep(1.5)
        dap = Dap(port)
        r = dap.request("x16/capture", {"top": 0})
        body = (r or {}).get("body", {})
        case("an expired lease ends its capture", body.get("state") == "done" and body.get("ended") == "stopped", str(r)[:200])
        r = dap.request("x16/perfConfig", {})
        case("and its claim on the statistics",
             (r or {}).get("body", {}).get("budget", {}).get("enabled") is False, str(r)[:200])

        m.call("get_status")
        dap.request("x16/capture", {"start": {"frames": 100000000}})
        time.sleep(1.5)
        r = dap.request("x16/capture", {"top": 0})
        case("an expired lease leaves another client's capture",
             (r or {}).get("body", {}).get("state") == "running", str(r)[:200])
        dap.request("x16/capture", {"stop": True, "top": 0})
        dap.close()


def fat32_image(path):
    # MBR, one type 0x0C partition, 1 sector per cluster, /HELLO.PRG in cluster 3.
    start, reserved, clusters = 2048, 32, 66000
    fat_sectors = ((clusters + 2) * 4 + 511) // 512
    total = reserved + 2 * fat_sectors + clusters
    data_lba = start + reserved + 2 * fat_sectors

    mbr = bytearray(512)
    mbr[0x1BE + 4] = 0x0C
    struct.pack_into("<II", mbr, 0x1BE + 8, start, total)
    mbr[510:512] = b"\x55\xAA"

    vbr = bytearray(512)
    vbr[0:3] = b"\xEB\x58\x90"
    vbr[3:11] = b"X16TEST "
    struct.pack_into("<HBHBHHBHHHII", vbr, 11, 512, 1, reserved, 2, 0, 0, 0xF8, 0, 63, 255, start, total)
    struct.pack_into("<IHHIHH", vbr, 36, fat_sectors, 0, 0, 2, 1, 6)
    vbr[66] = 0x29
    vbr[71:82] = b"X16TEST    "
    vbr[82:90] = b"FAT32   "
    vbr[510:512] = b"\x55\xAA"

    info = bytearray(512)
    struct.pack_into("<I", info, 0, 0x41615252)
    struct.pack_into("<III", info, 484, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF)
    struct.pack_into("<I", info, 508, 0xAA550000)

    fat = bytearray(512)
    struct.pack_into("<IIII", fat, 0, 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF)

    root = bytearray(512)
    root[0:11] = b"HELLO   PRG"
    root[11] = 0x20
    struct.pack_into("<HHI", root, 20, 0, 0, 0)
    struct.pack_into("<HI", root, 26, 3, 4)

    with open(path, "wb") as f:
        for lba, sector in ((0, mbr), (start, vbr), (start + 1, info), (start + 6, vbr),
                            (start + 7, info), (start + reserved, fat),
                            (start + reserved + fat_sectors, fat), (data_lba, root),
                            (data_lba + 1, b"\x01\x08\x00\x00" + bytes(508))):
            f.seek(lba * 512)
            f.write(sector)
        f.truncate((start + total) * 512)


def wait_sprite_frame(m):
    end = time.time() + 20
    while time.time() < end:
        d = data(m.call("sprite_mux_stats"))
        if "lastFrame" in d:
            return d
        time.sleep(0.02)
    return {}


def test_device_stats():
    print("device statistics")
    with Mcp() as m:
        m.init()
        r = m.call("get_sdcard", files="all", history=5)
        case("no card is not an error", not is_error(r) and data(r).get("attached") is False, str(r)[:200])
        case("and says so", r.get("content", [{}])[0].get("text") == "No SD card is attached.", str(r)[:200])
        case("and reports nothing else", "card" not in data(r) and "index" not in data(r), str(r)[:200])
        for label, args in (("an unknown files", {"files": "banana"}), ("a number for files", {"files": 1}),
                            ("a negative history", {"history": -1}), ("too much history", {"history": 129}),
                            ("a fractional history", {"history": 1.5}), ("a text history", {"history": "5"}),
                            ("reindex", {"reindex": True}), ("an unknown argument", {"path": "a.img"})):
            case(f"get_sdcard refuses {label}", is_error(m.call("get_sdcard", **args)))
        case("sprite_mux_stats takes no arguments", is_error(m.call("sprite_mux_stats", reset=True)))
        for label, args in (("a text enabled", {"enabled": "yes"}), ("a number for reset", {"reset": 1}),
                            ("a low burstGap", {"burstGap": 7}), ("a high burstGap", {"burstGap": 2049}),
                            ("a fractional burstGap", {"burstGap": 8.5}), ("a text burstGap", {"burstGap": "128"}),
                            ("an unknown argument", {"gap": 128})):
            case(f"sprite_mux_config refuses {label}", is_error(m.call("sprite_mux_config", **args)))
        d = data(m.call("sprite_mux_config"))
        case("sprite_mux_config with no arguments reports",
             d.get("enabled") is False and d.get("burstGap") == 128, str(d))

    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        image = os.path.join(tmp, "card.img")
        fat32_image(image)
        port = free_port()
        with Mcp(["-debugport", str(port), "-sdcard", image]) as m:
            m.init()
            end = time.time() + 30
            while time.time() < end and not data(m.call("get_sdcard")).get("history"):
                time.sleep(0.05)
            time.sleep(1)
            dap = Dap(port)
            dap.request("initialize", {"adapterID": "test", "linesStartAt1": False,
                                       "columnsStartAt1": False, "pathFormat": "uri"})
            dap.request("attach", {})
            dap.request("configurationDone")
            m.call("pause")

            d = data(m.call("get_sdcard", files="all"))
            card, index = d.get("card", {}), d.get("index", {})
            case("a card is attached", d.get("attached") is True, str(d)[:200])
            case("its image is a native path with a URI client attached",
                 os.path.normcase(card.get("image", "")) == os.path.normcase(image), str(card.get("image")))
            case("its size is reported", card.get("imageSize") == os.path.getsize(image), str(card)[:200])
            case("the index is ready", index.get("ready") is True and index.get("type") == "FAT32", str(index)[:200])
            paths = [f.get("path") for f in index.get("files", [])]
            case("all lists the file by its path in the image", "/HELLO.PRG" in paths, str(paths))
            history = d.get("history", [])
            case("the boot's reads are in the history", any(h.get("op") == "read" for h in history), str(history)[:200])
            inner = paths + [h["path"] for h in history if "path" in h]
            case("image paths are not converted for the client",
                 all(p.startswith("/") and "file:" not in p for p in inner), str(inner)[:200])

            d = data(m.call("get_sdcard"))
            touched = [f.get("path") for f in d.get("index", {}).get("files", [])]
            case("touched is the default", "/" in touched and "/HELLO.PRG" not in touched, str(touched))
            d = data(m.call("get_sdcard", files="none", history=1))
            case("none lists no files", "files" not in d.get("index", {}), str(d)[:200])
            case("history is limited", len(d.get("history", [])) == 1, str(d.get("history")))
            case("history 0 is empty", data(m.call("get_sdcard", history=0)).get("history") == [])

            def counters():
                c = data(m.call("get_sdcard", files="none", history=0)).get("card", {})
                return c.get("commands"), c.get("blocksRead"), c.get("blocksWritten")
            before = counters()
            case("reading moves no counter", before[0] and counters() == before, str(before))
            r = dap.request("x16/sdcard", {"files": "none", "history": 0})
            case("DAP reads the same image path",
                 (r or {}).get("body", {}).get("card", {}).get("image") == card.get("image"), str(r)[:200])
            m.call("continue")

            # An MCP claim, and DAP's beside it. Probed with config: DAP's stats would arm.
            def dap_on():
                return (dap.request("x16/spriteMuxConfig", {}) or {}).get("body", {}).get("enabled")
            case("recording starts off", dap_on() is False)
            d = data(m.call("sprite_mux_stats"))
            case("the first poll turns it on", d.get("enabled") is True and d.get("memoryBytes", 0) > 0, str(d)[:200])
            d = wait_sprite_frame(m)
            f = d.get("lastFrame", {})
            case("a frame is reported", all(k in f for k in ("frame", "effectiveSprites", "peakLine", "linesExhausted")),
                 str(d)[:200])
            case("frame stays the machine's frame number", isinstance(d.get("frame"), int), str(d.get("frame")))
            m.call("pause")
            mine = data(m.call("sprite_mux_stats")).get("lastFrame", {})
            theirs = (dap.request("x16/spriteMuxStats") or {}).get("body", {}).get("frame", {})
            case("a scanline is not shifted for a 0-based client",
                 "peakLine" in mine and mine.get("peakLine") == theirs.get("peakLine"), f"{mine} {theirs}")
            d = data(m.call("sprite_mux_config", reset=True))
            case("reset keeps recording on", d.get("enabled") is True, str(d))
            d = data(m.call("sprite_mux_stats"))
            case("and discards the frame", "lastFrame" not in d and isinstance(d.get("frame"), int), str(d)[:200])
            m.call("continue")

            d = data(m.call("sprite_mux_config", burstGap=256))
            r = dap.request("x16/spriteMuxConfig", {})
            case("burstGap is shared", d.get("burstGap") == 256 and (r or {}).get("body", {}).get("burstGap") == 256,
                 str(r)[:200])
            m.call("sprite_mux_config", burstGap=128)

            d = data(m.call("sprite_mux_config", enabled=False))
            case("MCP's off stops its own claim", d.get("enabled") is False and dap_on() is False, str(d))
            dap.request("x16/spriteMuxConfig", {"enabled": True})
            d = data(m.call("sprite_mux_config", enabled=False))
            case("MCP's off leaves DAP's claim", d.get("enabled") is True, str(d))
            r = dap.request("x16/spriteMuxConfig", {"enabled": False})
            case("and DAP's off, with MCP's gone, stops it",
                 (r or {}).get("body", {}).get("enabled") is False, str(r)[:200])
            m.call("sprite_mux_config", enabled=True)
            r = dap.request("x16/spriteMuxConfig", {"enabled": False})
            case("DAP's off leaves MCP's claim", (r or {}).get("body", {}).get("enabled") is True, str(r)[:200])
            dap.close()
            time.sleep(0.3)
            dap = Dap(port)
            case("a DAP disconnect leaves MCP's claim", dap_on() is True)
            m.call("sprite_mux_config", enabled=False)
            dap.close()

    # What a client claimed ends with its lease; what another holds does not.
    port = free_port()
    with HttpMcp(["-debugport", str(port)], env={"X16_MCP_LEASE_MS": "700"}) as m:
        m.init()
        time.sleep(0.4)
        d = data(m.call("sprite_mux_stats"))
        case("a poll on a lease arms recording", d.get("enabled") is True, str(d)[:160])
        time.sleep(1.5)
        dap = Dap(port)
        r = dap.request("x16/spriteMuxConfig", {})
        case("an expired lease releases a poll's claim", (r or {}).get("body", {}).get("enabled") is False, str(r)[:200])

        d = data(m.call("sprite_mux_config", enabled=True))
        case("a config claim on a lease", d.get("enabled") is True, str(d)[:160])
        time.sleep(1.5)
        r = dap.request("x16/spriteMuxConfig", {})
        case("an expired lease releases a config claim", (r or {}).get("body", {}).get("enabled") is False, str(r)[:200])

        m.call("sprite_mux_config", enabled=True)
        dap.request("x16/spriteMuxConfig", {"enabled": True})
        time.sleep(1.5)
        r = dap.request("x16/spriteMuxConfig", {})
        case("an expired lease leaves DAP's claim", (r or {}).get("body", {}).get("enabled") is True, str(r)[:200])
        dap.request("x16/spriteMuxConfig", {"enabled": False})
        dap.close()


def bp_entry(m, pc):
    for b in data(m.call("list_breakpoints")).get("breakpoints", []):
        if b.get("pc") == pc:
            return b
    return {}


def watch_entry(m, address):
    for w in data(m.call("list_breakpoints")).get("watchpoints", []):
        if w.get("addr") == address:
            return w
    return {}


def runs_on(m):
    m.call("continue")
    time.sleep(0.25)
    return data(m.call("get_status")).get("paused") is False


def stops_at(m, pc=None, reason="breakpoint"):
    since = data(m.call("continue")).get("stopId")
    d = data(m.call("wait_for_stop", sinceStopId=since))
    return d.get("completed") is True and d.get("stopReason") == reason and (pc is None or d.get("pc") == pc)


def test_enable_breakpoint():
    print("enable_breakpoint")
    with Mcp() as m:
        m.init()
        time.sleep(0.5)
        plant_loop(m)
        at = LOOP + 4
        m.call("set_breakpoint", address=at, condition="A == $05")
        d = data(m.call("enable_breakpoint", address=at, enabled=False))
        case("disable reports it", d.get("kind") == "exec" and d.get("enabled") is False, str(d))
        e = bp_entry(m, at)
        case("disabled in the list", e.get("enabled") is False, str(e))
        case("the condition is kept", e.get("condition") is not None, str(e))
        case("a disabled breakpoint does not stop", runs_on(m))
        m.call("pause")
        m.call("set_registers", pc=LOOP)
        d = data(m.call("enable_breakpoint", address=at, enabled=True))
        case("enable reports it", d.get("enabled") is True, str(d))
        case("it stops again", stops_at(m, at))
        m.call("enable_breakpoint", address=at, enabled=False)
        m.call("set_breakpoint", address=at, condition="A == $05")
        case("setting it again enables it", bp_entry(m, at).get("enabled") is True)
        m.call("remove_breakpoint", address=at)

        m.call("set_registers", pc=LOOP)
        m.call("set_breakpoint", address=0x0910, kind="write")
        d = data(m.call("enable_breakpoint", address=0x0910, kind="write", enabled=False))
        case("a watch can be disabled", d.get("kind") == "write" and d.get("enabled") is False, str(d))
        case("disabled in the list", watch_entry(m, 0x0910).get("active") is False)
        case("a disabled watch does not stop", runs_on(m))
        m.call("pause")
        m.call("set_registers", pc=LOOP)
        m.call("enable_breakpoint", address=0x0910, kind="write", enabled=True)
        case("it fires again", stops_at(m, reason="data breakpoint"))
        m.call("enable_breakpoint", address=0x0910, kind="write", enabled=False)
        d = data(m.call("set_breakpoint", address=0x0910, kind="write"))
        case("setting a watch again arms it", d.get("kind") == "write" and
             watch_entry(m, 0x0910).get("active") is True, str(d))
        r = m.call("enable_breakpoint", address=0x0910, kind="read", enabled=False)
        case("the wrong access kind is refused", is_error(r), str(r)[:160])
        m.call("remove_breakpoint", address=0x0910, kind="write")

        for label, args in (("no enabled", {"address": LOOP}),
                            ("a text enabled", {"address": LOOP, "enabled": "no"}),
                            ("a number enabled", {"address": LOOP, "enabled": 0}),
                            ("no address", {"enabled": True}),
                            ("nothing there", {"address": 0x0BAD, "enabled": False}),
                            ("no watch there", {"address": 0x0BAD, "kind": "write", "enabled": False}),
                            ("an unknown argument", {"address": LOOP, "enabled": True, "banana": 1})):
            case(f"enable_breakpoint refuses {label}", is_error(m.call("enable_breakpoint", **args)))

    port = free_port()
    with Mcp(["-bp", "0950", "-debugport", str(port)]) as m:
        m.init()
        time.sleep(0.3)
        m.call("pause")
        r = m.call("enable_breakpoint", address=0x0950, enabled=False)
        case("another owner's breakpoint is refused", is_error(r), str(r)[:160])
        m.call("set_breakpoint", address=0x0950)
        d = data(m.call("enable_breakpoint", address=0x0950, enabled=False))
        case("disabling a shared breakpoint turns off this client's claim",
             d.get("enabled") is False and d.get("stillArmed") is True, str(d))
        case("and the other owner keeps it enabled", bp_entry(m, 0x0950).get("enabled") is True)

        dap = Dap(port)
        dap.session()
        info = dap.request("dataBreakpointInfo", {"name": "$0A40"})["body"]
        rows = dap.request("setDataBreakpoints", {"breakpoints": [{"dataId": info["dataId"],
                                                                   "accessType": "write"}]})["body"]["breakpoints"]
        case("DAP holds a watch", rows and rows[0].get("verified") is True, str(rows))
        r = m.call("enable_breakpoint", address=0x0A40, kind="write", enabled=False)
        case("another owner's watch is refused", is_error(r), str(r)[:160])
        m.call("set_breakpoint", address=0x0A40, kind="write")
        r = m.call("enable_breakpoint", address=0x0A40, kind="write", enabled=False)
        case("a shared watch is refused", is_error(r), str(r)[:160])
        case("and stays active", watch_entry(m, 0x0A40).get("active") is True)
        dap.close()


def test_enable_per_owner():
    print("enable state per owner")
    port = free_port()
    with Mcp(["-debugport", str(port)]) as m:
        m.init()
        time.sleep(0.5)
        plant_loop(m)
        at = LOOP + 4
        dap = Dap(port)
        dap.session()

        def dap_holds(*addresses):
            dap.request("setInstructionBreakpoints",
                        {"breakpoints": [{"instructionReference": hex(a)} for a in addresses]})

        def rewind():
            m.call("pause")
            m.call("set_registers", pc=LOOP)

        m.call("set_breakpoint", address=at)
        m.call("enable_breakpoint", address=at, enabled=False)
        dap_holds(at)
        case("a second owner's claim is live", bp_entry(m, at).get("enabled") is True)
        rewind()
        case("and stops the machine", stops_at(m, at))

        dap_holds()
        e = bp_entry(m, at)
        case("its removal restores the first owner's disabled state",
             e.get("enabled") is False and e.get("owners") == ["mcp"], str(e))
        rewind()
        case("which does not stop", runs_on(m))

        dap_holds(at)
        d = data(m.call("enable_breakpoint", address=at, enabled=True))
        case("a claim can be enabled beside another owner", d.get("enabled") is True, str(d))
        d = data(m.call("enable_breakpoint", address=at, enabled=False))
        case("and disabled beside a live one", d.get("stillArmed") is True, str(d))
        rewind()
        case("the live owner still stops", stops_at(m, at))

        dap.close()
        time.sleep(0.4)
        e = bp_entry(m, at)
        case("session end leaves the disabled claim off",
             e.get("enabled") is False and e.get("owners") == ["mcp"], str(e))
        rewind()
        case("and it does not stop", runs_on(m))

        rewind()
        m.call("set_breakpoint", address=at)
        case("the owner's own set arms it", bp_entry(m, at).get("enabled") is True)
        case("and it stops", stops_at(m, at))
        d = data(m.call("remove_breakpoint", address=at))
        case("removal still works", d.get("removed") is True and d.get("stillArmed") is False, str(d))
        case("and the entry is gone", bp_entry(m, at) == {})


DUMP_REGIONS =[{"address": "$0A00", "length": 2}, {"address": "1000", "length": 3, "space": "vram"}]


def plant_dump_data(m):
    m.call("write_memory", address=0x0A00, hex="CAFE")
    m.call("write_memory", address="1000", hex="010203", space="vram")


def records_ok(raw, count):
    if len(raw) != count * 9:
        return False
    return all(raw[i * 9:i * 9 + 4] == struct.pack("<I", i) and
               raw[i * 9 + 4:i * 9 + 9] == bytes.fromhex("CAFE010203") for i in range(count))


def test_dump_breakpoint():
    print("dump breakpoints")
    lease = {"X16_MCP_LEASE_MS": "700"}
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        root = os.path.join(tmp, "root")
        outside = root + "2"            # shares the prefix on purpose
        os.mkdir(root)
        os.mkdir(outside)
        os.mkdir(os.path.join(root, "sub"))
        at = LOOP + 2

        with Mcp(["-fsroot", root]) as m:
            m.init()
            time.sleep(0.5)
            plant_loop(m)
            plant_dump_data(m)
            target = os.path.join(root, "d.bin")
            d = data(m.call("set_breakpoint", address=at, dump={"file": "d.bin", "regions": DUMP_REGIONS}))
            case("the reply names the file", os.path.normcase(d.get("dumpFile", "").replace("/", os.sep)) ==
                 os.path.normcase(target), str(d))
            case("nothing else stops there", d.get("alsoStops") is False, str(d))
            case("it runs on", runs_on(m))
            m.call("pause")
            e = bp_entry(m, at).get("dump", {})
            writes = e.get("writes", 0)
            case("the list counts its writes", writes > 0 and e.get("full") is False, str(e))
            case("and names its owner", e.get("owner") == ["mcp"], str(e))
            r = m.call("set_breakpoint", address=at, dump={"file": "again.bin", "regions": DUMP_REGIONS})
            case("a second dump is refused", is_error(r) and not os.path.exists(os.path.join(root, "again.bin")),
                 str(r)[:160])
            m.call("remove_breakpoint", address=at)
            with open(target, "rb") as f:
                case("each record is an index and the regions", records_ok(f.read(), writes))

            m.call("set_registers", pc=LOOP)
            m.call("set_breakpoint", address=at, dump={"file": "plain.bin", "regions": DUMP_REGIONS})
            m.call("set_breakpoint", address=at)
            case("setting it plain drops the dump", "dump" not in bp_entry(m, at))
            case("and it stops", stops_at(m, at))
            m.call("remove_breakpoint", address=at)

            kept = os.path.join(root, "kept.bin")
            with open(kept, "wb") as f:
                f.write(b"keep")
            bad_files = (("..", os.path.join("..", "esc.bin")),
                         ("a nested ..", os.path.join("sub", "..", "..", "esc.bin")),
                         ("an absolute path outside", os.path.join(tmp, "esc.bin")),
                         ("a folder sharing the prefix", os.path.join(outside, "esc.bin")),
                         ("a missing folder", os.path.join("nope", "x.bin")),
                         ("the root itself", "."),
                         ("an existing file", "kept.bin"))
            for label, bad in bad_files:
                r = m.call("set_breakpoint", address=at, dump={"file": bad, "regions": DUMP_REGIONS})
                case(f"dump refuses {label}", is_error(r), str(r)[:160])
                case("and arms nothing", bp_entry(m, at) == {})
            case("nothing escaped", not os.path.exists(os.path.join(tmp, "esc.bin")) and not os.listdir(outside))
            with open(kept, "rb") as f:
                case("the existing file is intact", f.read() == b"keep")

            up = os.path.join(root, "up")
            if folder_link(outside, up):
                r = m.call("set_breakpoint", address=at,
                           dump={"file": os.path.join("up", "esc.bin"), "regions": DUMP_REGIONS})
                case("dump refuses a folder link out", is_error(r) and not os.listdir(outside), str(r)[:160])
            link = os.path.join(root, "link.bin")
            dangling = os.path.join(outside, "dangling.bin")
            try:
                os.symlink(dangling, link)
                has_link = True
            except (OSError, NotImplementedError):
                has_link = False
            if has_link:
                r = m.call("set_breakpoint", address=at, dump={"file": "link.bin", "regions": DUMP_REGIONS})
                case("dump refuses a file link, even dangling", is_error(r) and not os.path.exists(dangling),
                     str(r)[:160])
            if os.name == "nt":
                for bad in ("NUL", "con.txt", "d2.bin:hidden"):
                    r = m.call("set_breakpoint", address=at, dump={"file": bad, "regions": DUMP_REGIONS})
                    case(f"dump refuses {bad}", is_error(r), str(r)[:160])

            region = {"address": 0x0A00, "length": 1}
            for label, args in (("a watch", {"kind": "write", "dump": {"file": "w.bin", "regions": [region]}}),
                                ("no file", {"dump": {"regions": [region]}}),
                                ("no regions", {"dump": {"file": "w.bin"}}),
                                ("empty regions", {"dump": {"file": "w.bin", "regions": []}}),
                                ("too many regions", {"dump": {"file": "w.bin", "regions": [region] * 17}}),
                                ("a long region", {"dump": {"file": "w.bin", "regions": [
                                    {"address": 0x0A00, "length": 4097}]}}),
                                ("an unknown dump key", {"dump": {"file": "w.bin", "regions": [region], "x": 1}}),
                                ("an unknown region key", {"dump": {"file": "w.bin", "regions": [
                                    {"address": 0x0A00, "bnak": 1}]}})):
                r = m.call("set_breakpoint", address=at, **args)
                case(f"dump refuses {label}", is_error(r) and not os.path.exists(os.path.join(root, "w.bin")),
                     str(r)[:160])

            m.call("set_breakpoint", address=at)
            m.call("enable_breakpoint", address=at, enabled=False)
            m.call("set_breakpoint", address=at, dump={"file": "kept.bin", "regions": DUMP_REGIONS})
            e = bp_entry(m, at)
            case("a refused dump leaves a held breakpoint as it was",
                 e.get("enabled") is False and "dump" not in e, str(e))

        with Mcp(["-fsroot", root, "-bp", "0902"]) as m:
            m.init()
            time.sleep(0.5)
            plant_loop(m)
            plant_dump_data(m)
            d = data(m.call("set_breakpoint", address=at, dump={"file": "both.bin", "regions": DUMP_REGIONS}))
            case("the reply says another owner stops here", d.get("alsoStops") is True, str(d))
            case("another owner's plain breakpoint stops", stops_at(m, at))
            case("and the dump records that hit", bp_entry(m, at).get("dump", {}).get("writes") == 1)
            m.call("remove_breakpoint", address=at)
            e = bp_entry(m, at)
            case("removing it leaves the other owner", e.get("owners") and "mcp" not in e["owners"] and
                 "dump" not in e, str(e))
            with open(os.path.join(root, "both.bin"), "rb") as f:
                case("one record", records_ok(f.read(), 1))

        port = free_port()
        with HttpMcp(["-fsroot", root, "-debugport", str(port), "-bp", "0902"], env=lease) as m:
            m.init()
            time.sleep(0.4)
            plant_loop(m)
            released = os.path.join(root, "lease.bin")
            m.call("set_breakpoint", address=at, dump={"file": "lease.bin", "regions": DUMP_REGIONS})
            m.call("set_breakpoint", address=0x0960)
            time.sleep(1.5)
            dap = Dap(port)
            rows = (dap.request("x16/listBreakpoints") or {}).get("body", {}).get("breakpoints", [])
            case("a lease ending releases its breakpoints",
                 not [b for b in rows if "mcp" in b.get("owners", [])], str(rows)[:200])
            kept_bp = [b for b in rows if b.get("pc") == at]
            case("and its dump, leaving the other owner", kept_bp and "dump" not in kept_bp[0], str(kept_bp))
            if os.name == "nt":
                try:
                    os.remove(released)
                    closed = True
                except OSError:
                    closed = False
                case("the file is closed", closed)
            dap.close()


def test_evaluate():
    print("evaluate")
    labels = ("al 000600 .start\n"
              "al 000610 .bp_clear\n"
              "al 00A000 .banked\n")
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "prog.lbl")
        with open(path, "w") as f:
            f.write(labels)
        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", tmp]) as m:
            m.init()
            time.sleep(0.5)
            plant_loop(m)
            dap = Dap(port)
            dap.session()

            def value(expr):
                return data(m.call("evaluate", expression=expr)).get("value")

            case("arithmetic", value("2+3*4") == 14)
            case("wraps at 32 bits", value("$FFFFFFFF+1") == 0)
            case("unsigned", value("-1") == 0xFFFFFFFF and value("-1<0") == 0)
            d = data(m.call("evaluate", expression="$C000"))
            case("$addr is a number", d.get("value") == 0xC000 and d.get("text") == "$C000 (49152)", str(d))
            m.call("write_memory", address="$0500", hex="1234")
            case("byte[]", value("byte[$0500]") == 0x12)
            case("word[]", value("word[$0500]") == 0x3412)
            m.call("set_registers", a=0x21, x=2)
            case("registers", value("A") == 0x21 and value("x*2") == 4)
            case("PC", value("PC") == LOOP)
            m.call("write_memory", address=0x1000, space="vram", hex="5A")
            case("vbyte[]", value("vbyte[$1000]") == 0x5A)

            m.call("load_symbols", path=path, bank=3)
            case("a symbol", value("start") == 0x600 and value("start+4") == 0x604)
            d = data(m.call("evaluate", expression="start"))
            case("no bank below the banked windows", "bank" not in d and d.get("text") == "$0600 (1536)", str(d))
            d = data(m.call("evaluate", expression="banked+1"))
            case("a symbol keeps its bank", d.get("value") == 0xA001 and d.get("bank") == 3, str(d))
            case("never hex", is_error(m.call("evaluate", expression="beef")))
            case("a label with a command's name is only a label", value("bp_clear") == 0x610)

            for bad in ("1/0", "1%0", "(1", "", "nosuch", "A=5", "x" * 300, "(" * 40 + "1" + ")" * 40):
                case(f"{bad[:12]!r} is an error", is_error(m.call("evaluate", expression=bad)))
            case("no expression is an error", is_error(m.call("evaluate")))
            case("a number is not an expression", is_error(m.call("evaluate", expression=5)))
            case("an unknown argument is refused",
                 is_error(m.call("evaluate", expression="1", context="repl")))
            case("A=5 assigned nothing", value("A") == 0x21)
            r = dap.request("mcp/evaluate", {"expression": "1"})
            case("a DAP client cannot call the MCP request", bool(r) and not r.get("success"), str(r)[:120])

            m.call("set_breakpoint", address=LOOP + 4)
            dap.request("setInstructionBreakpoints", {"breakpoints": [{"instructionReference": "0x0910"}]})
            m.call("set_breakpoint", address=0x0700, kind="write")
            held = data(m.call("list_breakpoints"))
            case("every owner's breakpoints are set",
                 len(held.get("breakpoints", [])) == 2 and len(held.get("watchpoints", [])) == 1, str(held)[:200])
            cycles = data(m.call("get_status")).get("cycles")
            for command in ("reset", "bp_add C000", "bp_add 0900", "bp_remove 0910", "bp_list",
                            "regs_all", "watch_add 0500", "watch_remove 0700", "watch_list",
                            "watch_clear", "vram 1000", "vera_reg", "vera_line 0"):
                case(f"{command} is not a command here", is_error(m.call("evaluate", expression=command)))
            r = m.call("evaluate", expression="bp_clear")
            case("bp_clear only reads a label", not is_error(r) and data(r).get("value") == 0x610, str(r)[:160])
            after = data(m.call("list_breakpoints"))
            case("no breakpoint or watch changed",
                 after.get("breakpoints") == held.get("breakpoints")
                 and after.get("watchpoints") == held.get("watchpoints"), str(after)[:200])
            case("nothing ran or reset", data(m.call("get_status")).get("cycles") == cycles)

            for reg, at in (("$9F20", "00"), ("$9F21", "10"), ("$9F22", "10")):
                m.call("write_memory", address=reg, hex=at)
            for _ in range(3):
                m.call("evaluate", expression="byte[$9F23]")
            case("reading the data port does not advance it",
                 value("byte[$9F20]") == 0 and value("byte[$9F21]") == 0x10)
            dap.close()

    port = free_port()
    with Mcp(["-mcp-readonly", "-debugport", str(port)]) as m:
        m.init()
        time.sleep(0.4)
        case("works read-only", data(m.call("evaluate", expression="1+1")).get("value") == 2)
        case("reads memory read-only", "value" in data(m.call("evaluate", expression="byte[$0400]")))
        case("still no commands", is_error(m.call("evaluate", expression="reset")))


def log_lines(m, cursor=None):
    args = {} if cursor is None else {"cursor": cursor}
    return data(m.call("read_log", **args))


def test_logpoints():
    print("logpoints")
    with Mcp() as m:
        m.init()
        plant_loop(m)
        case("nothing logged yet", log_lines(m).get("entries") == [], str(log_lines(m)))

        for bad, why in (("{", "unmatched"), ("}", "unmatched"), ("{}", "empty")):
            r = m.call("set_breakpoint", address=LOOP, log=bad)
            case(f"message {bad!r} is refused", is_error(r) and why in r["content"][0]["text"], str(r)[:160])
        case("and nothing is armed", not data(m.call("list_breakpoints")).get("breakpoints"))
        case("log needs a string", is_error(m.call("set_breakpoint", address=LOOP, log=5)))
        case("a watch takes no log", is_error(m.call("set_breakpoint", address=0x0910, kind="write", log="x")))

        d = data(m.call("set_breakpoint", address=LOOP, log="A={A} b={byte[$0910]} {{x}} {nosuchname}"))
        case("set reports the log", d.get("log", "").startswith("A={A}"), str(d))
        case("and nobody else stops it", d.get("alsoStops") is False, str(d))
        listed = data(m.call("list_breakpoints"))["breakpoints"]
        case("the listing has the message",
             listed and listed[0].get("logs") == [{"owner": "mcp", "message": "A={A} b={byte[$0910]} {{x}} {nosuchname}"}],
             str(listed))

        m.call("write_memory", address=0x0910, hex="07")
        r = data(m.call("run_frames", frames=2))
        case("the machine does not stop for it", r.get("reason") not in ("breakpoint",), str(r))
        got = log_lines(m)
        lines = got.get("entries", [])
        case("it logged", len(lines) > 0, str(got)[:200])
        text = lines[-1]["text"] if lines else ""
        case("A and memory print in hex", "A=$05 b=$05" in text, text)
        case("escapes print as braces", "{x}" in text, text)
        case("a bad expression prints its error", "{nosuchname:" in text, text)
        case("a line names its address", bool(lines) and lines[-1]["address"] == "$0900", str(lines[-1:]))
        case("a line names its owner", bool(lines) and lines[-1]["owner"] == "mcp", str(lines[-1:]))

        # Reading consumes nothing, so two readers see the same lines.
        again = log_lines(m)
        case("reading twice gives the same lines", again.get("entries") == got.get("entries"))

        # Past the size of the ring: reports loss, and the cursor carries on from it.
        m.call("run_frames", frames=30)
        late = log_lines(m, 0)
        case("an old cursor reports what it lost", late.get("lost", 0) > 0, str({k: late.get(k) for k in ("lost", "cursor", "more")}))
        case("and starts at the oldest line held",
             bool(late.get("entries")) and late["entries"][0]["seq"] == late["lost"], str(late.get("entries", [{}])[0]))
        case("a limit is honoured",
             len(data(m.call("read_log", cursor=late["lost"], limit=3))["entries"]) == 3)
        page = data(m.call("read_log", cursor=late["lost"], limit=3))
        case("more says there is more", page.get("more") is True, str(page.get("more")))
        follow = data(m.call("read_log", cursor=page["cursor"], limit=1))
        case("the returned cursor continues",
             follow["entries"][0]["seq"] == page["entries"][-1]["seq"] + 1, str(follow)[:200])
        case("no loss from a live cursor", follow.get("lost") == 0, str(follow.get("lost")))
        for bad in (-1, 1.5, "x", 2 ** 60):
            case(f"cursor {bad!r} is refused", is_error(m.call("read_log", cursor=bad)))
        for bad in (0, 257):
            case(f"limit {bad} is refused", is_error(m.call("read_log", limit=bad)))

        # Setting it without log makes it an ordinary breakpoint again.
        m.call("pause")
        m.call("set_breakpoint", address=LOOP)
        r = data(m.call("continue"))
        d = data(m.call("wait_for_stop", timeoutMs=3000))
        case("without log it stops", d.get("completed") is True, str(d)[:160])
        listed = data(m.call("list_breakpoints"))["breakpoints"]
        case("and the message is gone", listed and "logs" not in listed[0], str(listed))
        m.call("remove_breakpoint", address=LOOP)

    # Per owner: a logpoint must not hide another client's plain breakpoint.
    port = free_port()
    with Mcp(["-debugport", str(port)]) as m:
        m.init()
        time.sleep(0.3)
        plant_loop(m)
        dap = Dap(port)
        dap.session()
        d = data(m.call("set_breakpoint", address=LOOP + 2, log="m"))
        case("alone, nothing else stops it", d.get("alsoStops") is False, str(d))
        r = dap.request("setInstructionBreakpoints",
                        {"breakpoints": [{"instructionReference": hex(LOOP + 2)}]})
        case("DAP arms the same address", r["body"]["breakpoints"][0]["verified"] is True, str(r))
        d = data(m.call("set_breakpoint", address=LOOP + 2, log="m"))
        case("alsoStops sees the other client", d.get("alsoStops") is True, str(d))
        before = log_lines(m).get("cursor", 0)
        m.call("set_registers", pc=LOOP)
        r = data(m.call("run_frames", frames=2))
        case("a DAP breakpoint at the same address still stops the machine",
             r.get("reason") == "breakpoint", str(r)[:200])
        case("the logpoint still printed", len(log_lines(m, before).get("entries", [])) >= 1)
        dap.request("setInstructionBreakpoints", {"breakpoints": []})
        dap.close()

    # An HTTP client's lease ending releases its logpoints.
    with HttpMcp(env={"X16_MCP_LEASE_MS": "500"}) as m:
        m.init()
        plant_loop(m)
        m.call("set_breakpoint", address=LOOP, log="leased")
        time.sleep(1.2)
        case("a lease ending releases its logpoint",
             not data(m.call("list_breakpoints")).get("breakpoints"))

    # Read-only: reading the log is available and changes nothing.
    with Mcp(["-mcp-readonly"]) as m:
        m.init()
        time.sleep(0.2)
        d = data(m.call("read_log"))
        case("read_log works under -mcp-readonly", d.get("entries") == [], str(d)[:160])
        case("set_breakpoint does not", is_error(m.call("set_breakpoint", address=LOOP, log="x")))


def logpoint_run(with_log):
    """A deterministic machine, stepped to a fixed frame, then run under a loop."""
    with Mcp(["-deterministic", "-seed", "1", "-sound", "none"]) as m:
        m.init()
        m.call("pause")
        f0 = data(m.call("get_status")).get("frame", 0)
        m.call("run_frames", frames=600 - f0, timeoutMs=30000)
        frame = data(m.call("get_status")).get("frame")
        # A counting loop, IRQs live, so interrupts and machine state keep moving.
        m.call("write_memory", address=0x0900, hex="EE10094C0009")
        m.call("set_registers", pc=0x0900)
        if with_log:
            m.call("set_breakpoint", address=0x0900, log="n={byte[$0910]} X={X} v={vbyte[0]} "
                                                         "d={byte[$9F23]} t={byte[$9F04]} s={byte[$9F27]}")
        hashes = []
        for _ in range(10):
            hashes.append(data(m.call("run_frames", frames=10, hash=True, timeoutMs=30000)).get("hash"))
        last = data(m.call("get_status")).get("frame")
        logged = data(m.call("read_log")).get("cursor", 0)
        return frame, last, hashes, logged


def test_logpoint_determinism():
    print("logpoint determinism")
    f_a, l_a, plain, n_a = logpoint_run(False)
    f_b, l_b, logged, n_b = logpoint_run(True)
    case("both reach the same frame first", f_a == 600 and f_b == 600, f"{f_a} {f_b}")
    case("and end on the same frame", l_a == l_b and l_a == 700, f"{l_a} {l_b}")
    case("the plain run logged nothing", n_a == 0, str(n_a))
    case("the logpoint fired", n_b > 1000, str(n_b))
    case("every hash was taken", all(plain) and len(plain) == 10, str(plain[:2]))
    case("a logpoint leaves the machine state hash identical", plain == logged,
         next((f"frame {600 + 10 * (i + 1)}" for i, (x, y) in enumerate(zip(plain, logged)) if x != y), ""))


def stop_on_wait_booted(m):
    end = time.time() + 20
    while time.time() < end and data(m.call("get_status")).get("frame", 0) < 200:
        time.sleep(0.05)


def stop_on_run(m, hex_code, pc=0x0810):
    m.call("pause")
    m.call("write_memory", address=0x1000, hex="00")
    m.call("write_memory", address=pc, hex=hex_code)
    status = data(m.call("get_registers"))["cpu"]["status"]
    m.call("set_registers", pc=pc, status=status | 0x04)
    last = data(m.call("get_status")).get("stopId")
    m.call("continue")
    return data(m.call("wait_for_stop", sinceStopId=last, timeoutMs=3000))


def stop_on_runs_on(m):
    last = data(m.call("get_status")).get("stopId")
    m.call("continue")
    d = data(m.call("wait_for_stop", sinceStopId=last, timeoutMs=500))
    running = d.get("completed") is not True
    m.call("pause")
    count = data(m.call("read_memory", address=0x1000, length=1)).get("hex", "00")
    return running and count == "01"


def test_stop_on():
    print("set_stop_on")
    with Mcp() as m:
        m.init()
        stop_on_wait_booted(m)
        d = data(m.call("set_stop_on"))
        case("nothing armed by default", d.get("filters") == [] and d.get("armed") == [], str(d))
        r = m.call("set_stop_on", filters=["brk", "nope"])
        case("an unknown filter is refused", is_error(r), str(r)[:160])
        case("and changes nothing", data(m.call("set_stop_on")).get("filters") == [])
        case("filters must be a list", is_error(m.call("set_stop_on", filters="brk")))
        case("an unknown argument is refused", is_error(m.call("set_stop_on", on=["brk"])))

        d = data(m.call("set_stop_on", filters=["stp"]))
        case("arming reports it", d.get("filters") == ["stp"] and d.get("armed") == ["stp"], str(d))
        case("the result has the run state", "paused" in d and "stopReason" in d, str(d))
        d = stop_on_run(m, "DBA9018D00104C1108")
        case("stp: stops", d.get("completed") is True and d.get("stopReason") == "exception", str(d))
        s = d.get("stop", {})
        case("stp: names it", s.get("kind") == "stp" and s.get("address") == 0x0810 and s.get("programBank") == 0, str(s))
        case("stp: no vector", "vector" in s and s["vector"] is None, str(s))
        case("stp: the reason is on every result",
             data(m.call("get_status")).get("stopReason") == "exception")
        case("stp: resuming runs on, no second stop", stop_on_runs_on(m))

        m.call("set_stop_on", filters=["brk"])
        m.call("pause")
        m.call("write_memory", address=0x0316, hex="2008")
        m.call("write_memory", address=0x0820, hex="A9018D00104C2008")
        d = stop_on_run(m, "0000")
        s = d.get("stop", {})
        case("brk: stops", d.get("stopReason") == "exception" and s.get("kind") == "brk", str(d))
        case("brk: vector and address", s.get("vector") == 0xFFFE and s.get("address") == 0x0810, str(s))
        case("brk: resuming runs the handler, no second stop", stop_on_runs_on(m))

        d = data(m.call("set_stop_on", filters=[]))
        case("[] clears it", d.get("filters") == [] and d.get("armed") == [], str(d))

        m.call("set_stop_on", filters=["irq"])
        m.call("pause")
        status = data(m.call("get_registers"))["cpu"]["status"]
        m.call("set_registers", pc=0x0820, status=status & ~0x04)
        d = data(m.call("run_frames", frames=5))
        case("irq: ends a run of frames", d.get("stopReason") == "exception" and
             d.get("stop", {}).get("kind") == "irq", str(d)[:200])
        m.call("set_stop_on", filters=[])
        m.call("continue")

    port = free_port()
    with HttpMcp(["-debugport", str(port)], env={"X16_MCP_LEASE_MS": "700"}) as m:
        m.init()
        m.call("get_status")
        dap = Dap(port)
        dap.session()
        dap.request("setExceptionBreakpoints", {"filters": ["brk"]})
        d = data(m.call("set_stop_on", filters=["irq"]))
        case("armed is every client's", d.get("filters") == ["irq"] and sorted(d.get("armed", [])) == ["brk", "irq"],
             str(d))
        d = data(m.call("set_stop_on", filters=[]))
        case("clearing leaves the other client's", d.get("armed") == ["brk"], str(d))
        m.call("set_stop_on", filters=["nmi"])
        time.sleep(1.3)
        d = data(m.call("set_stop_on"))
        case("a lease expiring releases them", d.get("filters") == [] and d.get("armed") == ["brk"], str(d))
        dap.close()
        time.sleep(0.3)
        d = data(m.call("set_stop_on"))
        case("a DAP session ending releases its own", d.get("armed") == [], str(d))


GATE = 0x0920     # LDA $0940, BEQ GATE, then a NOP at GATE_END, JMP GATE_END
GATE_END = 0x0925
GATE_CODE = "AD4009F0FBEA4C2509"
GATE_FLAG = 0x0940


def plant_gate(m):
    """Pause, spinning at GATE until GATE_FLAG is set."""
    m.call("pause")
    m.call("write_memory", address=GATE, hex=GATE_CODE)
    m.call("write_memory", address=GATE_FLAG, hex="00")
    status = data(m.call("get_registers"))["cpu"]["status"]
    m.call("set_registers", pc=GATE, status=status | 0x04)


def dap_run_to_ok(m, dap):
    """The run-to slot is free: another owner can arm one and get there."""
    m.call("pause")
    m.call("write_memory", address=LOOP, hex=LOOP_CODE)
    m.call("set_registers", pc=LOOP)
    r = dap.request("x16/runTo", {"address": "$0901", "timeoutMs": 3000})
    return bool(r) and r.get("success") is True and r["body"].get("arrived") is True


def test_run_to():
    print("run_to")
    port = free_port()
    with Mcp(["-debugport", str(port)]) as m:
        m.init()
        stop_on_wait_booted(m)
        dap = Dap(port)
        dap.session()

        plant_loop(m)
        d = data(m.call("run_to", address=LOOP + 4))
        case("arrives", d.get("completed") is True and d.get("arrived") is True and
             d.get("reason") == "target", str(d))
        case("stops there", d.get("paused") is True and d.get("pc") == LOOP + 4 and
             d.get("stopReason") == "step", str(d))

        for label, args in (("nothing", {}), ("both forms", {"address": LOOP, "file": "a.s", "line": 1}),
                            ("a file alone", {"file": "a.s"}), ("a line alone", {"line": 1}),
                            ("line 0", {"file": "a.s", "line": 0}),
                            ("an unknown symbol", {"address": "no_such_label"}),
                            ("a VRAM prefix", {"address": "vram:100"}),
                            ("a bank on low RAM", {"address": LOOP, "bank": 1}),
                            ("a bank that disagrees", {"address": "04:A000", "bank": 5}),
                            ("a bank out of range", {"address": 0xA000, "bank": 256}),
                            ("a bad timeout", {"address": LOOP, "timeoutMs": 30001}),
                            ("an unknown argument", {"address": LOOP, "frames": 1})):
            case(f"run_to refuses {label}", is_error(m.call("run_to", **args)))
        case("a refusal leaves it paused", data(m.call("get_status")).get("paused") is True)

        m.call("write_memory", address=0x0910, hex="00")
        d = data(m.call("run_to", address=LOOP + 4))
        case("from the target, it goes round again", d.get("arrived") is True and
             data(m.call("read_memory", address=0x0910, length=1)).get("hex") == "05", str(d))

        m.call("continue")
        case("a running machine is refused", is_error(m.call("run_to", address=LOOP)))

        plant_loop(m)
        for bank in (4, 5):
            m.call("set_registers", ramBank=bank)
            m.call("write_memory", address="$A000", hex="EA4C00A0")
        m.call("set_registers", ramBank=5, pc=0xA001)
        d = data(m.call("run_to", address="04:A000", timeoutMs=200))
        case("another bank at the same address is not the target",
             d.get("completed") is False and d.get("reason") == "timeout" and d.get("paused") is False, str(d))
        m.call("pause")
        m.call("set_registers", ramBank=4, pc=0xA001)
        d = data(m.call("run_to", address="04:A000"))
        case("a bank in the address", d.get("arrived") is True and d.get("bank") == 4, str(d))
        m.call("set_registers", pc=0xA001)
        d = data(m.call("run_to", address=0xA000, bank=4))
        case("a bank as an argument", d.get("arrived") is True and d.get("bank") == 4, str(d))
        m.call("set_registers", ramBank=5, pc=0xA001)
        d = data(m.call("run_to", address=0xA000))
        case("no bank means whichever is mapped", d.get("arrived") is True and d.get("bank") is None, str(d))

        plant_loop(m)
        m.call("set_breakpoint", address=LOOP + 2)
        d = data(m.call("run_to", address=LOOP + 7))
        case("a breakpoint ends it", d.get("completed") is True and d.get("arrived") is False and
             d.get("reason") == "breakpoint" and d.get("pc") == LOOP + 2, str(d))
        m.call("remove_breakpoint", address=LOOP + 2)
        case("and clears its slot", dap_run_to_ok(m, dap))

        plant_loop(m)
        m.call("set_breakpoint", address=0x0910, kind="write")
        d = data(m.call("run_to", address=LOOP))
        case("a watch ends it", d.get("arrived") is False and d.get("reason") == "watchpoint" and
             d.get("stopReason") == "data breakpoint", str(d))
        m.call("remove_breakpoint", address=0x0910, kind="write")
        case("and clears its slot", dap_run_to_ok(m, dap))

        m.call("pause")
        m.call("write_memory", address=0x0A00, hex="DBEA4C010A")
        m.call("set_registers", pc=0x0A00)
        m.call("set_stop_on", filters=["stp"])
        d = data(m.call("run_to", address=0x0A03))
        case("an exception filter ends it", d.get("arrived") is False and d.get("reason") == "exception", str(d))
        m.call("set_stop_on", filters=[])
        case("and clears its slot", dap_run_to_ok(m, dap))

        plant_gate(m)
        rid = m.start("run_to", address=GATE_END, timeoutMs=20000)
        time.sleep(0.15)
        dap.request("pause", {"threadId": 1})
        d = data(m.result(rid))
        case("a pause ends it", d.get("completed") is True and d.get("arrived") is False and
             d.get("reason") == "pause", str(d))
        case("and clears its slot", dap_run_to_ok(m, dap))

        plant_gate(m)
        d = data(m.call("run_to", address=GATE_END, timeoutMs=100))
        case("a timeout answers", d.get("completed") is False and d.get("arrived") is False and
             d.get("reason") == "timeout", str(d))
        case("and leaves it going", d.get("paused") is False, str(d))
        r = dap.request("x16/runTo", {"address": "$0925"})
        case("another owner is refused while it is armed",
             bool(r) and r.get("success") is False and "another" in r.get("message", ""), str(r))
        case("as is another MCP call on a running machine", is_error(m.call("run_to", address=GATE_END)))
        last = d.get("stopId")
        m.call("write_memory", address=GATE_FLAG, hex="01")
        d = data(m.call("wait_for_stop", sinceStopId=last, timeoutMs=3000))
        case("it still arrives afterwards", d.get("completed") is True and d.get("pc") == GATE_END and
             d.get("stopReason") == "step", str(d))

        plant_gate(m)
        seq = dap.send("x16/runTo", {"address": "$0925", "timeoutMs": 20000})
        time.sleep(0.15)
        r = m.call("run_to", address=GATE_END)
        case("MCP is refused while DAP's is armed", is_error(r) and "another" in r["content"][0]["text"], str(r)[:160])
        m.call("continue")
        r = dap.answer(seq)
        case("an MCP continue ends a DAP run-to", bool(r) and r["body"].get("completed") is False and
             r["body"].get("reason") == "cancelled", str(r))

        plant_gate(m)
        r = dap.request("x16/runTo", {"address": "$0925", "timeoutMs": 100})
        case("a DAP timeout answers", bool(r) and r["body"].get("completed") is False and
             r["body"].get("reason") == "timeout", str(r))
        st = dap.request("x16/status")
        case("and leaves it going", bool(st) and st["body"].get("paused") is False, str(st))
        dap.events.clear()
        m.call("write_memory", address=GATE_FLAG, hex="01")
        ev = dap.event("stopped", 3)
        case("and a DAP target still stops it", bool(ev) and ev["body"].get("reason") == "step", str(ev))

        plant_gate(m)
        m.call("write_memory", address=GATE_FLAG, hex="01")
        r = dap.request("x16/runTo", {"address": "$0925"})
        case("DAP arrives", bool(r) and r["body"].get("arrived") is True and r["body"].get("reason") == "target" and
             r["body"].get("pc") == GATE_END, str(r))

        plant_gate(m)
        rid = m.start("run_to", address=GATE_END, timeoutMs=20000)
        time.sleep(0.15)
        m.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/cancelled",
                            "params": {"requestId": rid}}))
        time.sleep(0.15)
        m.call("pause")
        m.call("write_memory", address=GATE_FLAG, hex="01")
        d = data(m.call("run_to", address=GATE_END))
        case("a cancelled call blocks nothing", d.get("arrived") is True, str(d))

        plant_gate(m)
        rid = m.start("run_to", address=GATE_END, timeoutMs=20000)
        time.sleep(0.15)
        dap.request("restart", {})
        d = data(m.result(rid))
        case("a reset ends it", d.get("completed") is False and d.get("reason") == "cancelled", str(d))

        stop_on_wait_booted(m)
        plant_gate(m)
        dap.send("x16/runTo", {"address": "$0925", "timeoutMs": 20000})
        time.sleep(0.15)
        dap.close()
        time.sleep(0.3)
        m.call("write_memory", address=GATE_FLAG, hex="01")
        time.sleep(0.3)
        d = data(m.call("get_status"))
        case("a DAP disconnect releases it", d.get("paused") is False, str(d))
        m.call("pause")
        case("and frees the slot", is_error(m.call("run_to")) is True and
             data(m.call("run_to", address=GATE_END, timeoutMs=500)).get("completed") is not None)

    port = free_port()
    with HttpMcp(["-debugport", str(port)], env={"X16_MCP_LEASE_MS": "500"}) as m:
        m.init()
        m.call("get_status")
        dap = Dap(port)
        dap.session()
        plant_gate(m)
        d = data(m.call("run_to", address=GATE_END, timeoutMs=50))
        case("over HTTP, a timeout answers", d.get("completed") is False, str(d))
        time.sleep(1.2)
        dap.request("writeMemory", {"memoryReference": hex(GATE_FLAG), "data": "AQ=="})
        time.sleep(0.4)
        st = dap.request("x16/status")
        case("a lease expiring releases it", bool(st) and st["body"].get("paused") is False, str(st))
        dap.close()

    prg = struct.pack("<H", 0x0A00) + bytes.fromhex("EAEA4C020A") + b"\xEA" * 11
    with tempfile.TemporaryDirectory() as root:
        program = os.path.join(root, "src.prg")
        with open(program, "wb") as f:
            f.write(prg)
        with open(os.path.join(root, "src.dbg"), "wb") as f:
            f.write(source_dbg("forged.s"))
        port = free_port()
        with Mcp(["-debugport", str(port), "-fsroot", root]) as m:
            m.init()
            stop_on_wait_booted(m)
            dap = Dap(port)
            dap.session()
            m.call("pause")
            m.call("load_prg", path=program)
            status = data(m.call("get_registers"))["cpu"]["status"]
            m.call("set_registers", pc=0x0A00, status=status | 0x04)

            d = data(m.call("run_to", address="_hello"))
            case("a symbol", d.get("arrived") is True and d.get("pc") == 0x0A02, str(d))
            m.call("set_registers", pc=0x0A00)
            d = data(m.call("run_to", file="solo.s", line=3))
            case("a source line", d.get("arrived") is True and d.get("pc") == 0x0A02, str(d))
            m.call("set_registers", pc=0x0A00)
            r = dap.request("x16/runTo", {"file": "solo.s", "line": 3})
            case("a source line over DAP", bool(r) and r["body"].get("pc") == 0x0A02, str(r))
            m.call("set_registers", pc=0x0A00)
            r = dap.request("x16/runTo", {"address": "sym:_hello"})
            case("a symbol over DAP", bool(r) and r["body"].get("pc") == 0x0A02, str(r))

            res = m.call("run_to", file="solo.s", line=7)
            text = res.get("content", [{}])[0].get("text", "")
            case("a line in two places is refused, naming them",
                 is_error(res) and "BANKA" in text and "BANKB" in text, text)
            case("and it stayed paused", data(m.call("get_status")).get("paused") is True)
            for label, args in (("no code", {"file": "solo.s", "line": 4}),
                                ("an ambiguous file", {"file": "main.s", "line": 1}),
                                ("an unknown file", {"file": "nope.s", "line": 1}),
                                ("a bank on an unbanked line", {"file": "solo.s", "line": 3, "bank": 1})):
                case(f"run_to refuses {label}", is_error(m.call("run_to", **args)))

            m.call("set_registers", ramBank=4)
            m.call("write_memory", address="$A000", hex="EA4C00A0")
            m.call("set_registers", pc=0xA001)
            d = data(m.call("run_to", file="solo.s", line=7, bank=4))
            case("a bank picks one of them", d.get("arrived") is True and d.get("pc") == 0xA000 and
                 d.get("bank") == 4, str(d))
            dap.close()


def ym_poke(reg, val):
    """BIT $9F41 / BMI back, then write one YM2151 register."""
    return "2C419F30FB" + "A9%02X8D409F" % reg + "A9%02X8D419F" % val


def vera_poke(reg, val):
    return "A9%02X8D%02X9F" % (val, reg)


AUDIO_CODE = (
    "78" + vera_poke(0x25, 0x00) + vera_poke(0x20, 0xCC) + vera_poke(0x21, 0xF9) + vera_poke(0x22, 0x11) +
    vera_poke(0x23, 0x00) + vera_poke(0x23, 0x04) + vera_poke(0x23, 0xFF) + vera_poke(0x23, 0x60) +
    vera_poke(0x3C, 0x00) + vera_poke(0x3B, 0x80) + vera_poke(0x3B, 0x3F) +
    vera_poke(0x3D, 0x11) * 4 +
    ym_poke(0x22, 0xEF) + ym_poke(0x2A, 0x4A) + ym_poke(0x32, 0x80) + ym_poke(0x3A, 0x52) +
    ym_poke(0x42, 0x35) + ym_poke(0x62, 0x10) + ym_poke(0x82, 0x9F) +
    ym_poke(0x19, 0x45) + ym_poke(0x19, 0xA6) + ym_poke(0x08, 0x7A)
)


def test_audio():
    print("audio state and voice mute")
    port = free_port()
    with Mcp(["-debugport", str(port), "-deterministic", "-sound", "none"]) as m:
        m.init()
        case("booted", wait_basic(m))
        m.call("pause")
        run_planted(m, AUDIO_CODE)

        d = data(m.call("get_audio_state"))
        case("audio is running", d.get("audioRunning") is True, str(d)[:120])
        v = d.get("psg", {}).get("voices", [{}] * 16)[3]
        case("PSG raw registers", v.get("regs") == [0x00, 0x04, 0xFF, 0x60], str(v))
        case("PSG decode", v.get("wave") == "Saw" and v.get("freq") == 0x0400 and v.get("vol") == 63 and
             v.get("volLut") == 511 and v.get("left") is True and v.get("right") is True and
             v.get("pw") == 0x20 and v.get("shapeMask") == 0x1F, str(v))
        case("PSG pitch in Hz", abs(v.get("hz", 0) - 381.4697265625) < 1e-6, str(v.get("hz")))
        y = d.get("ym", {})
        c = y.get("channels", [{}] * 8)[2]
        case("YM raw registers", c.get("regs") == [0xEF, 0x4A, 0x80, 0x52], str(c)[:200])
        case("YM decode", c.get("ALG") == 7 and c.get("FB") == 5 and c.get("pan") == "L/R" and
             c.get("KC") == 0x4A and c.get("KF") == 32 and c.get("PMS") == 5 and c.get("AMS") == 2 and
             c.get("noteValid") is True, str(c)[:200])
        case("YM pitch in Hz", abs(c.get("hz", 0) - 440.0 * 2 ** (0.5 / 12)) < 1e-6, str(c.get("hz")))
        op = (c.get("operators") or [{}])[0]
        case("YM operator decode", op.get("slot") == "M1" and op.get("DT1") == 3 and op.get("MUL") == 5 and
             op.get("TL") == 0x10 and op.get("KS") == 2 and op.get("AR") == 0x1F, str(op))
        case("YM key-on is by slot", c.get("keyOnMask") == 0x0F and op.get("keyOn") is True, str(c.get("keyOnMask")))
        case("both LFO depths survive sharing $19",
             y.get("lfo", {}).get("amDepth") == 0x45 and y.get("lfo", {}).get("pmDepth") == 0x26, str(y.get("lfo")))
        p = d.get("pcm", {})
        case("PCM decode", p.get("ctrl") == 0x3F and p.get("format") == "stereo 16-bit" and p.get("volume") == 15 and
             p.get("volumeLut") == 64 and p.get("rate") == 0 and p.get("rateHz") == 0 and
             p.get("fifoQueued") == 4 and p.get("framesQueued") == 1, str(p))
        only = data(m.call("get_audio_state", chip="pcm"))
        case("'chip' limits the reply", "pcm" in only and "psg" not in only and "ym" not in only, str(only)[:160])
        case("an unknown chip is refused", is_error(m.call("get_audio_state", chip="sid")))

        dap = Dap(port)
        dap.session()
        def state_hash():
            return (dap.request("x16/status", {"hash": True}) or {}).get("body", {}).get("hash")
        before = state_hash()
        again = data(m.call("get_audio_state"))
        case("reading twice gives the same", again.get("pcm") == p and again.get("ym") == y and
             again.get("psg") == d.get("psg"))
        case("reading changes no state hash", before is not None and state_hash() == before)

        r = data(m.call("set_voice_mute", chip="psg", voice=3, muted=True))
        case("MCP mutes a PSG voice", r.get("mcp") == 8 and r.get("ui") == 0 and r.get("effective") == 8, str(r))
        r = (dap.request("x16/setVoiceMute", {"chip": "psg", "voice": 5, "muted": True}) or {}).get("body", {})
        case("DAP has its own mask", r.get("dap") == 32 and r.get("mcp") == 8 and r.get("ui") == 0 and
             r.get("effective") == 40, str(r))
        r = data(m.call("set_voice_mute", chip="ym", voice=2, muted=True))
        case("MCP mutes an FM channel", r.get("mcp") == 4 and r.get("effective") == 4, str(r))
        case("muting changes no state hash", state_hash() == before)
        s = data(m.call("get_audio_state"))
        voices = s.get("psg", {}).get("voices", [{}] * 16)
        case("each voice says who muted it", voices[3].get("mutedBy") == ["mcp"] and voices[3].get("muted") is True and
             voices[5].get("mutedBy") == ["dap"] and voices[0].get("mutedBy") == [] and voices[0].get("muted") is False,
             str(voices[3]))
        case("the FM channel does too", s.get("ym", {}).get("channels", [{}] * 8)[2].get("mutedBy") == ["mcp"])
        r = data(m.call("set_voice_mute", chip="psg", voice=5, muted=False))
        case("a client cannot lift another's mute", r.get("dap") == 32 and r.get("effective") == 40, str(r))

        for args in ({"chip": "psg", "voice": 16, "muted": True}, {"chip": "ym", "voice": 8, "muted": True},
                     {"chip": "pcm", "muted": True}, {"chip": "psg", "voice": 1}, {"voice": 1, "muted": True},
                     {"chip": "psg", "voice": 1.5, "muted": True}):
            case(f"set_voice_mute refuses {args}", is_error(m.call("set_voice_mute", **args)))

        dap.close()
        time.sleep(0.3)
        r = data(m.call("get_audio_state", chip="pcm")).get("mute", {})
        case("a DAP session's mutes end with it", r.get("psg", {}).get("dap") == 0 and r.get("psg", {}).get("mcp") == 8,
             str(r))

    with HttpMcp(["-deterministic", "-sound", "none"], env={"X16_MCP_LEASE_MS": "500"}) as m:
        m.init()
        r = data(m.call("set_voice_mute", chip="ym", muted=True))
        case("the whole chip mutes", r.get("mcp") == 0xFF, str(r))
        time.sleep(1.2)
        r = data(m.call("get_audio_state", chip="pcm")).get("mute", {})
        case("a lease ending releases its mutes", r.get("ym", {}).get("mcp") == 0 and
             r.get("ym", {}).get("effective") == 0, str(r))

    with Mcp(["-sound", "none"]) as m:
        m.init()
        d = data(m.call("get_audio_state"))
        op = d.get("ym", {}).get("channels", [{"operators": [{}]}])[0]["operators"][0]
        case("with no sound the chips are reported as not running",
             d.get("audioRunning") is False and "eg" in op and op["eg"] is None, str(op))

    with Mcp(["-mcp-readonly"]) as m:
        m.init()
        time.sleep(0.2)
        case("get_audio_state works under -mcp-readonly", "psg" in data(m.call("get_audio_state")))
        case("set_voice_mute does not", is_error(m.call("set_voice_mute", chip="psg", muted=True)))


T2_SET = "78A9108D089FA9008D099FA240CAD0FD"   # SEI, T2 = $0010, delay past underflow
T2_READ = "AD089F"
VIA_BURST = "78A200A020AD089FCAD0FA88D0F7"    # 8192 reads of $9F08
ALL_IO = ["via1", "via2", "vera", "spi", "ym", "emu", "midi", "openbus", "sdcard", "files", "i2c",
          "joystick", "serial"]


def cpu_regs(m):
    return data(m.call("get_registers"))["cpu"]


def word_at(m, address):
    return int.from_bytes(bytes.fromhex(data(m.call("read_memory", address=address, length=2))["hex"]), "little")


def park(m, status_or=0, status_and=0xFF, code="80FE"):
    m.call("write_memory", address=0x0810, hex=code)
    status = cpu_regs(m)["status"]
    m.call("set_registers", pc=0x0810, status=(status & status_and) | status_or)
    return cpu_regs(m)


def test_io_state(m, dap):
    d = data(m.call("get_io_state"))
    for key in ("irq", "via1", "via2", "i2c", "smc", "rtc", "joystick"):
        case(f"get_io_state has {key}", key in d, str(d)[:160])
    case("VIA1 is fitted", d["via1"].get("fitted") is True and len(d["via1"].get("regs", [])) == 16)
    case("an unfitted VIA2 says so", d["via2"] == {"fitted": False}, str(d["via2"]))
    case("the RTC holds its NVRAM", len(d["rtc"].get("nvram", "")) == 128)
    only = data(m.call("get_io_state", device="rtc"))
    case("'device' limits it", "rtc" in only and "via1" not in only and "irq" in only, str(only)[:120])
    for bad in ("sid", 3):
        case(f"device {bad!r} is refused", is_error(m.call("get_io_state", device=bad)))

    port = d["joystick"]["ports"][0]
    case("an undriven port reads high", port.get("port") == 1 and port.get("driven") is False and
         port.get("source") == "none" and port.get("mask") == 0xFFFF and port.get("shift") == 0xFFFF, str(port))
    m.call("joystick", port=1, buttons=[])
    port = data(m.call("get_io_state", device="joystick"))["joystick"]["ports"][0]
    case("a driven port is reported", port.get("driven") is True and port.get("source") == "virtual" and
         port.get("buttons") == [], str(port))
    m.call("joystick", port=1, release=True)
    port = data(m.call("get_io_state", device="joystick"))["joystick"]["ports"][0]
    case("a released port is empty again", port.get("driven") is False and port.get("shift") == 0xFFFF, str(port))

    run_planted(m, T2_SET)
    via = data(m.call("get_io_state", device="via1"))["via1"]
    case("a VIA flag is pending", "t2" in via.get("flags", []), str(via)[:200])

    def state_hash():
        return (dap.request("x16/status", {"hash": True}) or {}).get("body", {}).get("hash")
    before = state_hash()
    for _ in range(3):
        m.call("get_io_state")
        dap.request("x16/io")
    case("reading I/O state changes no state hash", before is not None and state_hash() == before)
    via = data(m.call("get_io_state", device="via1"))["via1"]
    case("and clears no VIA flag", "t2" in via.get("flags", []), str(via)[:200])
    run_planted(m, T2_READ)
    via = data(m.call("get_io_state", device="via1"))["via1"]
    case("the CPU reading T2 does clear it", "t2" not in via.get("flags", []), str(via)[:200])


def test_io_capture(m, dap):
    r = data(m.call("read_io_trace"))
    c = r.get("capture", {})
    case("nothing records by default", r.get("entries") == [] and c.get("enabled") is False and
         c.get("effective") == [], str(r)[:200])

    r = data(m.call("set_io_capture"))
    c = r.get("capture", {})
    case("the default leaves out VERA and open bus", "via1" in c.get("mcp", []) and
         "vera" not in c.get("mcp", []) and "openbus" not in c.get("mcp", []), str(c))
    case("the UI's selection is untouched", c.get("ui") == [], str(c))
    start = r.get("cursor")
    run_planted(m, "AD209F" + T2_READ)
    page = data(m.call("read_io_trace", cursor=start, limit=1024))
    entries = page.get("entries", [])
    case("a register read is recorded", any(e.get("address") == 0x9F08 and e.get("op") == "read" and
         e.get("device") == "via1" and e.get("kind") == "access" for e in entries), str(entries[:2]))
    case("VERA stays out of it", not any(e.get("device") == "vera" for e in entries))

    r = (dap.request("x16/ioCapture", {"devices": ["vera"]}) or {}).get("body", {})
    c = r.get("capture", {})
    case("DAP has its own set", c.get("dap") == ["vera"] and "via1" in c.get("mcp", []) and
         "vera" in c.get("effective", []), str(c))
    start = r.get("cursor")
    run_planted(m, "AD209F")
    entries = data(m.call("read_io_trace", cursor=start, limit=1024)).get("entries", [])
    case("its device is recorded", any(e.get("device") == "vera" for e in entries), str(entries[:2]))

    c = data(m.call("set_io_capture", devices=[])).get("capture", {})
    case("[] releases only the caller's", c.get("mcp") == [] and c.get("dap") == ["vera"] and
         c.get("effective") == ["vera"] and c.get("enabled") is True, str(c))
    for args in ({"devices": ["nope"]}, {"devices": "via1"}, {"devices": [1]}, {"banana": 1}):
        case(f"set_io_capture refuses {args}", is_error(m.call("set_io_capture", **args)))
    case("and changes nothing", data(m.call("read_io_trace", limit=1))["capture"]["mcp"] == [])
    case("read_io_trace takes no capture argument", is_error(m.call("read_io_trace", devices=["via1"])))
    case("so nothing was armed", data(m.call("read_io_trace", limit=1))["capture"]["mcp"] == [])

    start = data(m.call("set_io_capture", devices=["via1"]))["cursor"]
    run_planted(m, VIA_BURST)
    page = data(m.call("read_io_trace", cursor=start, limit=5))
    e = page.get("entries", [])
    case("a wrapped ring reports what was lost", page.get("lost", 0) > 0 and len(e) == 5 and
         e[0].get("seq") == start + page["lost"], str(page)[:200])
    case("a page is contiguous", [x.get("seq") for x in e] == list(range(e[0]["seq"], e[0]["seq"] + 5)) if e else False)
    case("and says there is more", page.get("more") is True and page.get("cursor") == e[-1]["seq"] + 1 if e else False)
    follow = data(m.call("read_io_trace", cursor=page["cursor"], limit=1))
    case("the cursor carries on", follow.get("lost") == 0 and
         follow.get("entries", [{}])[0].get("seq") == page["cursor"], str(follow)[:160])
    newest = data(m.call("set_io_capture", devices=[]))["cursor"]
    tail = data(m.call("read_io_trace", cursor=newest))
    case("a cursor at the end reads nothing", tail.get("entries") == [] and tail.get("more") is False, str(tail)[:160])
    for bad in (newest + 1, -1, 1.5, "x"):
        case(f"cursor {bad!r} is refused", is_error(m.call("read_io_trace", cursor=bad)))
    for bad in (0, 1025, 1.5):
        case(f"limit {bad} is refused", is_error(m.call("read_io_trace", limit=bad)))


def test_io_triggers(m, dap):
    m.call("write_memory", address=0x9F26, hex="00")   # IEN: no VERA source

    b = park(m, status_and=0xFF & ~0x04)
    case("no device is asserting", data(m.call("get_io_state", device="rtc"))["irq"]["line"] is False)
    dap.request("x16/status")   # drains events already sent
    dap.events.clear()
    r = data(m.call("trigger_interrupt", kind="irq"))
    a = cpu_regs(m)
    case("paused, I clear: taken", r.get("taken") is True and r.get("held") is False, str(r))
    case("it vectors", a["pc"] == word_at(m, 0xFFFE) and r.get("pc") == a["pc"] and
         r.get("vector") == 0xFFFE, f"{a['pc']:04X} {r}")
    case("it pushes three bytes", a["sp"] == b["sp"] - 3, f"{a['sp']:X} {b['sp']:X}")
    case("the return address is the parked PC", word_at(m, a["sp"] + 2) == 0x0810)
    case("it sets I", a["status"] & 0x04 != 0)
    case("the machine stays paused", r.get("paused") is True and data(m.call("get_status")).get("paused") is True,
         str(r))
    stopped = dap.event("stopped", timeout=3)
    case("DAP is told the PC moved", (stopped or {}).get("body", {}).get("reason") == "interrupt", str(stopped)[:200])

    b = park(m, status_or=0x04, code="EA80FD")   # NOP, BRA back: each step moves the PC
    r = data(m.call("trigger_interrupt", kind="irq"))
    a = cpu_regs(m)
    case("I set: held and the reply says so", r.get("taken") is False and r.get("held") is True and
         "note" in r, str(r))
    case("held: PC and SP do not move", a["pc"] == b["pc"] and a["sp"] == b["sp"], f"{a} {b}")

    m.call("set_registers", status=a["status"] & ~0x04)
    steps = [data(m.call("step")).get("completed") for _ in range(4)]
    a = cpu_regs(m)
    case("a held pulse is not remembered", steps == [True] * 4 and a["pc"] == 0x0810 and a["sp"] == b["sp"],
         f"{steps} {a['pc']:04X}")

    b = park(m, status_or=0x04, code="CBEA80FE")
    r = data(m.call("step"))
    case("parked in WAI", r.get("completed") is True and cpu_regs(m)["pc"] == 0x0811 and
         data(m.call("get_io_state", device="rtc"))["irq"]["inWai"] is True, str(r)[:160])
    r = data(m.call("trigger_interrupt", kind="irq"))
    a = cpu_regs(m)
    case("a held IRQ wakes WAI", r.get("held") is True and r.get("wokeFromWai") is True and
         data(m.call("get_io_state", device="rtc"))["irq"]["inWai"] is False, str(r))
    case("without vectoring, and still paused", a["pc"] == 0x0811 and a["sp"] == b["sp"] and
         r.get("paused") is True, f"{a['pc']:04X} {r.get('paused')}")

    b = park(m, status_or=0x04)
    r = data(m.call("trigger_interrupt", kind="nmi"))
    a = cpu_regs(m)
    case("an NMI ignores I", r.get("taken") is True and r.get("held") is False and
         a["pc"] == word_at(m, 0xFFFA) and r.get("vector") == 0xFFFA and a["sp"] == b["sp"] - 3, str(r))
    r = data(m.call("trigger_interrupt", kind="irq"))
    case("an IRQ inside the NMI handler is held", r.get("held") is True, str(r))

    # A device's flag survives a trigger; the pulse neither raises nor clears it.
    run_planted(m, T2_SET)
    m.call("write_memory", address=0x9F0E, hex="A0")   # IER: T2
    d = data(m.call("get_io_state", device="via1"))
    case("a device is asserting the line", d["irq"]["sources"] == ["via1"], str(d["irq"]))
    park(m, status_or=0x04)
    r = data(m.call("trigger_interrupt", kind="irq"))
    d = data(m.call("get_io_state", device="via1"))
    case("the reply names it", r.get("deviceLines") == ["via1"], str(r))
    case("a trigger leaves another device's assertion up", "t2" in d["via1"]["flags"] and d["irq"]["line"] is True,
         str(d["irq"]))
    m.call("write_memory", address=0x9F0E, hex="20")   # IER off
    d = data(m.call("get_io_state", device="via1"))
    case("and the line falls with its source", d["irq"]["line"] is False, str(d["irq"]))

    m.call("set_stop_on", filters=["irq"])
    park(m, status_and=0xFF & ~0x04)
    r = data(m.call("trigger_interrupt", kind="irq"))
    case("taken with a stop filter armed", r.get("taken") is True, str(r))
    last = data(m.call("get_status")).get("stopId")
    m.call("continue")
    d = data(m.call("wait_for_stop", sinceStopId=last, timeoutMs=500))
    case("an entry taken while paused is not a new stop", d.get("completed") is not True, str(d)[:200])
    m.call("set_stop_on", filters=[])
    m.call("pause")

    for args in ({}, {"kind": "brk"}, {"kind": 1}, {"kind": "irq", "banana": 1}):
        case(f"trigger_interrupt refuses {args}", is_error(m.call("trigger_interrupt", **args)))


def test_io():
    print("I/O state, trace and triggers")
    port = free_port()
    with Mcp(["-debugport", str(port), "-deterministic", "-sound", "none"]) as m:
        m.init()
        case("booted", wait_basic(m))
        m.call("pause")
        dap = Dap(port)
        dap.session()
        test_io_state(m, dap)
        test_io_capture(m, dap)
        test_io_triggers(m, dap)
        dap.close()
        time.sleep(0.3)
        c = data(m.call("read_io_trace", limit=1)).get("capture", {})
        case("a DAP session's capture ends with it", c.get("dap") == [] and c.get("effective") == [] and
             c.get("enabled") is False, str(c))

    with HttpMcp(["-deterministic", "-sound", "none"], env={"X16_MCP_LEASE_MS": "500"}) as m:
        m.init()
        c = data(m.call("set_io_capture", devices=["vera"])).get("capture", {})
        case("the lease's client captures", c.get("mcp") == ["vera"], str(c))
        time.sleep(1.2)
        c = data(m.call("read_io_trace", limit=1)).get("capture", {})
        case("a lease ending releases it", c.get("mcp") == [] and c.get("enabled") is False, str(c))

    with Mcp(["-mcp-readonly"]) as m:
        m.init()
        time.sleep(0.2)
        case("get_io_state works under -mcp-readonly", "via1" in data(m.call("get_io_state")))
        case("read_io_trace too", data(m.call("read_io_trace")).get("entries") == [])
        for name, args in (("set_io_capture", {"devices": ["via1"]}), ("trigger_interrupt", {"kind": "nmi"})):
            r = m.call(name, **args)
            case(f"{name} does not", is_error(r) and "read-only" in r["content"][0]["text"], str(r)[:160])
        case("nothing was armed", data(m.call("read_io_trace")).get("capture", {}).get("enabled") is False)


def set_layer(m, layer, *regs):
    set_vera(m, 0x0D if layer == 0 else 0x14, *regs)


def vram(m, address, *values):
    m.call("write_memory", address=address, hex="".join(f"{v:02X}" for v in values), space="vram")


def test_graphics_sprites(m):
    vram(m, 0x1FC00 + 5 * 8, 0x34, 0x8A, 0xF8, 0x03, 0x43, 0x02, 0x5B, 0x97)
    d = data(m.call("get_sprites", start=5, count=1))
    case("one sprite", d.get("count") == 1 and len(d.get("sprites", [])) == 1, str(d)[:200])
    s = (d.get("sprites") or [{}])[0]
    case("sprite index", s.get("index") == 5, str(s))
    case("sprite address", s.get("address") == (0x34 << 5 | 0xA << 13), str(s))
    case("sprite 8bpp", s.get("bpp") == 8, str(s))
    case("sprite position, signed", s.get("x") == -8 and s.get("y") == 0x243, str(s))
    case("sprite size", s.get("width") == 16 and s.get("height") == 32, str(s))
    case("sprite z", s.get("zDepth") == 2 and s.get("enabled") is True, str(s))
    case("sprite flips", s.get("hFlip") is True and s.get("vFlip") is True, str(s))
    case("sprite collision", s.get("collisionMask") == 5, str(s))
    case("sprite palette offset", s.get("paletteOffset") == 0x70, str(s))
    case("sprite raw", s.get("attr") == [0x34, 0x8A, 0xF8, 0x03, 0x43, 0x02, 0x5B, 0x97], str(s))
    d = data(m.call("get_sprites", start=120))
    case("sprites stop at 127", d.get("count") == 8 and d["sprites"][-1]["index"] == 127, str(d)[:120])
    case("all 128 by default", len(data(m.call("get_sprites")).get("sprites", [])) == 128)
    for args in ({"start": 128}, {"count": 0}, {"count": 129}, {"start": -1}, {"start": "a"}, {"banana": 1}):
        case(f"get_sprites refuses {args}", is_error(m.call("get_sprites", **args)))


def test_graphics_palette(m):
    vram(m, 0x1FA00 + 3 * 2, 0xA5, 0xF3)
    d = data(m.call("get_palette", start=3, count=2))
    e = (d.get("entries") or [{}])[0]
    case("palette range", d.get("count") == 2 and len(d.get("entries", [])) == 2, str(d)[:160])
    case("palette 12-bit", e.get("index") == 3 and e.get("rgb") == 0x3A5 and e.get("hex") == "3A5", str(e))
    case("palette channels", e.get("r") == 3 and e.get("g") == 10 and e.get("b") == 5, str(e))
    d = data(m.call("get_palette", start=250))
    case("palette stops at 255", d.get("count") == 6 and d["entries"][-1]["index"] == 255, str(d)[:120])
    case("all 256 by default", len(data(m.call("get_palette")).get("entries", [])) == 256)
    for args in ({"start": 256}, {"count": 0}, {"count": 257}, {"banana": 1}):
        case(f"get_palette refuses {args}", is_error(m.call("get_palette", **args)))


def test_graphics_renderer_copy(m):
    # src/video.c: fx_vram_cache_write stores to VRAM only.
    before = data(m.call("get_palette", start=9, count=1))["entries"][0]["rgb"]
    set_vera(m, 5, 2 << 1)
    set_vera(m, 9, 0x40)
    set_vera(m, 5, 6 << 1)
    set_vera(m, 9, 0x5A, 0x0C, 0xA5, 0x03)
    set_vera(m, 5, 0)
    set_vera(m, 0, 0x10, 0xFA, 0x01)
    set_vera(m, 3, 0x00)
    set_vera(m, 5, 2 << 1)
    set_vera(m, 9, 0x00)
    set_vera(m, 5, 0)
    raw = data(m.call("read_memory", address=0x1FA10, length=2, space="vram")).get("hex")
    now = data(m.call("get_palette", start=8, count=1))["entries"][0]
    case("an FX cache write moved VRAM only", raw == "5A0C", str(raw))
    case("the palette reports the renderer's copy", now.get("rgb") != 0xC5A, str(now))
    case("and other entries did not move",
         data(m.call("get_palette", start=9, count=1))["entries"][0]["rgb"] == before)


def test_graphics_tilemap(m):
    set_vera(m, 5, 0)
    set_vera(m, 9, 0x31)                       # VGA, both layers, no sprites
    set_layer(m, 0, 0x02, 0x10, 0x20)          # 4bpp, 32x32, map $2000, tiles $4000
    vram(m, 0x2000 + (2 * 32 + 3) * 2, 0x34, 0xBE)
    d = data(m.call("get_tilemap", layer=0, x=3, y=2, width=1, height=1))
    case("tile status", d.get("status") == "ok" and d.get("mode") == "tile" and d.get("bpp") == 4, str(d)[:200])
    case("tile geometry", d.get("mapBase") == 0x2000 and d.get("tileBase") == 0x4000 and
         d.get("mapWidth") == 32 and d.get("tileWidth") == 8, str(d)[:200])
    e = (d.get("entries") or [{}])[0]
    case("tile position", e.get("x") == 3 and e.get("y") == 2 and e.get("address") == 0x2086, str(e))
    case("tile index", e.get("tile") == 0x234, str(e))
    case("tile address", e.get("tileAddress") == 0x4000 + 0x234 * 32, str(e))
    case("tile palette offset", e.get("paletteOffset") == 0xB0, str(e))
    case("tile flips", e.get("hFlip") is True and e.get("vFlip") is True, str(e))

    for depth, bpp, size in ((1, 2, 16), (3, 8, 64)):
        set_layer(m, 0, depth, 0x10, 0x20)
        d = data(m.call("get_tilemap", layer=0, x=3, y=2, width=1, height=1))
        e = (d.get("entries") or [{}])[0]
        case(f"{bpp}bpp tile", d.get("bpp") == bpp and e.get("tileAddress") == (0x4000 + 0x234 * size) & 0x1FFFF, str(e))

    set_layer(m, 0, 0x00, 0x10, 0x20)          # 1bpp text
    vram(m, 0x2000, 0x41, 0x6E)
    d = data(m.call("get_tilemap", layer=0, width=1, height=1))
    e = (d.get("entries") or [{}])[0]
    case("text mode", d.get("mode") == "text" and d.get("bpp") == 1, str(d)[:160])
    case("text entry", e.get("tile") == 0x41 and e.get("fg") == 0xE and e.get("bg") == 6, str(e))
    case("a text entry has no flips", "hFlip" not in e, str(e))
    set_layer(m, 0, 0x08, 0x10, 0x20)          # T256C
    e = data(m.call("get_tilemap", layer=0, width=1, height=1))["entries"][0]
    case("T256C text", e.get("fg") == 0x6E and e.get("bg") == 0, str(e))

    set_layer(m, 0, 0xF2, 0xFF, 0x00)          # 256x256 at $1FE00
    vram(m, 0x00000, 0x77, 0x03)
    d = data(m.call("get_tilemap", layer=0, y=1, width=1, height=1))
    e = (d.get("entries") or [{}])[0]
    case("the map says it wraps", d.get("mapWraps") is True, str(d)[:200])
    case("a wrapped entry", e.get("address") == 0 and e.get("tile") == 0x377, str(e))
    d = data(m.call("get_tilemap", layer=0, x=255, y=255, width=1, height=1))
    case("the last entry of a wrapped map", (d.get("entries") or [{}])[0].get("address") ==
         (0x1FE00 + (255 * 256 + 255) * 2) & 0x1FFFF, str(d)[:200])

    set_layer(m, 0, 0x02, 0x10, 0x20)
    d = data(m.call("get_tilemap", layer=0, x=30, y=30, width=8, height=8))
    case("clipped to the map", d.get("clipped") is True and d.get("width") == 2 and d.get("height") == 2 and
         len(d.get("entries", [])) == 4, str(d)[:200])
    d = data(m.call("get_tilemap", layer=0, width=10, height=10, limit=25))
    case("stops at a whole row", d.get("truncated") is True and d.get("height") == 2 and d.get("nextY") == 2 and
         len(d.get("entries", [])) == 20, str(d)[:200])
    d = data(m.call("get_tilemap", layer=0))
    case("the default is the map, to the limit", d.get("width") == 32 and len(d.get("entries", [])) == 1024, str(d)[:120])
    for args in ({"layer": 2}, {"layer": -1}, {"x": 256}, {"width": 0}, {"height": 257}, {"limit": 0},
                 {"limit": 4097}, {"banana": 1}, {"layer": 0, "x": 32}, {"layer": 0, "y": 32},
                 {"layer": 0, "width": 20, "limit": 10}):
        case(f"get_tilemap refuses {args}", is_error(m.call("get_tilemap", **args)))

    set_layer(m, 0, 0x06, 0x10, 0x20)          # 4bpp bitmap
    d = data(m.call("get_tilemap", layer=0))
    case("a bitmap layer says so", d.get("status") == "bitmap" and "bitmap" in d.get("message", "") and
         d.get("entries") == [] and "mapWidth" not in d, str(d)[:200])
    set_layer(m, 0, 0x02, 0x10, 0x20)
    set_vera(m, 9, 0x21)
    d = data(m.call("get_tilemap", layer=0))
    case("a disabled layer says so", d.get("status") == "disabled" and "disabled" in d.get("message", "") and
         d.get("entries") == [], str(d)[:200])
    set_vera(m, 9, 0x30)
    d = data(m.call("get_tilemap", layer=0))
    case("output off says so", d.get("status") == "disabled" and "off" in d.get("message", "") and
         d.get("entries") == [], str(d)[:200])
    set_vera(m, 9, 0x31)


def test_graphics_purity(m, dap):
    vram(m, 0x12340, 0xAB)
    vram(m, 0x00080, 0x5A)
    set_vera(m, 5, 0x00)
    set_vera(m, 0, 0x40, 0x23, 0x11)           # $12340, step 1
    set_vera(m, 5, 0x01)
    set_vera(m, 0, 0x80, 0x00, 0x18)           # $00080, step -1
    set_vera(m, 5, 0x00)

    def snapshot():
        v = dap.request("x16/vera")["body"]
        return v.get("ports"), v.get("addrsel"), v.get("dcsel")

    def state_hash():
        return (dap.request("x16/status", {"hash": True}) or {}).get("body", {}).get("hash")

    before, h = snapshot(), state_hash()
    case("port 0 steps", before[0][0].get("increment") == 1 and before[0][0].get("address") == 0x12340, str(before[0][0]))
    case("port 1 steps", before[0][1].get("increment") == -1 and before[0][1].get("address") == 0x80, str(before[0][1]))
    for _ in range(3):
        m.call("get_sprites")
        m.call("get_palette")
        m.call("get_tilemap", layer=0)
        m.call("get_tilemap", layer=1)
        dap.request("x16/sprites")
        dap.request("x16/palette")
        dap.request("x16/tilemap", {"layer": 0})
    case("reading leaves VERA's ports where they were", snapshot() == before, str(snapshot()[0]))
    case("reading changes no state hash", h is not None and state_hash() == h)


def test_graphics():
    print("decoded graphics state")
    port = free_port()
    with Mcp(["-debugport", str(port), "-deterministic", "-sound", "none"]) as m:
        m.init()
        case("booted", wait_basic(m))
        m.call("pause")
        dap = Dap(port)
        dap.session()
        over_dap = dap.request("x16/palette", {"start": 1, "count": 1})["body"]
        case("the same palette over DAP",
             over_dap.get("entries") == data(m.call("get_palette", start=1, count=1)).get("entries"), str(over_dap)[:160])
        case("the boot palette", over_dap.get("entries", [{}])[0].get("rgb") == 0xFFF, str(over_dap)[:160])
        test_graphics_sprites(m)
        test_graphics_palette(m)
        test_graphics_renderer_copy(m)
        test_graphics_tilemap(m)
        test_graphics_purity(m, dap)
        dap.close()

    with Mcp(["-mcp-readonly"]) as m:
        m.init()
        time.sleep(0.2)
        case("get_sprites works under -mcp-readonly", len(data(m.call("get_sprites")).get("sprites", [])) == 128)
        case("get_palette too", len(data(m.call("get_palette")).get("entries", [])) == 256)
        case("get_tilemap too", data(m.call("get_tilemap")).get("status") in ("ok", "disabled", "bitmap"))


def io_capture_run(with_capture):
    with Mcp(["-deterministic", "-seed", "1", "-sound", "none"]) as m:
        m.init()
        m.call("pause")
        f0 = data(m.call("get_status")).get("frame", 0)
        m.call("run_frames", frames=600 - f0, timeoutMs=30000)
        if with_capture:
            m.call("set_io_capture", devices=ALL_IO)
        hashes = [data(m.call("run_frames", frames=10, hash=True, timeoutMs=30000)).get("hash") for _ in range(10)]
        return hashes, data(m.call("set_io_capture", devices=[])).get("cursor", 0)


def test_io_determinism():
    print("I/O trace determinism")
    plain, n_a = io_capture_run(False)
    traced, n_b = io_capture_run(True)
    case("the plain run recorded nothing", n_a == 1, str(n_a))
    case("the traced run did", n_b > 100, str(n_b))
    case("every hash was taken", all(plain) and len(plain) == 10, str(plain[:2]))
    case("capturing every device leaves the machine state hash identical", plain == traced,
         next((f"frame {600 + 10 * (i + 1)}" for i, (x, y) in enumerate(zip(plain, traced)) if x != y), ""))


def check_readonly_surface(m):
    names = {t["name"] for t in (m.rpc("tools/list") or {}).get("result", {}).get("tools", [])}
    case("only reading tools are listed", names == READING_TOOLS, str(names ^ READING_TOOLS))
    for name in sorted(LOSING_TOOLS | MOVING_TOOLS):
        r = m.call(name)
        text = (r.get("content") or [{}])[0].get("text", "")
        case(f"{name} is refused", is_error(r) and "read-only" in text, str(r)[:160])
    case("the machine kept running", data(m.call("get_status")).get("paused") is False)
    case("reads still work", data(m.call("read_memory", address=0, length=2)).get("length") == 2)
    case("nothing was armed", not data(m.call("list_breakpoints")).get("breakpoints"))


def test_refresh_events():
    print("refresh events reach the editor")
    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "prog.s").replace("\\", "/")
        with open(source, "w") as f:
            f.write("  nop\n")
        body = bytes(0x40 + i for i in range(16))
        path = os.path.join(tmp, "prog.prg")
        with open(path, "wb") as f:
            f.write(struct.pack("<H", 0x0A00) + body)
        with open(os.path.join(tmp, "prog.dbg"), "w") as f:
            f.write(K_PRG_DBG.replace('name="prog.s"', f'name="{source}"'))
        labels = os.path.join(tmp, "extra.lbl").replace("\\", "/")
        with open(labels, "w") as f:
            f.write("al 000A00 .thing\n")

        def uri(p):
            return "file:///" + p.lstrip("/")

        def settle(dap):
            time.sleep(0.15)
            dap.request("x16/status")
            dap.events.clear()

        def read_back(dap, ev):
            b = ev["body"]
            r = dap.request("readMemory", {"memoryReference": b["memoryReference"], "offset": b["offset"],
                                           "count": b["count"]})
            return base64.b64decode(r["body"]["data"])

        with Mcp(["-debugport", str(free_port()), "-fsroot", tmp]) as m:
            port = int(m.p.args[m.p.args.index("-debugport") + 1])
            m.init()
            time.sleep(0.5)

            dap = Dap(port)
            dap.request("initialize", {"adapterID": "test", "pathFormat": "uri",
                                       "supportsInvalidatedEvent": True, "supportsMemoryEvent": True})
            dap.request("attach", {})
            dap.request("configurationDone")
            plant_loop(m)
            settle(dap)

            m.call("write_memory", address=0x0900, hex="AABBCC")
            e = dap.event("memory", timeout=3)
            b = (e or {}).get("body", {})
            case("an MCP write reaches the editor",
                 b.get("memoryReference") == "0x0900" and b.get("offset") == 0 and b.get("count") == 3, str(e))
            case("and its range reads back", bool(e) and read_back(dap, e) == b"\xAA\xBB\xCC")
            e = dap.event("invalidated", timeout=3)
            case("its scopes are refreshed", "variables" in (e or {}).get("body", {}).get("areas", []), str(e))
            settle(dap)

            m.call("write_memory", address="1B000", hex="1234", space="vram")
            e = dap.event("memory", timeout=3)
            case("VRAM carries the vram: reference",
                 (e or {}).get("body", {}).get("memoryReference") == "vram:1B000" and read_back(dap, e) == b"\x12\x34",
                 str(e))
            settle(dap)

            m.call("set_registers", a=0x12, x=0x34)
            e = dap.event("invalidated", timeout=3)
            case("an MCP register write invalidates variables",
                 "variables" in (e or {}).get("body", {}).get("areas", []), str(e))
            settle(dap)
            m.call("set_registers", pc=LOOP)
            e = dap.event("invalidated", timeout=3)
            case("moving the PC invalidates the stack too",
                 sorted((e or {}).get("body", {}).get("areas", [])) == ["stacks", "variables"], str(e))
            settle(dap)

            m.call("load_symbols", path=labels)
            e = dap.event("loadedSource", timeout=3)
            b = (e or {}).get("body", {})
            case("load_symbols announces a new source in the editor's format",
                 b.get("reason") == "new" and
                 b.get("source", {}).get("path") == uri(os.path.realpath(labels).replace("\\", "/")), str(e))
            settle(dap)
            m.call("load_symbols", path=labels)
            e = dap.event("loadedSource", timeout=3)
            case("reloading it is changed", (e or {}).get("body", {}).get("reason") == "changed", str(e))
            settle(dap)

            d = data(m.call("load_prg", path=path))
            e = dap.event("memory", timeout=3)
            b = (e or {}).get("body", {})
            case("load_prg reports the range",
                 d.get("debugInfo") is True and b.get("memoryReference") == "0x0A00" and b.get("count") == 16, str(e))
            case("which holds the program", bool(e) and read_back(dap, e) == body)
            e = dap.event("loadedSource", timeout=3)
            b = (e or {}).get("body", {})
            case("and announces its source",
                 b.get("reason") == "new" and b.get("source", {}).get("path") == uri(source), str(e))
            sources = data(m.call("list_sources"))
            names = json.dumps(sources)
            case("MCP's source list is not given label files", "extra.lbl" not in names, names[:200])
            settle(dap)

            m.call("run_frames", frames=1)
            m.call("save_state", slot="r")
            settle(dap)
            m.call("load_state", slot="r")
            e = dap.event("invalidated", timeout=3)
            case("an MCP state load invalidates everything", (e or {}).get("body", {}).get("areas") == ["all"], str(e))
            dap.close()
            time.sleep(0.3)

            dap = Dap(port)
            dap.request("initialize", {"adapterID": "test"})
            dap.request("attach", {})
            dap.request("configurationDone")
            settle(dap)
            m.call("write_memory", address=0x0900, hex="01")
            m.call("set_registers", a=1)
            m.call("load_symbols", path=labels)
            e = dap.event("loadedSource", timeout=3)
            case("loadedSource needs no capability", (e or {}).get("body", {}).get("reason") == "changed", str(e))
            dap.request("x16/status")
            got = [x.get("event") for x in dap.events if x.get("event") in ("memory", "invalidated")]
            case("an editor that did not opt in gets neither of the others", not got, str(got))
            dap.close()


def test_readonly():
    print("-mcp-readonly")
    port = free_port()
    with Mcp(["-mcp-readonly", "-debugport", str(port)]) as m:
        r = m.init()
        case("initialize says so", "read-only" in (r or {}).get("result", {}).get("instructions", ""), str(r)[:200])
        time.sleep(0.4)
        check_readonly_surface(m)
        dap = Dap(port)
        dap.session()
        dap.request("pause", {"threadId": 1})
        dap.event("stopped")
        case("DAP stopped it", data(m.call("get_status")).get("paused") is True)
        d = data(m.call("wait_for_stop", timeoutMs=300))
        case("wait_for_stop reports the stop", d.get("completed") is True, str(d)[:160])
        case("and does not resume it", data(m.call("get_status")).get("paused") is True)
        dap.close()
        time.sleep(0.6)
        case("a DAP session ending is not left stopped for it",
             data(m.call("get_status")).get("paused") is False)

    port = free_port()
    with HttpMcp(["-mcp-readonly", "-debugport", str(port)], env={"X16_MCP_LEASE_MS": "700"}) as m:
        m.init()
        time.sleep(0.4)
        check_readonly_surface(m)
        dap = Dap(port)
        dap.session()
        dap.request("pause", {"threadId": 1})
        dap.event("stopped")
        m.call("get_status")
        time.sleep(1.5)
        r = dap.request("x16/status")
        case("its lease ending does not resume DAP's stop", (r or {}).get("body", {}).get("paused") is True, str(r)[:160])
        dap.request("continue", {"threadId": 1})
        dap.close()

    p = subprocess.run([EXE, "-rom", ROM, "-hidden", "-mcp-readonly"], stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
    case("-mcp-readonly without -mcp is refused", p.returncode != 0, str(p.returncode))


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
                 test_hash_memory, test_search_memory, test_dump_memory, test_warp, test_mouse, test_load_prg,
                 test_client_format, test_call_stack, test_sources, test_source_fifo, test_dbg_fifo,
                 test_read_paths,
                 test_perf_tools, test_device_stats, test_enable_breakpoint, test_enable_per_owner,
                 test_dump_breakpoint, test_evaluate, test_logpoints, test_logpoint_determinism,
                 test_stop_on, test_run_to, test_audio, test_io, test_io_determinism, test_graphics,
                 test_refresh_events, test_readonly):
        try:
            test()
        except Exception as ex:   # one broken case must not hide the others
            case(f"{test.__name__} ran to completion", False, f"{type(ex).__name__}: {ex}")
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
