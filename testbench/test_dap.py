import socket, json, time, sys, base64

def send_dap(sock, msg):
    body = json.dumps(msg)
    header = "Content-Length: %d\r\n\r\n" % len(body)
    sock.sendall((header + body).encode())

def recv_dap(sock, timeout=3):
    sock.settimeout(timeout)
    data = b''
    results = []
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
            while b'\r\n\r\n' in data:
                header_end = data.index(b'\r\n\r\n')
                header = data[:header_end].decode()
                for line in header.split('\r\n'):
                    if line.startswith('Content-Length:'):
                        cl = int(line.split(':')[1].strip())
                        break
                body_start = header_end + 4
                if len(data) >= body_start + cl:
                    body = data[body_start:body_start + cl].decode()
                    data = data[body_start + cl:]
                    results.append(json.loads(body))
                else:
                    break
    except socket.timeout:
        pass
    return results

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    sock.connect(('127.0.0.1', 9009))
except ConnectionRefusedError:
    print("ERROR: Cannot connect to DAP server on port 9009. Is emulator running with -debugport?")
    sys.exit(1)
print("Connected to DAP server on port 9009")

passed = 0
failed = 0

def check(name, msgs, expect_type=None, expect_cmd=None, expect_success=True):
    global passed, failed
    if not msgs:
        print("  FAIL %s: no response" % name)
        failed += 1
        return None
    for m in msgs:
        mtype = m.get("type", "")
        mcmd = m.get("command", m.get("event", ""))
        ok = True
        if expect_type and mtype != expect_type:
            ok = False
        if expect_cmd and mcmd != expect_cmd:
            ok = False
        if expect_type == "response" and expect_success is not None:
            if m.get("success") != expect_success:
                ok = False
        if ok:
            print("  PASS %s" % name)
            passed += 1
            return m
    print("  FAIL %s: unexpected response: %s" % (name, json.dumps(msgs[0])[:100]))
    failed += 1
    return msgs[0]

# 1. Initialize
send_dap(sock, {"seq": 1, "type": "request", "command": "initialize", "arguments": {"adapterID": "test"}})
time.sleep(0.5)
msgs = recv_dap(sock, 1)
m = check("initialize response", [m for m in msgs if m.get("type") == "response"], "response", "initialize")
if m and m.get("body", {}).get("supportsConfigurationDoneRequest"):
    print("       Capabilities OK")
check("initialized event", [m for m in msgs if m.get("event") == "initialized"], "event", "initialized")

# 2. Launch
send_dap(sock, {"seq": 2, "type": "request", "command": "launch", "arguments": {"stopOnEntry": True}})
time.sleep(0.5)
msgs = recv_dap(sock, 1)
check("launch response", [m for m in msgs if m.get("type") == "response"], "response", "launch")
check("thread event", [m for m in msgs if m.get("event") == "thread"], "event", "thread")

# 3. ConfigurationDone
send_dap(sock, {"seq": 3, "type": "request", "command": "configurationDone", "arguments": {}})
time.sleep(0.5)
msgs = recv_dap(sock, 1)
check("configDone response", [m for m in msgs if m.get("type") == "response"], "response", "configurationDone")
check("stopped event (entry)", [m for m in msgs if m.get("event") == "stopped"], "event", "stopped")

# 4. Threads
send_dap(sock, {"seq": 4, "type": "request", "command": "threads"})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
m = check("threads response", msgs, "response", "threads")
if m:
    threads = m.get("body", {}).get("threads", [])
    if len(threads) == 1 and threads[0].get("name") == "6502 CPU":
        print("       Thread: %s (id=%d)" % (threads[0]["name"], threads[0]["id"]))

# 5. StackTrace
send_dap(sock, {"seq": 5, "type": "request", "command": "stackTrace", "arguments": {"threadId": 1}})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
m = check("stackTrace response", msgs, "response", "stackTrace")
if m:
    frames = m.get("body", {}).get("stackFrames", [])
    if frames:
        print("       Frame: %s" % frames[0].get("name", "?"))

