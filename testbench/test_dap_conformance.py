#!/usr/bin/env python3
"""Checks the debug server against the Debug Adapter Protocol specification.

    python tests/fetch_dap_schema.py
    python testbench/test_dap_conformance.py <x16emu> <rom.bin>

Every case runs on its own private emulator, because the correct outcome of many
of them is that the process exits. Every message received is also validated
against the official schema. Without the schema those checks are skipped with a
message, unless X16_DAP_SCHEMA_REQUIRED is set, as it is in CI.
"""

import base64
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SCHEMA_PATH = Path(__file__).resolve().parent.parent / "tests" / "dap-schema" / "debugAdapterProtocol.json"

passed = 0
failed = 0
schema_errors = {}
DEFS = None


def case(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  PASS {name}", flush=True)
    else:
        failed += 1
        print(f"  FAIL {name}" + (f": {detail}" if detail else ""), flush=True)


# --- schema -----------------------------------------------------------------

def type_ok(value, t):
    if t == "object":
        return isinstance(value, dict)
    if t == "array":
        return isinstance(value, list)
    if t == "string":
        return isinstance(value, str)
    if t == "boolean":
        return isinstance(value, bool)
    if t == "null":
        return value is None
    if t == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if t == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return True


def check_node(node, value, path, errs):
    if "$ref" in node:
        check_node(DEFS[node["$ref"].split("/")[-1]], value, path, errs)
        return
    for part in node.get("allOf", []):
        check_node(part, value, path, errs)
    t = node.get("type")
    if t:
        kinds = t if isinstance(t, list) else [t]
        if not any(type_ok(value, k) for k in kinds):
            errs.append(f"{path}: expected {t}, got {type(value).__name__}")
            return
    if "enum" in node and value not in node["enum"]:
        errs.append(f"{path}: {value!r} not in {node['enum']}")
    if isinstance(value, dict):
        for r in node.get("required", []):
            if r not in value:
                errs.append(f"{path}: missing required '{r}'")
        for k, sub in node.get("properties", {}).items():
            if k in value:
                check_node(sub, value[k], f"{path}.{k}", errs)
    if isinstance(value, list) and "items" in node:
        for i, v in enumerate(value):
            check_node(node["items"], v, f"{path}[{i}]", errs)


def validate(msg):
    if DEFS is None:
        return
    kind = msg.get("type")
    if kind == "response":
        cmd = msg.get("command", "")
        if not msg.get("success"):
            name = "ErrorResponse"
        elif "/" in cmd:
            name = "Response"
        else:
            name = cmd[0].upper() + cmd[1:] + "Response"
            name = name if name in DEFS else "Response"
        label = f"response:{cmd}"
    elif kind == "event":
        ev = msg.get("event", "")
        name = ev[0].upper() + ev[1:] + "Event"
        name = name if name in DEFS else "Event"
        label = f"event:{ev}"
    else:
        name, label = "ProtocolMessage", f"message:{kind}"
    errs = []
    check_node(DEFS[name], msg, name, errs)
    for e in errs:
        schema_errors.setdefault(label, set()).add(e)


# --- client -----------------------------------------------------------------

class Dap:
    def __init__(self, port):
        self.msgs, self.buf, self.seq, self.closed = [], b"", 1, False
        deadline = time.time() + 20
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.02)

    def pump(self, timeout):
        self.s.settimeout(timeout)
        try:
            data = self.s.recv(1 << 20)
        except socket.timeout:
            return
        except OSError:
            self.closed = True
            return
        if not data:
            self.closed = True
            return
        self.buf += data
        while True:
            end = self.buf.find(b"\r\n\r\n")
            if end < 0:
                return
            length = None
            for line in self.buf[:end].split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            if length is None or len(self.buf) < end + 4 + length:
                return
            raw = self.buf[end + 4:end + 4 + length]
            self.buf = self.buf[end + 4 + length:]
            msg = json.loads(raw.decode("utf-8"))
            validate(msg)
            self.msgs.append(msg)

    def drain(self, secs):
        end = time.time() + secs
        while time.time() < end and not self.closed:
            self.pump(max(0.01, end - time.time()))

    def send(self, command, args=None):
        seq = self.seq
        self.seq += 1
        msg = {"seq": seq, "type": "request", "command": command}
        if args is not None:
            msg["arguments"] = args
        body = json.dumps(msg).encode()
        try:
            self.s.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        except OSError:
            self.closed = True
        return seq

    def response_to(self, seq, timeout=8):
        end = time.time() + timeout
        while True:
            for m in self.msgs:
                if m.get("type") == "response" and m.get("request_seq") == seq:
                    return m
            if time.time() >= end or self.closed:
                return None
            self.pump(0.05)

    def req(self, command, args=None, timeout=8):
        return self.response_to(self.send(command, args), timeout)

    def wait_event(self, name, start, timeout=8):
        end = time.time() + timeout
        while True:
            for m in self.msgs[start:]:
                if m.get("type") == "event" and m.get("event") == name:
                    return m
            if time.time() >= end or self.closed:
                return None
            self.pump(0.05)

    def status(self):
        r = self.req("x16/status")
        return r["body"] if r and r.get("success") else {}

    def var(self, name):
        r = self.req("variables", {"variablesReference": 1})
        for v in (r or {}).get("body", {}).get("variables", []):
            if v["name"] == name:
                return v["value"]
        return None

    def pause(self):
        if self.status().get("paused"):
            return
        start = len(self.msgs)
        self.req("pause", {"threadId": 1})
        self.wait_event("stopped", start)

    def breakpoints(self):
        r = self.req("x16/listBreakpoints")
        return (r or {}).get("body", {})

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


