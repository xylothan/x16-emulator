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

    def __init__(self, kind="attach", extra=(), handshake=True, warp=True, kind_args=None, files=None):
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        self.port = probe.getsockname()[1]
        probe.close()
        self.dir = tempfile.TemporaryDirectory()
        for name, text in (files or {}).items():
            with open(os.path.join(self.dir.name, name), "w", newline="\n") as f:
                f.write(text)
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


def test_expressions():
    print("expressions")
    with Emu() as e:
        d = e.d
        d.pause()
        init = next(m for m in d.msgs if m.get("command") == "initialize")
        case("setExpression is advertised", init["body"].get("supportsSetExpression") is True)
        case("completions are advertised", init["body"].get("supportsCompletionsRequest") is True)

        def ev(expr, **extra):
            return d.req("evaluate", dict({"expression": expr}, **extra))

        def result(expr):
            r = ev(expr)
            return r["body"]["result"] if r and r["success"] else None

        def put(expr, value):
            return d.req("setExpression", {"expression": expr, "value": value})

        case("arithmetic", result("2+3*4") == "$0E (14)", str(result("2+3*4")))
        case("width", result("$FFFFFFFF+1") == "$00 (0)", str(result("$FFFFFFFF+1")))
        for bad in ("1/0", "1%0", "(1", "1+", "nosuchname", "beef", "A=5", "x" * 300):
            r = ev(bad)
            case(f"{bad[:12]!r} is an error", bool(r) and not r["success"], str(r)[:120])

        put("byte[$0400]", "$5A")
        case("$addr alone is still a peek", result("$0400") == "$5A (90)", str(result("$0400")))
        case("0addr alone is still a peek", result("0400") == "$5A (90)", str(result("0400")))
        case("in an expression $addr is a number", result("$0400+0") == "$0400 (1024)",
             str(result("$0400+0")))
        case("byte[] reads memory", result("byte[$0400]+1") == "$5B (91)", str(result("byte[$0400]+1")))
        case("A alone keeps its form", (result("A") or "").startswith("$"), str(result("A")))

        for reg, at in (("$9F20", 0x00), ("$9F21", 0x10), ("$9F22", 0x10)):
            d.req("writeMemory", {"memoryReference": reg.replace("$", "0x"),
                                  "data": base64.b64encode(bytes([at])).decode()})
        cycles = d.status().get("cycles")
        for ctx in ("hover", "watch", "repl"):
            ev("byte[$9F23]+byte[$9F23]", context=ctx)
        case("reading the data port does not advance it",
             result("byte[$9F20]") == "$00 (0)" and result("byte[$9F21]") == "$10 (16)",
             f"{result('byte[$9F20]')} {result('byte[$9F21]')}")
        case("evaluating runs nothing", d.status().get("cycles") == cycles)

        r = put("A", "$12")
        case("a register is set", bool(r and r["success"]) and (d.var("A") or "").startswith("$12"), str(r))
        case("reports what it holds", r["body"]["value"] == d.var("A"), f"{r['body']} {d.var('A')}")
        r = put("a", "$1FF")
        case("a wide value is cut to fit", bool(r and r["success"]) and (d.var("A") or "").startswith("$FF"), str(r))
        r = put("A", "$34 (52)")
        case("takes the form the view displays", bool(r and r["success"]) and (d.var("A") or "").startswith("$34"), str(r))
        r = put("X", "A+1")
        case("the value is an expression", bool(r and r["success"]) and (d.var("X") or "").startswith("$35"), str(r))

        r = put("byte[$0410+1]", "$AB")
        case("a byte is set", bool(r and r["success"]) and result("byte[$0411]") == "$AB (171)", str(r))
        r = put("word[$0500]", "$1234")
        case("a word is set, low byte first",
             bool(r and r["success"]) and result("byte[$0500]") == "$34 (52)"
             and result("byte[$0501]") == "$12 (18)", str(r))
        r = put("vbyte[$01000]", "$77")
        case("a VRAM byte is set", bool(r and r["success"]) and result("vbyte[$1000]") == "$77 (119)", str(r))
        r = put("R3", "$BEEF")
        case("R3 is set where it lives", bool(r and r["success"]) and result("word[$08]") == "$BEEF (48879)", str(r))
        r = put("PC", "$0810")
        case("PC is set", bool(r and r["success"]) and (d.var("PC") or "").startswith("$0810"), str(r))

        put("rambank", "3")
        put("byte[$A000]", "$11")
        put("rambank", "5")
        put("byte[$A000]", "$55")
        case("rambank is set", result("rambank") == "$05 (5)", str(result("rambank")))
        case("a named bank is read without switching",
             result("byte[3:$A000]") == "$11 (17)" and result("rambank") == "$05 (5)",
             str(result("byte[3:$A000]")))
        r = put("byte[3:$A000]", "$22")
        case("a write to an unmapped bank is refused", bool(r) and not r["success"], str(r))
        case("and stores nothing", result("byte[3:$A000]") == "$11 (17)" and result("byte[5:$A000]") == "$55 (85)")
        r = put("byte[5:$A000]", "$66")
        case("a write to the mapped bank works", bool(r and r["success"]) and result("byte[5:$A000]") == "$66 (102)", str(r))

        before = result("byte[$0400]")
        cycles = d.status().get("cycles")
        for target, value in (("1+1", "1"), ("A+1", "1"), ("!byte[$0400]", "1"), ("reset", "1"),
                              ("bp_clear", "1"), ("byte[$1000000]", "1"), ("byte[$0400]", "1/0"),
                              ("byte[$0400]", "nosuch"), ("byte[$0400]", "010")):
            r = put(target, value)
            case(f"setExpression {target} = {value} is refused", bool(r) and not r["success"], str(r)[:120])
        case("a refused set stores nothing", result("byte[$0400]") == before)
        case("and runs nothing", d.status().get("cycles") == cycles)
        for args in ({}, {"expression": "A"}, {"value": "1"}, {"expression": 1, "value": "1"}):
            r = d.req("setExpression", args)
            case(f"setExpression {args} is refused", bool(r) and not r["success"], str(r)[:120])

        with tempfile.TemporaryDirectory() as t:
            path = os.path.join(t, "prog.lbl")
            with open(path, "w") as f:
                f.write("al 000600 .start_here\nal 000610 .START_HERE\nal 000620 .rambuf\nal 000630 .other\n")
            r = d.req("x16/loadSymbols", {"path": path})
            case("labels load", bool(r and r["success"]), str(r))

        def complete(text, column, **extra):
            r = d.req("completions", dict({"text": text, "column": column}, **extra))
            return r["body"]["targets"] if r and r["success"] else None

        t = complete("ra", 3)
        case("completes a register and a symbol",
             sorted(i["label"] for i in t or []) == ["rambank", "rambuf"], str(t))
        case("replaces the fragment", all(i["start"] == 1 and i["length"] == 2 for i in t or []), str(t))
        t = complete("byte[STA", 9)
        case("matches case-insensitively, once each",
             [i["label"].lower() for i in t or []] == ["start_here"], str(t))
        case("starts after the bracket", bool(t) and t[0]["start"] == 6 and t[0]["length"] == 3, str(t))
        t = complete("r1", 3)
        case("registers come by prefix", {"R1", "R10", "R15"} <= {i["label"] for i in t or []}, str(t))
        t = complete("rambuf+other", 3)
        case("uses the text before the cursor",
             sorted(i["label"] for i in t or []) == ["rambank", "rambuf"], str(t))
        t = complete("x\nra", 3, line=2)
        case("line selects the line", sorted(i["label"] for i in t or []) == ["rambank", "rambuf"], str(t))
        case("start counts within the whole text", bool(t) and t[0]["start"] == 3, str(t))
        t = complete("\U0001F600ra", 5)
        case("columns are UTF-16", sorted(i["label"] for i in t or []) == ["rambank", "rambuf"], str(t))
        case("and so is start", bool(t) and t[0]["start"] == 3, str(t))
        for text, col in (("$ra", 4), ("12", 3), ("0xra", 5)):
            case(f"{text!r} is a number and has none", complete(text, col) == [], str(complete(text, col)))
        case("past the last line has none", complete("ra", 3, line=9) == [])
        case("a column past the end is clamped", bool(complete("ra", 99)))
        for args in ({}, {"text": "a"}, {"column": 1}, {"text": 1, "column": 1}):
            r = d.req("completions", args)
            case(f"completions {args} is refused", bool(r) and not r["success"], str(r)[:120])

    with session({"linesStartAt1": False, "columnsStartAt1": False}, {}) as e:
        d = e.d
        d.pause()
        r = d.req("completions", {"text": "x\nram", "column": 3, "line": 1})
        t = r["body"]["targets"] if r and r["success"] else []
        case("zero-based column and line", [i["label"] for i in t] == ["rambank"], str(r))
        case("zero-based start", bool(t) and t[0]["start"] == 2 and t[0]["length"] == 3, str(t))


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