# 6. Scopes
send_dap(sock, {"seq": 6, "type": "request", "command": "scopes", "arguments": {"frameId": 1}})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
m = check("scopes response", msgs, "response", "scopes")
if m:
    scopes = m.get("body", {}).get("scopes", [])
    print("       Scopes: %s" % ", ".join(s["name"] for s in scopes))

# 7. Variables (registers)
send_dap(sock, {"seq": 7, "type": "request", "command": "variables", "arguments": {"variablesReference": 1}})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
m = check("variables (registers)", msgs, "response", "variables")
if m:
    vlist = m.get("body", {}).get("variables", [])
    for v in vlist[:3]:
        print("       %s = %s" % (v["name"], v["value"]))

# 8. StepIn
send_dap(sock, {"seq": 8, "type": "request", "command": "stepIn", "arguments": {"threadId": 1}})
time.sleep(0.5)
msgs = recv_dap(sock, 1)
check("stepIn response", [m for m in msgs if m.get("type") == "response"], "response", "stepIn")
check("stopped event (step)", [m for m in msgs if m.get("event") == "stopped"], "event", "stopped")

# 9. Evaluate
send_dap(sock, {"seq": 9, "type": "request", "command": "evaluate", "arguments": {"expression": "PC"}})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
m = check("evaluate (PC)", msgs, "response", "evaluate")
if m:
    print("       PC = %s" % m.get("body", {}).get("result", "?"))

# 10. Evaluate memory
send_dap(sock, {"seq": 10, "type": "request", "command": "evaluate", "arguments": {"expression": "$0000"}})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
m = check("evaluate ($0000)", msgs, "response", "evaluate")
if m:
    print("       [$0000] = %s" % m.get("body", {}).get("result", "?"))

# 11. setExceptionBreakpoints (VS always sends this)
send_dap(sock, {"seq": 11, "type": "request", "command": "setExceptionBreakpoints", "arguments": {"filters": []}})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
check("setExceptionBreakpoints", msgs, "response", "setExceptionBreakpoints")

# 12. Pipelined requests in one TCP segment.
#
# A client sends its configuration burst without waiting for each response, so
# several framed messages routinely arrive in a single read. The framing used to
# NUL-terminate each body in place, which overwrote the first byte of the next
# message; the connection then stopped dispatching entirely. Nothing above can
# catch that, because every request here is sent on its own with a sleep after
# it.
body_a = json.dumps({"seq": 90, "type": "request", "command": "threads"})
body_b = json.dumps({"seq": 91, "type": "request", "command": "evaluate",
                     "arguments": {"expression": "PC"}})
blob = ("Content-Length: %d\r\n\r\n%s" % (len(body_a), body_a) +
        "Content-Length: %d\r\n\r\n%s" % (len(body_b), body_b))
sock.sendall(blob.encode())
time.sleep(0.5)
msgs = recv_dap(sock, 2)
seqs = sorted(m.get("request_seq") for m in msgs if m.get("type") == "response")
if seqs == [90, 91]:
    print("  PASS both pipelined requests answered")
    passed += 1
else:
    print("  FAIL both pipelined requests answered: got %r" % (seqs,))
    failed += 1

# A third request after the burst proves the buffer is still usable.
send_dap(sock, {"seq": 92, "type": "request", "command": "threads"})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
check("connection still live after pipelining", msgs, "response", "threads")

# 13. x16/joystick: hold, read back, release.
#
# The only way a script can drive a game that reads the SNES ports. The state is
# level-based rather than edge-based -- the button list is the complete held set
# -- so what matters is that a press survives being read, that a button left off
# the list is released, and that the port hands itself back when the session is
# done with it. None of that is visible without a running emulator, because the
# port is only enabled and only latched by the machine.