class Emu:
    """kind is "launch", "attach", or None for a session that sent neither."""

    def __init__(self, kind="attach", extra=(), handshake=True, warp=True, kind_args=None):
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        self.port = probe.getsockname()[1]
        probe.close()
        self.dir = tempfile.TemporaryDirectory()
        cmd = [EXE, "-rom", ROM, "-debugport", str(self.port), "-hidden", "-zeroram"]
        if warp:
            cmd.append("-warp")
        cmd += list(extra)
        self.p = subprocess.Popen(cmd, cwd=self.dir.name, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.d = Dap(self.port)
        if handshake:
            self.d.req("initialize", {"adapterID": "conformance", "linesStartAt1": True,
                                      "columnsStartAt1": True, "pathFormat": "path"})
            if kind:
                self.d.req(kind, kind_args or {})
            self.d.req("configurationDone")

    def exits(self, secs=8):
        try:
            self.p.wait(secs)
            return True
        except subprocess.TimeoutExpired:
            return False

    def stays_up(self, secs=2):
        return not self.exits(secs)

    def reconnects(self):
        """A dropped session must leave a server the next client can use."""
        self.d.close()
        time.sleep(0.2)
        if self.p.poll() is not None:
            return False
        try:
            d = Dap(self.port)
        except OSError:
            return False
        try:
            return bool(d.req("x16/status", timeout=5))
        finally:
            d.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.d.close()
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait(5)
        try:
            self.dir.cleanup()
        except OSError:
            pass   # Windows can hold the directory for a moment after exit


def order(msgs, cmd):
    seen = []
    for m in msgs:
        if m.get("type") == "response" and m.get("command") == cmd:
            seen.append("response")
        elif m.get("type") == "event" and m.get("event") == "stopped":
            seen.append("stopped")
    return seen


# --- cases ------------------------------------------------------------------

def test_evaluate_contexts():
    print("evaluate contexts")
    with Emu() as e:
        d = e.d
        d.pause()
        d.req("setInstructionBreakpoints", {"breakpoints": [{"instructionReference": "0xC000"}]})
        d.req("evaluate", {"expression": "watch_add 0400"})
        pc, cycles, listing = d.var("PC"), d.status().get("cycles"), d.breakpoints()

        commands = ("reset", "bp_clear", "bp_add C100", "bp_remove C000",
                    "watch_add 0500", "watch_remove 0400", "watch_clear")
        for ctx in ("hover", "watch", "clipboard", "variables"):
            refused = all(not (d.req("evaluate", {"expression": c, "context": ctx}) or {"success": True})["success"]
                          for c in commands)
            case(f"{ctx}: state-changing commands are refused", refused)
        case("nothing changed: PC", d.var("PC") == pc, f"{pc} -> {d.var('PC')}")
        case("nothing changed: cycles", d.status().get("cycles") == cycles)
        case("nothing changed: breakpoints and watchpoints", d.breakpoints() == listing)

        for expr in ("A", "PC", "$0000", "bp_list", "watch_list", "vram 1B000 2", "regs_all"):
            r = d.req("evaluate", {"expression": expr, "context": "hover"})
            case(f"hover can read {expr}", bool(r and r["success"]))

        for ctx in ("hover", "watch", "repl", None):
            for expr in ("someLabel", "$ZZZZ", "0xNOPE"):
                args = {"expression": expr}
                if ctx:
                    args["context"] = ctx
                r = d.req("evaluate", args)
                case(f"{ctx or 'no context'}: unknown {expr} is an error", bool(r) and not r["success"])

        r = d.req("evaluate", {"expression": "bp_add C100", "context": "repl"})
        case("repl runs bp_add", bool(r and r["success"]) and "added" in r["body"]["result"])
        r = d.req("evaluate", {"expression": "bp_remove C100"})
        case("a missing context is the console", bool(r and r["success"]) and "removed" in r["body"]["result"])


def test_disconnect():
    print("disconnect")
    for kind, args, ends in (("attach", {}, False),
                             ("launch", {}, True),
                             (None, {}, False),
                             ("attach", {"terminateDebuggee": True}, True),
                             ("launch", {"terminateDebuggee": False}, False),
                             (None, {"terminateDebuggee": True}, True)):
        name = f"{kind or 'no launch or attach'} + disconnect {args or ''}".strip()
        with Emu(kind) as e:
            r = e.d.req("disconnect", args)
            case(f"{name}: answered", bool(r and r["success"]))
            if ends:
                case(f"{name}: emulator exits", e.exits())
            else:
                case(f"{name}: emulator stays up", e.stays_up())
                case(f"{name}: next client connects", e.reconnects())


def test_terminate():
    print("terminate")
    with Emu("launch") as e:
        d = e.d
        d.pause()
        start = len(d.msgs)
        seq = d.send("terminate", {})
        r = d.response_to(seq)
        d.wait_event("terminated", start)
        names = [m.get("event") or "response" for m in d.msgs[start:]
                 if m.get("type") == "event" or m.get("request_seq") == seq]
        case("launch: answered", bool(r and r["success"]))
        case("launch: response, exited, terminated in order",
             [n for n in names if n in ("response", "exited", "terminated")] == ["response", "exited", "terminated"],
             str(names))
        case("launch: emulator exits", e.exits())

    for kind in ("attach", None):
        label = kind or "no launch or attach"
        with Emu(kind) as e:
            d = e.d
            d.pause()
            d.req("setInstructionBreakpoints", {"breakpoints": [{"instructionReference": "0xC000"}]})
            start = len(d.msgs)
            r = d.req("terminate", {})
            ev = d.wait_event("terminated", start)
            case(f"{label}: answered", bool(r and r["success"]))
            case(f"{label}: terminated event", ev is not None)
            case(f"{label}: emulator stays up", e.stays_up())
            st = d.status()
            case(f"{label}: machine resumed", st.get("paused") is False, str(st.get("paused")))
            case(f"{label}: breakpoints released", not d.breakpoints().get("breakpoints"))
            case(f"{label}: next client connects", e.reconnects())


def test_dropped_socket():
    print("dropped socket")
    for kind in ("launch", "attach"):
        with Emu(kind) as e:
            e.d.close()
            case(f"{kind}: a dropped socket never quits the emulator", e.stays_up())
            case(f"{kind}: next client connects", e.reconnects())


def test_events_wait_for_initialize():
    print("events before initialize")
    # At normal speed, so the client is connected before the boot reaches the
    # breakpoint. Under -warp the stop can happen with nobody there to be told.
    with Emu(extra=["-bp", "FFCF"], handshake=False, warp=False) as e:
        d = e.d
        end = time.time() + 30
        while time.time() < end and not d.status().get("paused"):
            time.sleep(0.02)
        d.drain(0.2)
        case("the machine stopped before initialize", bool(d.status().get("paused")))
        early = [m.get("event") for m in d.msgs if m.get("type") == "event"]
        case("no event before the initialize response", not early, str(early[:4]))

        start = len(d.msgs)
        init_seq = d.send("initialize", {"adapterID": "conformance"})
        d.response_to(init_seq)
        d.wait_event("initialized", start)
        got = [m for m in d.msgs[start:] if m.get("type") == "response" or m.get("event") == "initialized"]
        case("initialized follows the response",
             len(got) >= 2 and got[0].get("type") == "response" and got[1].get("event") == "initialized")

        d.req("launch", {})
        start = len(d.msgs)
        d.req("configurationDone")
        case("configurationDone reports the stop", d.wait_event("stopped", start) is not None)
        d.drain(0.3)
        stops = [m for m in d.msgs[start:] if m.get("event") == "stopped"]
        case("the stop is reported once", len(stops) == 1, f"{len(stops)} stopped events")


def test_pause_order():
    print("pause")
    with Emu() as e:
        d = e.d
        start = len(d.msgs)
        d.req("pause", {"threadId": 1})
        d.wait_event("stopped", start)
        d.drain(0.2)
        seen = order(d.msgs[start:], "pause")
        case("response comes before stopped", seen[:2] == ["response", "stopped"], str(seen))


def test_goto():
    print("goto")
    with Emu() as e:
        d = e.d
        start = len(d.msgs)
        r = d.req("goto", {"threadId": 1, "targetId": 0xC000})
        case("running: refused with notStopped", bool(r) and not r["success"] and r.get("message") == "notStopped",
             str(r and r.get("message")))
        d.drain(0.3)
        case("running: no stopped event", not [m for m in d.msgs[start:] if m.get("event") == "stopped"])
        case("running: machine still running", d.status().get("paused") is False)

        d.pause()
        start = len(d.msgs)
        r = d.req("goto", {"threadId": 1, "targetId": 0xC000})
        ev = d.wait_event("stopped", start)
        case("stopped: accepted", bool(r and r["success"]))
        case("stopped: reports reason goto", bool(ev) and ev["body"]["reason"] == "goto")
        case("stopped: response comes first", order(d.msgs[start:], "goto")[:2] == ["response", "stopped"])
        case("stopped: PC moved", (d.var("PC") or "").startswith("$C000"), str(d.var("PC")))

        # A goto that is refused has not taken over execution, so it must leave
        # a step in flight alone.
        step = d.send("x16/step", {"frames": 1000000})
        time.sleep(0.1)
        r = d.req("goto", {"threadId": 1, "targetId": 0xC000})
        case("during a step: refused", bool(r) and not r["success"])
        d.req("pause", {"threadId": 1})
        s = d.response_to(step)
        case("during a step: the step is not cancelled",
             bool(s) and s.get("body", {}).get("reason") == "pause",
             str(s and s.get("body", {}).get("reason")))


def test_set_variable():
    print("setVariable")
    with Emu() as e:
        d = e.d
        d.pause()

        def put(value, name="A"):
            return d.req("setVariable", {"variablesReference": 1, "name": name, "value": value})

        case("accepts $hex", bool((put("$12") or {}).get("success")))
        for bad in ("not-a-number", "", "$", "0x", "$GG", "12abc", "-1", "+1", "$0x12",
                    "$12 (19)", "$12 (", "$12 [nv", "4294967296"):
            r = put(bad)
            case(f"rejects {bad!r}", bool(r) and not r["success"], str(r and r.get("body")))
        case("a rejected value changes nothing", (d.var("A") or "").startswith("$12"), str(d.var("A")))

        r = put("0x1FF")
        a = d.var("A")
        case("reports the stored value", bool(r and r["success"]) and r["body"]["value"] == a,
             f"response {r and r['body'].get('value')!r}, stored {a!r}")
        case("stores the low byte of an 8-bit register", (a or "").startswith("$FF"), str(a))

        r = put("$34 (52)")
        case("accepts the form the view displays", bool(r and r["success"]) and (d.var("A") or "").startswith("$34"))
        r = put("52")
        case("reads a plain number as decimal", bool(r and r["success"]) and (d.var("A") or "").startswith("$34"))

        shown = d.var("P (Status)")
        r = put(shown, "P (Status)")
        case("accepts the status register as displayed", bool(r and r["success"]), str(shown))

        r = put("$C123", "PC")
        case("reports a 16-bit value", bool(r and r["success"]) and r["body"]["value"].startswith("$C123"))


# --- breakpoints ------------------------------------------------------------

def write_debug_info(folder):
    """Two source lines at $0800 and $0803, and a label on the second."""
    src = os.path.join(folder, "fixture.c").replace("\\", "/")
    with open(src, "w") as f:
        f.write("first();\nsecond();\nthird();\n")
    dbg = os.path.join(folder, "fixture.dbg")
    with open(dbg, "w", newline="\n") as f:
        f.write('version\tmajor=2,minor=0\n')
        f.write('info\tcsym=0,file=1,lib=0,line=2,mod=1,scope=1,seg=1,span=2,sym=1,type=0\n')
        f.write(f'file\tid=0,name="{src}",size=30,mtime=0x00000000,mod=0\n')
        f.write('mod\tid=0,name="fixture.o",file=0\n')
        f.write('seg\tid=0,name="CODE",start=0x000800,size=0x0040,addrsize=absolute,type=rw,oname="f.prg",ooffs=0\n')
        f.write('span\tid=0,seg=0,start=0,size=3\n')
        f.write('span\tid=1,seg=0,start=3,size=3\n')
        f.write('scope\tid=0,name="",mod=0,size=64,span=0+1\n')
        f.write('line\tid=0,file=0,line=1,type=1,span=0\n')
        f.write('line\tid=1,file=0,line=2,type=1,span=1\n')
        f.write('sym\tid=0,name="conflabel",addrsize=absolute,size=1,scope=0,def=0,type=lab,val=0x0803,seg=0\n')
    return src, dbg


def put_mem(d, addr, data):
    d.req("writeMemory", {"memoryReference": hex(addr), "data": base64.b64encode(bytes(data)).decode()})


def get_byte(d, addr):
    r = d.req("readMemory", {"memoryReference": hex(addr), "count": 1})
    return base64.b64decode(r["body"]["data"])[0]


def set_reg(d, name, value):
    d.req("setVariable", {"variablesReference": 1, "name": name, "value": value})


def plant(d, a="$00", pc="$0800"):
    """NOPs looping at $0800, and INC $1000 looping at $0810, with IRQs masked."""
    d.pause()
    put_mem(d, 0x0800, [0xEA] * 6 + [0x4C, 0x00, 0x08])
    put_mem(d, 0x0810, [0xEE, 0x00, 0x10, 0x4C, 0x10, 0x08])
    put_mem(d, 0x1000, [0])
    set_reg(d, "P (Status)", "$24")
    set_reg(d, "A", a)
    set_reg(d, "PC", pc)


def go(d, secs=3):
    start = len(d.msgs)
    d.req("continue", {"threadId": 1})
    return d.wait_event("stopped", start, secs)


def nothing_armed(d):
    listing = d.breakpoints()
    return not listing.get("breakpoints") and not listing.get("watchpoints")


def refused(r, n=0):
    bps = (r or {}).get("body", {}).get("breakpoints", [])
    return len(bps) > n and bps[n].get("verified") is False and bool(bps[n].get("message"))


def test_source_breakpoints():
    print("source breakpoints")
    with tempfile.TemporaryDirectory() as t:
        src, dbg = write_debug_info(t)
        with Emu(kind_args={"dbgFile": dbg}) as e:
            d = e.d
            plant(d)
            source = {"path": src}

            r = d.req("setBreakpoints", {"source": source})
            case("breakpoints is optional", bool(r and r["success"]) and r["body"]["breakpoints"] == [])

            r = d.req("setBreakpoints", {"source": source, "lines": [1]})
            case("the deprecated lines form is read",
                 bool(r and r["success"]) and r["body"]["breakpoints"][0].get("verified") is True, str(r))
            d.req("setBreakpoints", {"source": source, "breakpoints": []})
            case("an empty list clears it", nothing_armed(d))

            for label, bp in (("an unreadable condition", {"line": 1, "condition": "A == banana"}),
                              ("an unreadable address", {"line": 1, "condition": "byte[zz] != 0"}),
                              ("trailing text", {"line": 1, "condition": "A == 5 junk"}),
                              ("a bank out of range", {"line": 1, "condition": "bank == 300"}),
                              ("a bank on unbanked memory", {"line": 1, "condition": "bank == 2"}),
                              ("a modulo hitCondition", {"line": 1, "hitCondition": "% 3"}),
                              ("a greater-than hitCondition", {"line": 1, "hitCondition": "> 5"}),
                              ("a pending line with a bad condition", {"line": 99, "condition": "nope"})):
                r = d.req("setBreakpoints", {"source": source, "breakpoints": [bp]})
                case(f"refused: {label}", refused(r), str(r and r.get("body")))
                case(f"nothing armed: {label}", nothing_armed(d))

            bps = [{"line": 1000 + i} for i in range(300)]
            r = d.req("setBreakpoints", {"source": source, "breakpoints": bps})
            got = (r or {}).get("body", {}).get("breakpoints", [])
            case("one reply per entry past the table's size", len(got) == 300, str(len(got)))
            case("entries that did not fit are refused", len(got) == 300 and refused(r, 299))
            d.req("setBreakpoints", {"source": source, "breakpoints": []})

            r = d.req("setBreakpoints", {"source": source, "breakpoints": [{"line": 1, "condition": "A == 1"}]})
            bp_id = r["body"]["breakpoints"][0]["id"]
            case("a condition that can apply is verified", r["body"]["breakpoints"][0]["verified"] is True)
            case("it does not stop while false", go(d, 0.4) is None)
            d.pause()
            set_reg(d, "A", "$01")
            set_reg(d, "PC", "$0800")
            ev = go(d)
            case("it stops once true", bool(ev) and ev["body"]["reason"] == "breakpoint", str(ev))
            case("the stop names it", bool(ev) and ev["body"].get("hitBreakpointIds") == [bp_id], str(ev))


def test_function_breakpoints():
    print("function breakpoints")
    with tempfile.TemporaryDirectory() as t:
        _, dbg = write_debug_info(t)
        with Emu(kind_args={"dbgFile": dbg}) as e:
            d = e.d
            plant(d)

            def fn(**bp):
                return d.req("setFunctionBreakpoints", {"breakpoints": [dict(name="conflabel", **bp)]})

            r = fn()
            b = r["body"]["breakpoints"][0]
            case("verified", b.get("verified") is True, str(b))
            case("instructionReference is a string", b.get("instructionReference") == "0x0803", str(b))
            d.req("setFunctionBreakpoints", {"breakpoints": []})

            r = d.req("setFunctionBreakpoints", {"breakpoints": [{"name": "nosuchlabel"}]})
            case("refused: an unknown label", refused(r), str(r and r.get("body")))
            for label, bp in (("an unreadable condition", {"condition": "A == banana"}),
                              ("an unreadable hitCondition", {"hitCondition": "% 2"})):
                r = fn(**bp)
                case(f"refused: {label}", refused(r), str(r and r.get("body")))
                case(f"nothing armed: {label}", nothing_armed(d))

            r = fn(condition="A == 1")
            bp_id = r["body"]["breakpoints"][0]["id"]
            case("the condition applies: no stop while false", go(d, 0.4) is None)
            d.pause()
            set_reg(d, "A", "$01")
            set_reg(d, "PC", "$0800")
            ev = go(d)
            case("stops once true", bool(ev) and ev["body"]["reason"] == "function breakpoint", str(ev))
            case("the stop names it", bool(ev) and ev["body"].get("hitBreakpointIds") == [bp_id], str(ev))

            fn(hitCondition="3")
            entry = d.breakpoints()["breakpoints"][0]
            case("hitCondition reaches the core", entry.get("ignoreCount") == 2, str(entry))

            fn(condition="A == 7")
            d.req("setFunctionBreakpoints", {"breakpoints": []})
            d.req("evaluate", {"expression": "bp_add 0803"})
            entry = d.breakpoints()["breakpoints"][0]
            case("a removed condition is not inherited", entry.get("condition") is None, str(entry))
            d.req("evaluate", {"expression": "bp_clear"})

            d.req("setInstructionBreakpoints",
                  {"breakpoints": [{"instructionReference": "0x0803", "condition": "A == 1"}]})
            before = d.breakpoints()
            r = fn()
            case("refused: would change another breakpoint's condition", refused(r), str(r and r.get("body")))
            case("the other breakpoint is unchanged", d.breakpoints() == before)


def test_instruction_stop():
    print("instruction breakpoint stops")
    with Emu() as e:
        d = e.d
        plant(d)
        r = d.req("setInstructionBreakpoints", {"breakpoints": [{"instructionReference": "0x0803"}]})
        bp_id = r["body"]["breakpoints"][0]["id"]
        ev = go(d)
        case("reason", bool(ev) and ev["body"]["reason"] == "instruction breakpoint", str(ev))
        case("the stop names it", bool(ev) and ev["body"].get("hitBreakpointIds") == [bp_id], str(ev))


def test_data_breakpoints():
    print("data breakpoints")
    with Emu() as e:
        d = e.d
        plant(d, pc="$0810")

        def info(name, ref=None):
            args = {"name": name}
            if ref is not None:
                args["variablesReference"] = ref
            r = d.req("dataBreakpointInfo", args)
            return r["body"].get("dataId") if r and r["success"] else "failed"

        for name, ref in (("A", 1), ("PC", 1), ("R0", 1), ("$00-$0F", 2), ("A", None), ("banana", None),
                          ("$12345", None), ("R16", 4), ("$0000", 3)):
            case(f"no data breakpoint for {name} in scope {ref}", info(name, ref) is None, str(info(name, ref)))
        case("R0 watches both of its bytes", info("R0", 4) == "2:2", str(info("R0", 4)))
        case("R15 watches both of its bytes", info("R15", 4) == "32:2", str(info("R15", 4)))
        case("a stack entry is its address", info("$01FF", 3) == "511", str(info("$01FF", 3)))
        case("an address expression", info("$1000") == "4096", str(info("$1000")))

        def data(**bp):
            return d.req("setDataBreakpoints", {"breakpoints": [bp]})

        for label, bp in (("an unknown access type", {"dataId": "4096", "accessType": "execute"}),
                          ("read access on VRAM", {"dataId": "vram:04000", "accessType": "read"}),
                          ("a hitCondition", {"dataId": "4096", "hitCondition": "3"}),
                          ("an unreadable condition", {"dataId": "4096", "condition": "== banana"}),
                          ("a condition past a byte", {"dataId": "4096", "condition": "== 300"}),
                          ("a value test on two bytes", {"dataId": "2:2", "condition": "== 1"}),
                          ("an invalid dataId", {"dataId": "banana"}),
                          ("an address past 16 bits", {"dataId": "65536"})):
            r = data(**bp)
            case(f"refused: {label}", refused(r), str(r and r.get("body")))
            case(f"nothing armed: {label}", nothing_armed(d))

        r = data(dataId="2:2", accessType="write")
        wps = d.breakpoints().get("watchpoints", [])
        case("a two-byte watch is armed as two bytes", len(wps) == 1 and wps[0]["len"] == 2, str(wps))

        r = data(dataId="4096", accessType="write")
        b = r["body"]["breakpoints"][0]
        case("a write watch is verified and has an id", b.get("verified") is True and "id" in b, str(b))
        ev = go(d)
        case("reason", bool(ev) and ev["body"]["reason"] == "data breakpoint", str(ev))
        case("the stop names it", bool(ev) and ev["body"].get("hitBreakpointIds") == [b.get("id")], str(ev))

        r = data(dataId="4096", condition="== 5")
        b = r["body"]["breakpoints"][0]
        ev = go(d)
        case("a value condition applies", bool(ev) and get_byte(d, 0x1000) == 5, str(get_byte(d, 0x1000)))
        case("a filtered stop names it", bool(ev) and ev["body"].get("hitBreakpointIds") == [b.get("id")], str(ev))

        d.req("setDataBreakpoints", {"breakpoints": []})
        d.req("evaluate", {"expression": "watch_add 1000"})
        before = d.breakpoints()
        r = data(dataId="4096", condition="== 9")
        case("refused: would change another owner's watch", refused(r), str(r and r.get("body")))
        case("the other watch is unchanged", d.breakpoints() == before)


# --- views and wire format --------------------------------------------------

def session(init, kind_args):
    """An emulator whose initialize carried `init`."""
    e = Emu(kind=None, handshake=False)
    e.d.req("initialize", dict({"adapterID": "conformance"}, **init))
    e.d.req("attach" if "noDebug" not in kind_args else "launch", kind_args)
    e.d.req("configurationDone")
    return e


def top_frame(d):
    return d.req("stackTrace", {"threadId": 1})["body"]["stackFrames"][0]


def test_stack_frames():
    print("stack frames")
    with Emu() as e:
        d = e.d
        plant(d)
        f = top_frame(d)
        case("no source: line and column are 0", f.get("line") == 0 and f.get("column") == 0, str(f))
        case("no source: the field is absent", "source" not in f, str(f))
        case("presentationHint is a string", isinstance(f.get("presentationHint"), str), str(f))
        case("the disassembly is still reachable", bool(f.get("instructionPointerReference")), str(f))


def test_line_base():
    print("zero-based lines")
    with tempfile.TemporaryDirectory() as t:
        src, dbg = write_debug_info(t)
        with session({"linesStartAt1": False, "columnsStartAt1": False}, {"dbgFile": dbg}) as e:
            d = e.d
            plant(d)
            r = d.req("setBreakpoints", {"source": {"path": src}, "breakpoints": [{"line": 1}]})
            b = r["body"]["breakpoints"][0]
            case("line 1 is the second source line", b.get("verified") is True and b.get("line") == 1, str(b))
            entry = d.breakpoints()["breakpoints"][0]
            case("it armed at that line's address", entry.get("pc") == 0x0803, str(entry))
            set_reg(d, "PC", "$0803")
            f = top_frame(d)
            case("frames use the same base", f.get("line") == 1 and f.get("column") == 0, str(f))
            r = d.req("breakpointLocations", {"source": {"path": src}, "line": 0, "endLine": 1})
            lines = sorted(b["line"] for b in r["body"]["breakpoints"])
            case("breakpointLocations too", lines == [0, 1], str(lines))
            r = d.req("gotoTargets", {"source": {"path": src}, "line": 1})
            case("gotoTargets too", r["success"] and r["body"]["targets"][0]["line"] == 1, str(r))


def test_uri_paths():
    print("uri paths")
    with tempfile.TemporaryDirectory() as t:
        src, dbg = write_debug_info(t)
        uri = Path(src).as_uri()
        with session({"pathFormat": "uri"}, {"dbgFile": dbg}) as e:
            d = e.d
            plant(d)
            r = d.req("setBreakpoints", {"source": {"path": uri}, "breakpoints": [{"line": 1}]})
            b = r["body"]["breakpoints"][0]
            case("a file: URI finds the file", b.get("verified") is True, str(b))
            case("the reply echoes the client's spelling", b.get("source", {}).get("path") == uri, str(b))
            f = top_frame(d)
            case("frames carry a URI", f.get("source", {}).get("path", "").startswith("file://"), str(f))
            r = d.req("source", {"source": {"path": uri}, "sourceReference": 0})
            case("source reads through a URI", bool(r and r["success"]) and "first();" in r["body"]["content"], str(r)[:200])


def test_no_debug():
    print("noDebug")
    with session({}, {"noDebug": True}) as e:
        d = e.d
        for cmd, args in (("setBreakpoints", {"source": {"path": "x.c"}, "breakpoints": [{"line": 1}]}),
                          ("setFunctionBreakpoints", {"breakpoints": [{"name": "x"}]}),
                          ("setInstructionBreakpoints", {"breakpoints": [{"instructionReference": "0x0800"}]}),
                          ("setDataBreakpoints", {"breakpoints": [{"dataId": "4096"}]})):
            r = d.req(cmd, args)
            case(f"{cmd} is refused", refused(r), str(r and r.get("body")))
        case("nothing armed", nothing_armed(d))
        case("the machine runs", not d.status().get("paused"))


def test_disassemble():
    print("disassemble")
    with Emu() as e:
        d = e.d
        plant(d)

        def dis(**args):
            r = d.req("disassemble", dict({"memoryReference": "0x0800", "instructionCount": 4}, **args))
            return r["body"]["instructions"] if r and r["success"] else None

        def addr(row):
            return int(row["address"], 16)

        got = dis()
        case("exactly the count", got is not None and len(got) == 4, str(got and len(got)))
        case("from the reference", bool(got) and addr(got[0]) == 0x0800, str(got and got[0]))
        got = dis(instructionCount=500)
        case("past the old 200 row cap", got is not None and len(got) == 500, str(got and len(got)))
        got = dis(instructionOffset=2)
        case("a positive instructionOffset", bool(got) and addr(got[0]) == 0x0802, str(got and got[0]))
        got = dis(instructionOffset=-2)
        case("a negative instructionOffset", bool(got) and addr(got[0]) == 0x07FE or
             (bool(got) and addr(got[0]) < 0x0800), str(got and got[0]))
        got = dis(memoryReference="0x0001", instructionOffset=-10, instructionCount=12)
        case("padding before $0000 keeps the count", got is not None and len(got) == 12, str(got and len(got)))
        case("the padding is marked invalid", bool(got) and got[0].get("presentationHint") == "invalid", str(got and got[0]))
        got = dis(memoryReference="0xFFFE", instructionCount=20)
        case("padding past $FFFF keeps the count", got is not None and len(got) == 20, str(got and len(got)))
        case("the tail is marked invalid", bool(got) and got[-1].get("presentationHint") == "invalid", str(got and got[-1]))
        for ref in ("banana", "vram:1000"):
            r = d.req("disassemble", {"memoryReference": ref, "instructionCount": 1})
            case(f"refused: {ref}", bool(r) and r["success"] is False, str(r))
        r = d.req("disassemble", {"memoryReference": "0x0800", "instructionCount": 70000})
        case("refused: more rows than it will send", bool(r) and r["success"] is False, str(r)[:200])

    with tempfile.TemporaryDirectory() as t:
        src, dbg = write_debug_info(t)
        with Emu(kind_args={"dbgFile": dbg}) as e:
            plant(e.d)
            r = e.d.req("disassemble", {"memoryReference": "0x0800", "instructionCount": 1})
            row = r["body"]["instructions"][0]
            case("the source goes in location", "location" in row and "source" not in row, str(row))


def test_read_memory():
    print("readMemory")
    with Emu() as e:
        d = e.d
        d.pause()

        def read(ref, count):
            r = d.req("readMemory", {"memoryReference": ref, "count": count}, timeout=20)
            b = r["body"] if r and r["success"] else {}
            return len(base64.b64decode(b.get("data", ""))), b.get("unreadableBytes", 0)

        case("past the old 4096 cap", read("0x0000", 5000) == (5000, 0), str(read("0x0000", 5000)))
        case("a reply over 64K arrives whole", read("vram:0", 0x20000) == (0x20000, 0))
        case("the end of VRAM is unreadable, not short", read("vram:1FFFE", 8) == (2, 6), str(read("vram:1FFFE", 8)))
        case("a zero count", read("0x0000", 0) == (0, 0), str(read("0x0000", 0)))


def test_wire_format():
    print("wire format")
    with tempfile.TemporaryDirectory() as t:
        path = os.path.join(t, "latin1.c")
        with open(path, "wb") as f:
            f.write(b"caf\xe9 \xed\xa0\x80 ok\n")
        with Emu() as e:
            d = e.d
            # The client parses every frame as UTF-8 and would raise on a bad one.
            r = d.req("source", {"source": {"path": path}, "sourceReference": 0})
            case("invalid UTF-8 arrives as U+FFFD", bool(r and r["success"]) and "�" in r["body"]["content"], str(r)[:200])
            case("the valid text survives", bool(r and r["success"]) and "ok" in r["body"]["content"])

            a = d.req("readMemory", {"memoryReference": "banana", "count": 1})
            b = d.req("nosuchcommand")
            ida, idb = a["body"]["error"]["id"], b["body"]["error"]["id"]
            case("different errors, different ids", ida != idb, f"{ida} {idb}")
            again = d.req("nosuchcommand")
            case("the same error, the same id", again["body"]["error"]["id"] == idb)
            r = d.req("x16/joystick", {"index": 0, "buttons": ["{nope}"]})
            err = r["body"]["error"]
            case("a brace travels in a variable",
                 err["format"] == "{text}" and "{nope}" in err.get("variables", {}).get("text", ""), str(err))

            caps = d.req("initialize", {"adapterID": "x"})
            case("no value formatting is claimed",
                 "supportsValueFormattingOptions" not in ((caps or {}).get("body") or {}), str(caps)[:120])


def test_step_granularity():
    print("step granularity")
    with tempfile.TemporaryDirectory() as t:
        _, dbg = write_debug_info(t)
        with Emu(kind_args={"dbgFile": dbg}) as e:
            d = e.d
            plant(d)

            def step(cmd, **args):
                set_reg(d, "PC", "$0800")
                start = len(d.msgs)
                d.req(cmd, dict({"threadId": 1}, **args))
                d.wait_event("stopped", start, 4)
                return int(str(d.var("PC")).lstrip("$").split()[0], 16)

            for cmd in ("next", "stepIn"):
                pc = step(cmd)
                case(f"{cmd} moves a source line", pc == 0x0803, hex(pc))
                pc = step(cmd, granularity="instruction")
                case(f"{cmd} by instruction moves one", pc == 0x0801, hex(pc))


# --- main -------------------------------------------------------------------

def main():
    global EXE, ROM, DEFS
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    EXE, ROM = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])

    if SCHEMA_PATH.exists():
        DEFS = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))["definitions"]
    elif os.environ.get("X16_DAP_SCHEMA_REQUIRED"):
        print("the DAP schema is required here; run tests/fetch_dap_schema.py", file=sys.stderr)
        return 1
    else:
        print("SKIP schema validation: run python tests/fetch_dap_schema.py to enable it")

    for test in (test_evaluate_contexts, test_disconnect, test_terminate, test_dropped_socket,
                 test_events_wait_for_initialize, test_pause_order, test_goto, test_set_variable,
                 test_source_breakpoints, test_function_breakpoints, test_instruction_stop,
                 test_data_breakpoints, test_stack_frames, test_line_base, test_uri_paths,
                 test_no_debug, test_disassemble, test_read_memory, test_wire_format,
                 test_step_granularity):
        try:
            test()
        except Exception as ex:   # one broken case must not hide the others
            case(f"{test.__name__} ran to completion", False, f"{type(ex).__name__}: {ex}")

    if DEFS is not None:
        print("schema")
        case("every message matched the schema", not schema_errors,
             "; ".join(f"{k}: {sorted(v)[0]}" for k, v in sorted(schema_errors.items())))

    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
