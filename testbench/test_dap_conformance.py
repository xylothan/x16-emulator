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

    def __init__(self, kind="attach", extra=(), handshake=True, warp=True):
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
                self.d.req(kind, {})
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
                 test_events_wait_for_initialize, test_pause_order, test_goto, test_set_variable):
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