def joystick(seq, args, pause=0.3):
    send_dap(sock, {"seq": seq, "type": "request", "command": "x16/joystick",
                    "arguments": args})
    time.sleep(pause)
    for m in recv_dap(sock, 1):
        if m.get("type") == "response" and m.get("command") == "x16/joystick":
            return m
    return None


def check_joystick(name, resp, want):
    global passed, failed
    if resp is None or not resp.get("success"):
        print("  FAIL %s: no successful response" % name)
        failed += 1
        return None
    body = resp.get("body", {})
    for key, expected in want.items():
        got = body.get(key)
        if key == "buttons":
            got = sorted(got or [])
            expected = sorted(expected)
        if got != expected:
            print("  FAIL %s: %s was %r, expected %r" % (name, key, got, expected))
            failed += 1
            return body
    print("  PASS %s" % name)
    passed += 1
    return body


# Active low, so a held button is a cleared bit: A is bit 0, and the latch forces
# bits 12-15 high.
check_joystick("x16/joystick holds a button",
               joystick(93, {"index": 0, "buttons": ["a"]}),
               {"index": 0, "mask": 0xFFFE, "buttons": ["a"], "enabled": True,
                "slotEnabled": True})

# Driving a port must not need -joy1..-joy4, or headless automation could not
# use it at all.
check_joystick("x16/joystick enables the port it drives",
               joystick(94, {"index": 2, "buttons": ["left", "b"]}),
               {"index": 2, "slotEnabled": True, "enabled": True,
                "buttons": ["left", "b"]})

# No buttons and no mask is a query: it must report the held state without
# disturbing it, which is what lets a test assert what it just pressed.
check_joystick("x16/joystick queries without changing state",
               joystick(95, {"index": 0}),
               {"index": 0, "mask": 0xFFFE, "buttons": ["a"], "enabled": True})

# The list is the whole held set, so a button left off it is released.
check_joystick("x16/joystick releases buttons left off the list",
               joystick(96, {"index": 0, "buttons": ["start"]}),
               {"index": 0, "buttons": ["start"], "enabled": True})

# A raw mask replays a captured state verbatim.
check_joystick("x16/joystick accepts a raw mask",
               joystick(97, {"index": 1, "mask": 0xFFFF & ~(1 << 4)}),
               {"index": 1, "buttons": ["up"], "enabled": True})

check_joystick("x16/joystick releases the port",
               joystick(98, {"index": 0, "enabled": False}),
               {"index": 0, "mask": 0xFFFF, "buttons": [], "enabled": False})

# enabled:true on its own connects a controller with nothing held. That is not
# the same as an empty port -- a driven port runs its shift register out, so the
# KERNAL sees a controller -- and it is what a game waiting for one needs.
check_joystick("x16/joystick connects a controller with nothing held",
               joystick(99, {"index": 0, "enabled": True}),
               {"index": 0, "mask": 0xFFFF, "buttons": [], "enabled": True,
                "slotEnabled": True})

# Leave the machine as it was found, so anything running after this is not
# holding a direction it never asked for.
for _slot in (0, 1, 2):
    joystick(100 + _slot, {"index": _slot, "enabled": False})

m = joystick(102, {"index": 9, "buttons": ["a"]})
if m is not None and not m.get("success"):
    print("  PASS x16/joystick rejects a port that does not exist")
    passed += 1
else:
    print("  FAIL x16/joystick rejects a port that does not exist")
    failed += 1

m = joystick(103, {"index": 0, "buttons": ["turbo"]})
if m is not None and not m.get("success"):
    print("  PASS x16/joystick rejects an unknown button")
    passed += 1
else:
    print("  FAIL x16/joystick rejects an unknown button")
    failed += 1

# 14. readMemory/writeMemory against VRAM.
#
# VRAM is not in the CPU map -- the CPU reaches it only through VERA's data
# port, one auto-incrementing byte at a time -- so a "vram:" reference is the
# only way a DAP client can read or write it. Nothing here is checkable from the
# unit tests: it needs a real VERA with a real framebuffer behind it.