def output_lines(d, start):
    return [m["body"]["output"] for m in d.msgs[start:]
            if m.get("type") == "event" and m.get("event") == "output"]


def wait_output(d, start, want, secs=3):
    end = time.time() + secs
    while time.time() < end:
        lines = output_lines(d, start)
        if any(want in l for l in lines):
            return lines
        d.pump(0.05)
    return output_lines(d, start)


def test_logpoints():
    print("logpoints")
    with tempfile.TemporaryDirectory() as t:
        src, dbg = write_debug_info(t)
        with Emu(kind_args={"dbgFile": dbg}) as e:
            d = e.d
            init = next(m for m in d.msgs if m.get("command") == "initialize")
            case("logpoints are advertised", init["body"].get("supportsLogPoints") is True)
            plant(d, a="$2A")
            source = {"path": src}

            r = d.req("setBreakpoints", {"source": source, "breakpoints": [{"line": 1, "logMessage": "{"}]})
            case("a malformed message is refused", refused(r), str(r and r.get("body")))
            case("and nothing is armed", nothing_armed(d))

            start = len(d.msgs)
            r = d.req("setBreakpoints", {"source": source, "breakpoints": [
                {"line": 1, "logMessage": "A={A} {{ok}} {nosuchsymbol}"}]})
            case("a logpoint is verified", r["body"]["breakpoints"][0].get("verified") is True, str(r))
            listing = d.breakpoints()["breakpoints"]
            case("the listing shows its message",
                 listing and listing[0].get("logs", [{}])[0].get("message") == "A={A} {{ok}} {nosuchsymbol}",
                 str(listing))
            ev = go(d, 0.5)
            case("it does not stop the machine", ev is None, str(ev))
            lines = wait_output(d, start, "A=$2A")
            hit = [l for l in lines if "A=$2A" in l]
            case("it prints through output events", bool(hit), str(lines[:3]))
            case("with escapes resolved", bool(hit) and "{ok}" in hit[0], str(hit[:1]))
            case("a bad expression shows in the line",
                 bool(hit) and "{nosuchsymbol:" in hit[0], str(hit[:1]))
            case("each line ends in a newline", bool(hit) and hit[0].endswith("\n"), str(hit[:1]))
            d.pause()

            # A plain breakpoint in the same request stops; the logpoint beside it does not.
            r = d.req("setBreakpoints", {"source": source, "breakpoints": [
                {"line": 1, "logMessage": "log"}, {"line": 2}]})
            set_reg(d, "PC", "$0800")
            ev = go(d)
            plain = r["body"]["breakpoints"][1]["id"]
            case("the plain breakpoint beside it stops", bool(ev) and ev["body"].get("hitBreakpointIds") == [plain], str(ev))

            # Another owner's plain breakpoint at the logpoint's address stops it.
            d.req("setBreakpoints", {"source": source, "breakpoints": [{"line": 1, "logMessage": "log"}]})
            d.req("setInstructionBreakpoints", {"breakpoints": [{"instructionReference": "0x0800"}]})
            set_reg(d, "PC", "$0806")
            ev = go(d)
            case("another owner's plain breakpoint still stops",
                 bool(ev) and ev["body"].get("reason") in ("breakpoint", "instruction breakpoint"), str(ev))
            case("a stop names only what asked to stop",
                 bool(ev) and all(i != r["body"]["breakpoints"][0]["id"]
                                  for i in ev["body"].get("hitBreakpointIds", [])), str(ev))
            d.req("setInstructionBreakpoints", {"breakpoints": []})

            # A logMessage dropped by the next request is a plain breakpoint again.
            r = d.req("setBreakpoints", {"source": source, "breakpoints": [{"line": 1}]})
            set_reg(d, "PC", "$0806")
            ev = go(d)
            case("without logMessage it stops again", bool(ev) and ev["body"]["reason"] == "breakpoint", str(ev))
            d.req("setBreakpoints", {"source": source, "breakpoints": []})
            case("clearing leaves nothing armed", nothing_armed(d))

            # A client that stops reading is neither stalled on nor dropped.
            d.req("setBreakpoints", {"source": source, "breakpoints": [
                {"line": 1, "logMessage": "x" * 200 + "{A}"}]})
            set_reg(d, "PC", "$0800")
            start = len(d.msgs)
            d.req("continue", {"threadId": 1})
            time.sleep(1.5)
            s = d.status()
            case("a client that stopped reading is still connected", bool(s), str(s)[:120])
            lines = wait_output(d, start, "lines lost")
            case("and hears how many lines it lost", any("lines lost" in l for l in lines),
                 str([l[:40] for l in lines[:2]]))
            d.pause()
            d.req("setBreakpoints", {"source": source, "breakpoints": []})


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


