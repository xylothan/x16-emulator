#!/usr/bin/env python3
"""Checks the MCP server, over the same pipes a real client uses.

    python testbench/test_mcp.py <x16emu> <rom.bin>

Each case launches a private emulator with -mcp. Every line it writes to stdout
must be one JSON-RPC message: anything else would break a real client.
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

    def rpc(self, method, params=None, timeout=15):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.write(json.dumps(msg))
        end = time.time() + timeout
        while time.time() < end:
            r = self.recv(max(0.05, end - time.time()))
            if r is None:
                return None
            if r.get("id") == rid:
                return r
        return None

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
        case("the read-only tools are listed",
             {"get_status", "get_registers", "read_memory", "disassemble", "list_breakpoints"} <= names, str(names))
        for t in listed:
            s = t.get("inputSchema", {})
            case(f"{t['name']} has an object schema", s.get("type") == "object", str(s))
            case(f"{t['name']} has a description", bool(t.get("description")))
            case(f"{t['name']} says it is read-only", t.get("annotations", {}).get("readOnlyHint") is True)

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


def main():
    global EXE, ROM
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    EXE, ROM = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    for test in (test_protocol, test_tools, test_lifetime, test_with_dap):
        try:
            test()
        except Exception as ex:   # one broken case must not hide the others
            case(f"{test.__name__} ran to completion", False, f"{type(ex).__name__}: {ex}")
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
