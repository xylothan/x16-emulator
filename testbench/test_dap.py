import socket, json, time, sys

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

# 13. Disconnect
send_dap(sock, {"seq": 12, "type": "request", "command": "disconnect", "arguments": {"terminateDebuggee": False}})
time.sleep(0.3)
msgs = recv_dap(sock, 1)
check("disconnect response", msgs, "response", "disconnect")

sock.close()
print("\n%d passed, %d failed" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