FLAG = 0x1000
CBINV = 0x0316      # KERNAL's BRK vector in RAM


def wait_booted(d):
    end = time.time() + 20
    while time.time() < end and d.status().get("frame", 0) < 200:
        time.sleep(0.05)


def plant_brk(d):
    # BRK at $0810. The KERNAL's BRK path lands in a loop at $0820 that sets a flag.
    d.pause()
    put_mem(d, CBINV, [0x20, 0x08])
    put_mem(d, 0x0810, [0x00, 0x00])
    put_mem(d, 0x0820, [0xA9, 0x01, 0x8D, 0x00, 0x10, 0x4C, 0x20, 0x08])
    put_mem(d, FLAG, [0])
    set_reg(d, "P (Status)", "$24")
    set_reg(d, "PC", "$0810")


def plant_stp(d):
    # STP at $0810, then a loop that sets a flag.
    d.pause()
    put_mem(d, 0x0810, [0xDB, 0xA9, 0x01, 0x8D, 0x00, 0x10, 0x4C, 0x11, 0x08])
    put_mem(d, FLAG, [0])
    set_reg(d, "P (Status)", "$24")
    set_reg(d, "PC", "$0810")


def runs_on(d):
    start = len(d.msgs)
    d.req("continue", {"threadId": 1})
    stopped = d.wait_event("stopped", start, 0.6)
    running = not d.status().get("paused")
    d.pause()
    return stopped is None and running and get_byte(d, FLAG) == 1