def read_mem(seq, ref, count):
    send_dap(sock, {"seq": seq, "type": "request", "command": "readMemory",
                    "arguments": {"memoryReference": ref, "count": count}})
    time.sleep(0.3)
    for m in recv_dap(sock, 1):
        if m.get("type") == "response" and m.get("command") == "readMemory":
            return m
    return None


def write_mem(seq, ref, data):
    send_dap(sock, {"seq": seq, "type": "request", "command": "writeMemory",
                    "arguments": {"memoryReference": ref,
                                  "data": base64.b64encode(bytes(data)).decode()}})
    time.sleep(0.3)
    for m in recv_dap(sock, 1):
        if m.get("type") == "response" and m.get("command") == "writeMemory":
            return m
    return None


def mem_bytes(resp):
    if not resp or not resp.get("success"):
        return None
    return base64.b64decode(resp.get("body", {}).get("data", ""))


def expect(name, cond):
    global passed, failed
    if cond:
        print("  PASS %s" % name)
        passed += 1
    else:
        print("  FAIL %s" % name)
        failed += 1


# The same number means different memory in each space, which is the whole
# reason the prefix has to exist.
cpu = mem_bytes(read_mem(110, "0x1B000", 16))
vram = mem_bytes(read_mem(111, "vram:1B000", 16))
expect("readMemory reads VRAM", vram is not None and len(vram) == 16)
expect("VRAM and the same CPU address are different memory", cpu is not None and vram != cpu)

# The echoed address has to come back in the space it was read from, or paging
# through VRAM by feeding it back would silently land in the CPU map.
r = read_mem(112, "vram:1B000", 4)
expect("the echoed address keeps the vram: prefix",
       r is not None and r.get("body", {}).get("address", "").lower().startswith("vram:"))

# A round trip through the write path. $1F9C0 is in the sprite attribute area,
# well clear of the text screen the READY prompt is sitting on.
scratch = "vram:1F9C0"
before = mem_bytes(read_mem(113, scratch, 4))
write_mem(114, scratch, [0xDE, 0xAD, 0xBE, 0xEF])
after = mem_bytes(read_mem(115, scratch, 4))
expect("writeMemory writes VRAM", after == b"\xde\xad\xbe\xef")
if before is not None:
    write_mem(116, scratch, list(before))
    expect("the scratch bytes restore", mem_bytes(read_mem(117, scratch, 4)) == before)

# $ and 0x have to mean the same address, and an offset has to apply in the
# space the reference named.
a = mem_bytes(read_mem(118, "vram:$1B000", 8))
b = mem_bytes(read_mem(119, "vram:0x1B000", 8))
expect("vram: accepts $ and 0x alike", a is not None and a == b == vram[:8])

# The trap this replaced: strtoul returned 0 for anything it could not read, so
# a typo answered success with the bytes at CPU address 0 and nothing in the
# response to say it was the wrong memory.
for bad in ("vram:zzz", "notanaddress", "vram:"):
    r = read_mem(120, bad, 4)
    expect("readMemory rejects %r instead of reading address 0" % bad,
           r is not None and not r.get("success"))

r = read_mem(121, "$C000", 4)
expect("readMemory accepts a $-prefixed CPU address", r is not None and r.get("success"))

# 15. Ownership: a session must take away only what it asked for.
#
# The core records who wanted each breakpoint, so a client disconnecting clears
# its own and leaves everything else armed. Reconstructing that from outside the
# core is what this replaced, and every version of it deleted somebody's
# breakpoints. Nothing below can be checked from the unit tests: it needs a real
# session, a real teardown, and a real reconnect.
#
# Start the emulator with `-bp <addr>` to get the strongest form of this -- the
# case where the surviving breakpoint is the user's, set before any client
# existed.

def evaluate(s, expr, pause=0.3):
    global _eval_seq
    _eval_seq += 1
    send_dap(s, {"seq": 500 + _eval_seq, "type": "request", "command": "evaluate",
                 "arguments": {"expression": expr}})
    time.sleep(pause)
    for m in recv_dap(s, 1):
        if m.get("type") == "response" and m.get("command") == "evaluate":
            return m.get("body", {}).get("result", "")
    return ""