def test_exception_stops():
    print("exception stops")
    with Emu(kind=None, handshake=False) as e:
        d = e.d
        caps = d.req("initialize", {"adapterID": "conformance", "linesStartAt1": True,
                                    "columnsStartAt1": True, "pathFormat": "path"})["body"]
        filters = caps.get("exceptionBreakpointFilters", [])
        case("four filters", [f.get("filter") for f in filters] == ["irq", "nmi", "brk", "stp"], str(filters))
        case("all off by default", all(f.get("default") is False for f in filters), str(filters))
        case("exceptionInfo advertised", caps.get("supportsExceptionInfoRequest") is True)
        d.req("attach", {})
        d.req("configurationDone")
        wait_booted(d)

        r = d.req("exceptionInfo", {"threadId": 1})
        case("exceptionInfo needs an exception stop", r and r.get("success") is False, str(r))

        r = d.req("setExceptionBreakpoints", {"filters": ["brk", "nope", 5]})
        b = (r or {}).get("body", {}).get("breakpoints", [])
        case("one result per filter", len(b) == 3, str(b))
        case("a known filter is verified", len(b) == 3 and b[0].get("verified") is True, str(b))
        case("an unknown name is not", len(b) == 3 and b[1].get("verified") is False and b[1].get("message"), str(b))
        case("nor a non-string", len(b) == 3 and b[2].get("verified") is False, str(b))
        r = d.req("setExceptionBreakpoints", {"filters": "brk"})
        case("filters must be an array", r and r.get("success") is False, str(r))

        plant_brk(d)
        ev = go(d)
        body = (ev or {}).get("body", {})
        case("brk: reason", body.get("reason") == "exception", str(ev))
        case("brk: text names it", "BRK at $0810" in body.get("text", ""), str(body))
        case("brk: event carries the stop", body.get("stop", {}).get("kind") == "brk", str(body))
        info = (d.req("exceptionInfo", {"threadId": 1}) or {}).get("body", {})
        case("brk: exceptionId", info.get("exceptionId") == "brk", str(info))
        case("brk: breakMode", info.get("breakMode") == "always", str(info))
        case("brk: vector", info.get("vector") == 0xFFFE, str(info))
        case("brk: address of the opcode", info.get("address") == 0x0810 and info.get("programBank") == 0, str(info))
        case("brk: in the description", "$FFFE" in info.get("description", ""), str(info))
        case("brk: stopped in the handler", not str(d.var("PC")).startswith("$0810"), str(d.var("PC")))
        case("brk: resuming runs the handler, no second stop", runs_on(d))

        d.req("setExceptionBreakpoints", {"filters": []})
        plant_stp(d)
        ev = go(d)
        body = (ev or {}).get("body", {})
        case("stp unarmed: still stops", bool(ev), str(ev))
        case("stp unarmed: not an exception", body.get("reason") != "exception", str(body))
        r = d.req("exceptionInfo", {"threadId": 1})
        case("stp unarmed: no exceptionInfo", r and r.get("success") is False, str(r))
        case("stp unarmed: PC past the opcode", str(d.var("PC")).startswith("$0811"), str(d.var("PC")))
        case("stp unarmed: resumes", runs_on(d))

        d.req("setExceptionBreakpoints", {"filters": ["stp"]})
        plant_stp(d)
        ev = go(d)
        body = (ev or {}).get("body", {})
        case("stp: reason", body.get("reason") == "exception", str(ev))
        info = (d.req("exceptionInfo", {"threadId": 1}) or {}).get("body", {})
        case("stp: exceptionId", info.get("exceptionId") == "stp", str(info))
        case("stp: no vector", "vector" in info and info["vector"] is None, str(info))
        case("stp: address", info.get("address") == 0x0810, str(info))
        case("stp: PC past the opcode", str(d.var("PC")).startswith("$0811"), str(d.var("PC")))
        case("stp: resuming runs on, no second stop", runs_on(d))

        # ldx #$42 / ldy #3 / lda #0 / jsr i2c_write_byte / jmp *: the SMC raises an NMI.
        d.req("setExceptionBreakpoints", {"filters": ["nmi"]})
        d.pause()
        put_mem(d, 0x0810, [0xA2, 0x42, 0xA0, 0x03, 0xA9, 0x00, 0x20, 0xC9, 0xFE, 0x4C, 0x19, 0x08])
        set_reg(d, "P (Status)", "$24")
        set_reg(d, "PC", "$0810")
        ev = go(d, 5)
        body = (ev or {}).get("body", {})
        case("nmi: reason", body.get("reason") == "exception", str(ev))
        info = (d.req("exceptionInfo", {"threadId": 1}) or {}).get("body", {})
        case("nmi: exceptionId", info.get("exceptionId") == "nmi", str(info))
        case("nmi: vector", info.get("vector") == 0xFFFA, str(info))

        d.req("setExceptionBreakpoints", {"filters": ["irq"]})
        set_reg(d, "P (Status)", "$20")
        set_reg(d, "PC", "$0820")
        ev = go(d, 10)
        body = (ev or {}).get("body", {})
        case("irq: reason", body.get("reason") == "exception", str(ev))
        info = (d.req("exceptionInfo", {"threadId": 1}) or {}).get("body", {})
        case("irq: exceptionId", info.get("exceptionId") == "irq", str(info))
        case("irq: vector", info.get("vector") == 0xFFFE, str(info))
        s = d.status().get("stop", {})
        case("irq: in x16/status", s.get("kind") == "irq" and s.get("reason") == "exception", str(s))
        d.req("restart")
        r = d.req("exceptionInfo", {"threadId": 1})
        case("a reset ends the exception", r and r.get("success") is False, str(r))
        d.req("setExceptionBreakpoints", {"filters": []})
        d.req("continue", {"threadId": 1})

        d.req("setExceptionBreakpoints", {"filters": ["brk"]})
        d.close()
        time.sleep(0.3)
        d2 = Dap(e.port)
        e.d = d2
        d2.req("initialize", {"adapterID": "conformance", "linesStartAt1": True,
                              "columnsStartAt1": True, "pathFormat": "path"})
        d2.req("attach", {})
        d2.req("configurationDone")
        wait_booted(d2)
        plant_brk(d2)
        case("a session's filters end with it", runs_on(d2))

        d2.req("setExceptionBreakpoints", {"filters": ["brk"]})
        plant_brk(d2)
        case("brk stops again", (go(d2) or {}).get("body", {}).get("reason") == "exception")
        d2.close()
        time.sleep(0.3)
        d3 = Dap(e.port)
        e.d = d3
        d3.req("initialize", {"adapterID": "conformance", "linesStartAt1": True,
                              "columnsStartAt1": True, "pathFormat": "path"})
        start = len(d3.msgs)
        d3.req("launch", {"stopOnEntry": True})
        d3.req("configurationDone")
        ev = d3.wait_event("stopped", start)
        body = (ev or {}).get("body", {})
        case("a later stop is not the old exception", body.get("reason") == "entry" and "stop" not in body, str(ev))
        r = d3.req("exceptionInfo", {"threadId": 1})
        case("and has no exceptionInfo", r and r.get("success") is False, str(r))

    with session({}, {"noDebug": True}) as e:
        d = e.d
        r = d.req("setExceptionBreakpoints", {"filters": ["brk"]})
        b = (r or {}).get("body", {}).get("breakpoints", [])
        case("noDebug arms no filter", len(b) == 1 and b[0].get("verified") is False, str(b))
        wait_booted(d)
        plant_brk(d)
        case("and a BRK runs through", runs_on(d))


def test_saved_interrupt_setting():
    print("saved break-on-interrupt setting")
    ini = "[X16Debugger][Prefs]\nBreakOnInterrupt=1\n"
    with Emu(extra=["-imgui"], files={"imgui.ini": ini}) as e:
        d = e.d
        d.drain(0.5)   # configurationDone can report a stop after its response
        ev = go(d, 12)
        if not ev:
            print("  SKIP no graphical debugger in this environment")
            return
        case("the saved setting stops the machine", ev["body"]["reason"] == "interrupt", str(ev))
        case("empty filters are acknowledged",
             (d.req("setExceptionBreakpoints", {"filters": []}) or {}).get("success") is True)
        ev = go(d, 12)
        case("empty filters leave the saved setting in force",
             bool(ev) and ev["body"]["reason"] == "interrupt", str(ev))
        d.req("setExceptionBreakpoints", {"filters": ["irq"]})
        ev = go(d, 12)
        case("a filter reports as an exception", bool(ev) and ev["body"]["reason"] == "exception", str(ev))
        d.req("setExceptionBreakpoints", {"filters": []})
        ev = go(d, 12)
        case("clearing it returns to the saved setting",
             bool(ev) and ev["body"]["reason"] == "interrupt", str(ev))


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
                          ("an unreadable hitCondition", {"dataId": "4096", "hitCondition": "% 2"}),
                          ("a zero hitCondition", {"dataId": "4096", "hitCondition": "0"}),
                          ("a numeric hitCondition", {"dataId": "4096", "hitCondition": 3}),
                          ("a range past 16 bits", {"dataId": "65535:2"}),
                          ("a length that wraps", {"dataId": "4096:4294963200"}),
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
        d.req("evaluate", {"expression": "watch_remove 1000"})
        d.req("setDataBreakpoints", {"breakpoints": []})

        plant(d, pc="$0810")
        r = data(dataId="4096", hitCondition="3")
        b = r["body"]["breakpoints"][0]
        wps = d.breakpoints().get("watchpoints", [])
        case("hitCondition: verified", b.get("verified") is True, str(b))
        case("hitCondition: listed", len(wps) == 1 and wps[0].get("ignoreCount") == 2, str(wps))
        ev = go(d)
        case("hitCondition: stops on the third write", bool(ev) and get_byte(d, 0x1000) == 3,
             str(get_byte(d, 0x1000)))
        case("hitCondition: the stop names it", bool(ev) and ev["body"].get("hitBreakpointIds") == [b.get("id")], str(ev))
        ev = go(d)
        case("hitCondition: every later hit stops", bool(ev) and get_byte(d, 0x1000) == 4,
             str(get_byte(d, 0x1000)))

        d.req("setDataBreakpoints", {"breakpoints": []})
        plant(d, pc="$0810")
        data(dataId="4096", hitCondition=">= 2")
        ev = go(d)
        case("hitCondition: >= N", bool(ev) and get_byte(d, 0x1000) == 2, str(get_byte(d, 0x1000)))

        d.req("setDataBreakpoints", {"breakpoints": []})
        plant(d, pc="$0810")
        data(dataId="4096", hitCondition="3")
        go(d)
        data(dataId="4096", hitCondition="3")
        wps = d.breakpoints().get("watchpoints", [])
        case("hitCondition: an unchanged re-send keeps the count", len(wps) == 1 and wps[0]["hits"] == 3, str(wps))
        ev = go(d)
        case("hitCondition: so the next hit stops", bool(ev) and get_byte(d, 0x1000) == 4,
             str(get_byte(d, 0x1000)))
        data(dataId="4096", hitCondition="3", condition="== 9")
        wps = d.breakpoints().get("watchpoints", [])
        case("hitCondition: a changed filter restarts it", len(wps) == 1 and wps[0]["hits"] == 0, str(wps))
        data(dataId="4096", hitCondition="3")
        d.req("setDataBreakpoints", {"breakpoints": []})
        data(dataId="4096", hitCondition="3")
        wps = d.breakpoints().get("watchpoints", [])
        case("hitCondition: removing it restarts it", len(wps) == 1 and wps[0]["hits"] == 0, str(wps))

        d.req("setDataBreakpoints", {"breakpoints": []})
        plant(d, pc="$0810")
        data(dataId="4096", hitCondition="2", condition="== 5")
        ev = go(d, 5)
        wps = d.breakpoints().get("watchpoints", [])
        case("hitCondition: counts only what the condition passes",
             bool(ev) and get_byte(d, 0x1000) == 5 and len(wps) == 1 and wps[0]["hits"] == 2, str(wps))

        d.req("setDataBreakpoints", {"breakpoints": []})
        plant(d, pc="$0810")
        r = d.req("setDataBreakpoints", {"breakpoints": [{"dataId": "4096", "hitCondition": "2"},
                                                         {"dataId": "4095:2"}]})
        counted, plain = [x.get("id") for x in r["body"]["breakpoints"]]
        ev = go(d)
        case("hitCondition: an overlapping watch inside its count is not named",
             bool(ev) and ev["body"].get("hitBreakpointIds") == [plain], str(ev))
        ev = go(d)
        case("hitCondition: named once past it",
             bool(ev) and sorted(ev["body"].get("hitBreakpointIds", [])) == sorted([counted, plain]), str(ev))

        d.req("setDataBreakpoints", {"breakpoints": []})
        d.req("evaluate", {"expression": "watch_add 1000"})
        before = d.breakpoints()
        r = data(dataId="4096", hitCondition="2")
        case("hitCondition: refused on another owner's watch", refused(r), str(r and r.get("body")))
        case("hitCondition: that watch is unchanged", d.breakpoints() == before)
        d.req("evaluate", {"expression": "watch_remove 1000"})
        data(dataId="4096", hitCondition="2")
        d.req("evaluate", {"expression": "watch_add 1000"})
        wps = d.breakpoints().get("watchpoints", [])
        case("hitCondition: another owner cannot join a counted watch",
             len(wps) == 1 and wps[0]["owners"] == ["dapSource"], str(wps))
        d.req("setDataBreakpoints", {"breakpoints": []})