def bp_addrs(s):
    # "N breakpoints: $1234 $5678" -> ["$1234", "$5678"]
    out = evaluate(s, "bp_list")
    return [t for t in out.split() if t.startswith("$")]

_eval_seq = 0

baseline = bp_addrs(sock)
print("       breakpoints before this session: %s" % (" ".join(baseline) or "(none)"))

# A console breakpoint at an address nothing else is using.
scratch = "3F00"
evaluate(sock, "bp_add " + scratch)
if ("$" + scratch) in bp_addrs(sock):
    print("  PASS console bp_add arms a breakpoint")
    passed += 1
else:
    print("  FAIL console bp_add arms a breakpoint")
    failed += 1

# The interesting one: claim an address the user already had. Both owners now
# want it, and the session going away must not take it with them.
shared = baseline[0][1:] if baseline else None
if shared:
    evaluate(sock, "bp_add " + shared)

send_dap(sock, {"seq": 13, "type": "request", "command": "disconnect",
                "arguments": {"terminateDebuggee": False}})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
check("disconnect response", msgs, "response", "disconnect")
sock.close()
time.sleep(0.5)

after_session = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
after_session.connect(('127.0.0.1', 9009))
send_dap(after_session, {"seq": 1, "type": "request", "command": "initialize",
                         "arguments": {"adapterID": "test"}})
time.sleep(0.5)
recv_dap(after_session, 1)

remaining = bp_addrs(after_session)
print("       breakpoints after teardown: %s" % (" ".join(remaining) or "(none)"))

if ("$" + scratch) not in remaining:
    print("  PASS the session's own breakpoint went with it")
    passed += 1
else:
    print("  FAIL the session's own breakpoint went with it: still armed, and now "
          "owned by nobody")
    failed += 1

if not baseline:
    print("  SKIP a pre-existing breakpoint survives a session "
          "(start the emulator with -bp to check this)")
elif all(a in remaining for a in baseline):
    print("  PASS every pre-existing breakpoint survived the session")
    passed += 1
else:
    print("  FAIL every pre-existing breakpoint survived the session: had %s, left %s"
          % (" ".join(baseline), " ".join(remaining) or "(none)"))
    failed += 1

after_session.close()
time.sleep(0.3)

# 14. Disconnect
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('127.0.0.1', 9009))
send_dap(sock, {"seq": 1, "type": "request", "command": "initialize",
                "arguments": {"adapterID": "test"}})
time.sleep(0.4)
recv_dap(sock, 1)
send_dap(sock, {"seq": 12, "type": "request", "command": "disconnect", "arguments": {"terminateDebuggee": False}})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
check("disconnect response", msgs, "response", "disconnect")

sock.close()

# 15. An oversized frame must not poison the receive buffer for later clients.
#
# A body bigger than the buffer drops the connection -- there is no way to
# resynchronise a stream we cannot hold. The header was left sitting in the
# buffer though, so the next client was accepted, had that stale header
# re-parsed before it had written a byte, and was dropped on connect. For the
# life of the process.
probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
probe.connect(('127.0.0.1', 9009))
probe.sendall(b"Content-Length: 100000\r\n\r\n")
time.sleep(0.5)
probe.close()
time.sleep(0.5)

try:
    after = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    after.connect(('127.0.0.1', 9009))
    time.sleep(0.5)          # let a poll run before we send anything
    send_dap(after, {"seq": 1, "type": "request", "command": "initialize",
                     "arguments": {"adapterID": "test"}})
    time.sleep(0.5)
    msgs = recv_dap(after, 2)
    check("server still usable after an oversized frame", msgs, "response", "initialize")
    after.close()
except OSError as e:
    print("  FAIL server still usable after an oversized frame: %s" % e)
    failed += 1

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