def test_data_breakpoint_bytes():
    print("data breakpoints by address")
    with Emu(kind=None, handshake=False) as e:
        d = e.d
        init = d.req("initialize", {"adapterID": "conformance", "linesStartAt1": True,
                                    "columnsStartAt1": True, "pathFormat": "path"})
        caps = (init or {}).get("body") or {}
        case("the capability is advertised", caps.get("supportsDataBreakpointBytes") is True, str(caps)[:200])
        d.req("attach", {})
        d.req("configurationDone")
        plant(d, pc="$0810")

        def info(name, **args):
            r = d.req("dataBreakpointInfo", dict(args, name=name))
            return r["body"].get("dataId") if r and r["success"] else "failed"

        for label, name, args, want in (
                ("hex", "0x1000", {"asAddress": True}, "4096"),
                ("decimal", "4096", {"asAddress": True}, "4096"),
                ("a leading zero is still decimal", "0100", {"asAddress": True}, "100"),
                ("a range", "0x1000", {"asAddress": True, "bytes": 4}, "4096:4"),
                ("one byte", "0x1000", {"asAddress": True, "bytes": 1}, "4096"),
                ("up to the last byte", "0xFFFF", {"asAddress": True, "bytes": 1}, "65535"),
                ("all of it", "0", {"asAddress": True, "bytes": 65536}, "0:65536"),
                ("across $A000", "0x9FFF", {"asAddress": True, "bytes": 2}, "40959:2"),
                ("across $C000", "0xBFFF", {"asAddress": True, "bytes": 2}, "49151:2"),
                ("VRAM", "vram:0x1B000", {"asAddress": True, "bytes": 16}, "vram:1B000:16"),
                ("the last of VRAM", "vram:1FFFF", {"asAddress": True, "bytes": 1}, "vram:1FFFF"),
                ("bytes on an expression", "$1000", {"bytes": 3}, "4096:3"),
                ("bytes on a variable", "R0", {"variablesReference": 4, "bytes": 4}, "2:4")):
            got = info(name, **args)
            case(f"bytes: {label}", got == want, str(got))
            r = d.req("setDataBreakpoints", {"breakpoints": [{"dataId": got}]})
            case(f"bytes: {label} arms", bool(r) and r["body"]["breakpoints"][0].get("verified") is True, str(r))
            d.req("setDataBreakpoints", {"breakpoints": []})

        for label, name, args in (
                ("past the end", "0xFFFF", {"asAddress": True, "bytes": 2}),
                ("past 16 bits", "0x10000", {"asAddress": True}),
                ("a 24-bit address", "0x01C000", {"asAddress": True, "bytes": 1}),
                ("a name", "banana", {"asAddress": True}),
                ("a register name", "R0", {"asAddress": True}),
                ("no digits", "0x", {"asAddress": True}),
                ("zero bytes", "0x1000", {"asAddress": True, "bytes": 0}),
                ("negative bytes", "0x1000", {"asAddress": True, "bytes": -1}),
                ("fractional bytes", "0x1000", {"asAddress": True, "bytes": 1.5}),
                ("bytes as text", "0x1000", {"asAddress": True, "bytes": "4"}),
                ("too many bytes", "0", {"asAddress": True, "bytes": 0x20001}),
                ("past the end of VRAM", "vram:1FFFF", {"asAddress": True, "bytes": 2}),
                ("past the end, by expression", "$FFFF", {"bytes": 2})):
            case(f"no dataId: {label}", info(name, **args) is None, str(info(name, **args)))

        r = d.req("dataBreakpointInfo", {"name": "0x9FFF", "asAddress": True, "bytes": 2})
        case("a range into a banked window says so", "any bank" in r["body"].get("description", ""), str(r["body"]))
        case("it offers every access type",
             set(r["body"].get("accessTypes", [])) == {"read", "write", "readWrite"}, str(r["body"]))

        # INC $9FFF, INC $A000, JMP back.
        put_mem(d, 0x0820, [0xEE, 0xFF, 0x9F, 0xEE, 0x00, 0xA0, 0x4C, 0x20, 0x08])
        set_reg(d, "PC", "$0820")
        r = d.req("setDataBreakpoints", {"breakpoints": [{"dataId": "40959:2", "accessType": "write"}]})
        ident = r["body"]["breakpoints"][0].get("id")
        ev = go(d)
        case("a range across $A000 fires below it",
             bool(ev) and ev["body"].get("hitBreakpointIds") == [ident] and
             d.req("x16/registers")["body"]["cpu"]["pc"] == 0x0823, str(ev))
        set_reg(d, "RAM Bank", "$05")
        ev = go(d)
        case("and above it, in whichever bank is mapped",
             bool(ev) and ev["body"].get("hitBreakpointIds") == [ident] and
             d.req("x16/registers")["body"]["cpu"]["pc"] == 0x0826, str(ev))
        d.req("setDataBreakpoints", {"breakpoints": []})


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

    for test in (test_evaluate_contexts, test_expressions, test_disconnect, test_terminate, test_dropped_socket,
                 test_events_wait_for_initialize, test_pause_order, test_goto, test_set_variable,
                 test_source_breakpoints, test_logpoints, test_function_breakpoints, test_instruction_stop,
                 test_exception_stops, test_saved_interrupt_setting,
                 test_data_breakpoints, test_data_breakpoint_bytes, test_stack_frames, test_line_base, test_uri_paths,
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
