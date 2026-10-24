import socket, json, time, sys, base64

# The port is an argument so a run can dodge whatever else on the machine has
# already claimed 9009 -- a stray emulator or an editor's DAP client connecting
# to this one will otherwise hijack the session halfway through the suite.
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9009


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
    sock.connect(('127.0.0.1', PORT))
except ConnectionRefusedError:
    print("ERROR: Cannot connect to DAP server on port %d. Is emulator running with -debugport?" % PORT)
    sys.exit(1)
print("Connected to DAP server on port %d" % PORT)

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

# 15b. x16/perfStats and x16/perfConfig: the guest performance budget.
#
# The arithmetic is pinned by tests/test_perf_budget.c, which drives the module
# directly. What can only be checked here is that the surface is wired to a real
# machine: that the frame period it reports is the one VERA is actually scanning
# at, and that a target frame rate means what the header says it means.


def perf(seq, args=None):
    send_dap(sock, {"seq": seq, "type": "request", "command": "x16/perfConfig",
                    "arguments": args if args is not None else {}})
    time.sleep(0.3)
    for m in recv_dap(sock, 1):
        if m.get("command") == "x16/perfConfig":
            return m
    return None


# Frames only complete while the machine runs, and the stepping tests above left
# it stopped. Nothing here means anything against a paused CPU.
send_dap(sock, {"seq": 129, "type": "request", "command": "continue",
                "arguments": {"threadId": 1}})
time.sleep(0.5)
recv_dap(sock, 1)

r = perf(130, {"enabled": True, "targetFps": 60, "idleMode": "auto", "reset": True})
expect("x16/perfConfig arms profiling", r is not None and r.get("success"))
budget = r["body"]["budget"] if r and r.get("success") else {}
expect("profiling reports itself enabled", budget.get("enabled") is True)

# 8 MHz over a VGA frame: 8e6 * (800*525) / 25e6. The emulator derives this from
# the video timing rather than hardcoding it, so a wrong answer here means the
# budget is being measured against a frame the machine is not scanning.
expect("a VGA frame at 8 MHz is 134400 cycles", budget.get("cyclesPerFrame") == 134400)
expect("which is 256 cycles per scanline", budget.get("cyclesPerScanline") == 256)
expect("the 60 fps budget is one whole frame",
       budget.get("budgetCycles") == 134400 and budget.get("framesPerBudget") == 1)

# A lower target buys more frames, not longer ones.
r = perf(131, {"targetFps": 30})
b30 = r["body"]["budget"] if r and r.get("success") else {}
expect("30 fps spans two vsyncs", b30.get("framesPerBudget") == 2)
expect("and doubles the budget", b30.get("budgetCycles") == 268800)
expect("without changing the frame period", b30.get("cyclesPerFrame") == 134400)

# A target faster than the machine scans is the other question -- "must this fit
# in half a frame?" -- and gets a fractional budget rather than being rounded
# up to a whole one.
r = perf(132, {"targetFps": 120})
b120 = r["body"]["budget"] if r and r.get("success") else {}
expect("a sub-frame target gets a fraction of a frame",
       b120.get("budgetCycles", 0) < 134400 * 0.6)

perf(133, {"targetFps": 60})

time.sleep(1.0)  # let some frames accumulate
send_dap(sock, {"seq": 134, "type": "request", "command": "x16/perfStats",
                "arguments": {"windows": [1, 0], "includeFrames": 3}})
time.sleep(0.5)
stats = None
for m in recv_dap(sock, 2):
    if m.get("command") == "x16/perfStats":
        stats = m
expect("x16/perfStats responds", stats is not None and stats.get("success"))

if stats and stats.get("success"):
    body = stats["body"]
    cur = body.get("current", {})
    expect("a completed frame is reported", bool(cur))
    expect("the frame holds a frame's worth of cycles",
           130000 <= cur.get("totalCycles", 0) <= 140000)
    # The invariant the whole classification rests on.
    expect("work and idle account for every cycle",
           cur.get("workCycles", 0) + cur.get("idleCycles", -1) == cur.get("totalCycles"))

    wins = body.get("windows", [])
    expect("both requested windows came back", len(wins) == 2)
    if wins:
        bw = wins[0].get("budgetWork", {})
        expect("percentiles are ordered",
               bw.get("min", 0) <= bw.get("p50", 0) <= bw.get("p95", 0) <= bw.get("max", 0))
        expect("the window reports overrun accounting",
               "overruns" in wins[0] and "overrunPct" in wins[0])
    frames = body.get("frames", [])
    expect("raw frame samples came back", len(frames) == 3)
    if len(frames) >= 2:
        expect("raw frames are oldest first", frames[0]["frame"] < frames[-1]["frame"])

# Zones are attributed by address, so one drawn over ROM must collect something.
r = perf(135, {"reset": True,
               "zones": [{"name": "kernal", "start": 0xC000, "end": 0xFFFF}]})
expect("zones can be declared over DAP", r is not None and r.get("success"))
time.sleep(0.8)
send_dap(sock, {"seq": 136, "type": "request", "command": "x16/perfStats",
                "arguments": {"windows": [1]}})
time.sleep(0.5)
for m in recv_dap(sock, 2):
    if m.get("command") == "x16/perfStats":
        zones = m["body"].get("zones", [])
        expect("the declared zone came back", len(zones) == 1)
        if zones:
            expect("the zone is named as declared", zones[0].get("name") == "kernal")
            expect("and has window statistics", "window30s" in zones[0])

# Leave the machine as we found it: profiling costs it something per instruction.
r = perf(137, {"enabled": False, "zones": []})
expect("profiling can be switched back off",
       r is not None and r["body"]["budget"].get("enabled") is False)

# 15c. VERA bandwidth: the other half of "what is eating my frame?".
#
# The arithmetic is pinned by tests/test_vera_bandwidth.c, which drives the
# model directly against the RTL's decode tables. What can only be checked here
# is that it is wired to a real machine and to a real display mode -- a booted
# ROM sits at the BASIC prompt in 1bpp text mode on layer 1, so there are
# numbers that MUST appear and bounds they cannot leave.
send_dap(sock, {"seq": 138, "type": "request", "command": "x16/perfConfig",
                "arguments": {"enabled": True, "reset": True}})
time.sleep(1.0)  # let several frames land
send_dap(sock, {"seq": 139, "type": "request", "command": "x16/perfStats",
                "arguments": {"windows": [1]}})
time.sleep(0.5)
bwstats = None
for m in recv_dap(sock, 2):
    if m.get("command") == "x16/perfStats":
        bwstats = m

expect("x16/perfStats carries a bandwidth object",
       bwstats is not None and "bandwidth" in bwstats.get("body", {}))

if bwstats and bwstats.get("success"):
    bwb = bwstats["body"]["bandwidth"]

    # The bus geometry, quoted from the RTL. If these drift, every figure built
    # on them is wrong, and the unit tests would not notice a bad constant
    # reaching the wire.
    expect("a scanline is 800 clocks", bwb.get("lineClocks") == 800)
    expect("a VRAM access is four bytes", bwb.get("bytesPerAccess") == 4)
    expect("and costs two clocks", bwb.get("accessClocks") == 2)
    expect("VGA scans 525 lines", bwb.get("scanlinesPerFrame") == 525)

    expect("bandwidth accounting reports itself enabled", bwb.get("enabled") is True)
    expect("a frame has been accounted", bwb.get("linesActive", 0) > 0)

    # The BASIC prompt is a text-mode screen on layer 1, so layer 1 must be
    # fetching and the peak line must be a real number of clocks.
    layers = bwb.get("layers", [])
    expect("both layers are reported", len(layers) == 2)
    if len(layers) == 2:
        expect("layer 1 is fetching at the BASIC prompt",
               layers[1].get("fetches", 0) > 0)
        expect("its bytes are its fetches times four",
               layers[1].get("bytes") == layers[1].get("fetches", 0) * 4)
        # 1bpp 8px tiles: 40 map + 80 data = 120 a line, 121-123 when scrolled.
        # Checked as a range because hscroll and the scroll tile move it.
        peak = layers[1].get("peakFetches", 0)
        expect("a text-mode line fetches about 120 words", 118 <= peak <= 126)

    # The headline. It must be inside the window a scanline actually has --
    # a text screen is nowhere near filling the bus.
    peak_clocks = bwb.get("peakLineClocks", 0)
    expect("the peak scanline is a real cost", peak_clocks > 0)
    expect("and fits inside the scanline", peak_clocks <= 800)
    expect("so no line is over budget", bwb.get("linesOverBudget", -1) == 0)
    expect("percentile and mean are ordered",
           bwb.get("meanLineClocks", 0) <= bwb.get("p95LineClocks", 0) <= peak_clocks)
    expect("sprites are left the rest of the scanline",
           bwb.get("spriteBusHeadroomClocksAtPeak", -1) == 800 - peak_clocks)
    # Every clock in this object is bus occupancy, not sprite render time. The
    # two differ by the sprite renderer's duty cycle, and a client that
    # subtracted one from the other would be wrong by about fivefold.
    expect("and the clock unit is declared", bwb.get("clockUnits") == "bus")

    port = bwb.get("port", {})
    expect("the data port is reported", isinstance(port, dict) and "writeBytes" in port)

# The data port is the part a program controls, and the debugger is NOT a
# program. Two different paths have to be kept off the books, and only the
# first is directly observable from here:
#
#   * writeMemory with a "vram:" reference reaches VRAM through
#     video_space_write(), around the $9F23 path entirely. Checked below.
#   * writeMemory to the CPU address $9F23 goes through real_write6502() with
#     debugOn set, which raises video_set_debug_write() so the accounting
#     declines it. That one cannot be pinned reliably from out here -- under
#     -warp the frames roll far faster than requests arrive, so a burst of
#     debugger pokes never lands in one frame whether it is charged or not.
#     video.c carries the guard and the comment instead.
#
# If either were charged, opening a memory view would move the numbers the
# developer is trying to read.
send_dap(sock, {"seq": 140, "type": "request", "command": "x16/perfConfig",
                "arguments": {"reset": True}})
time.sleep(0.3)
recv_dap(sock, 1)
send_dap(sock, {"seq": 141, "type": "request", "command": "writeMemory",
                "arguments": {"memoryReference": "vram:1B000",
                              "data": base64.b64encode(bytes(64)).decode()}})
time.sleep(0.5)
recv_dap(sock, 1)

# Sampling has to be generous here, and it is worth saying why. The bandwidth
# figures are per-frame, and a machine sitting at the BASIC prompt touches VRAM
# only when the cursor blinks -- measured at about 8 frames in 120. Under -warp
# thousands of emulated frames pass between two requests, so each reply shows an
# essentially random frame. One sample would be a coin toss; a hundred makes
# missing the blink altogether vanishingly unlikely.
#
# Requests are pipelined in batches rather than sent one at a time because
# recv_dap() blocks for its whole timeout, so a request-reply-request loop would
# spend a minute here to gather what this gathers in seconds.
debugger_charged = False
guest_port_bytes = 0
seq = 142
for _ in range(5):
    for _ in range(20):
        send_dap(sock, {"seq": seq, "type": "request", "command": "x16/perfStats",
                        "arguments": {"windows": [1], "includeZones": False}})
        seq += 1
        time.sleep(0.03)
    for m in recv_dap(sock, 1):
        if m.get("command") != "x16/perfStats":
            continue
        p = m.get("body", {}).get("bandwidth", {}).get("port", {})
        guest_port_bytes = max(guest_port_bytes,
                               p.get("writeBytes", 0) + p.get("readBytes", 0))
        # 64 bytes landing in one frame could only be the DAP write above.
        if p.get("writeBytes", 0) >= 64:
            debugger_charged = True

expect("a debugger VRAM write is not charged to the guest", not debugger_charged)
expect("but the guest's own data-port traffic is", guest_port_bytes > 0)

# Leave the machine as we found it.
perf(seq, {"enabled": False})


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
    # "N breakpoints: $1234 05:A000" -> ["$1234", "05:A000"]
    out = evaluate(s, "bp_list")
    return [t for t in out.split() if t.startswith("$") or ":" in t[1:]]

_eval_seq = 0

# 15d. x16/listBreakpoints: every armed breakpoint/watchpoint, any owner.
#
# setBreakpoints and its siblings only tell a client about breakpoints IT
# itself set. This is the read-back for the whole picture. setBreakpoints /
# setFunctionBreakpoints resolve a source line (or label) to an address
# through a loaded .dbg file, which this generic-ROM harness does not have,
# so -- like those requests elsewhere in this file -- they are not exercised
# here. What IS checked is the console-owned path this file already has a
# hand on: does the shape, the owner decoding, and the hit/ignore-count
# accounting come back right for an entry the harness can actually create.


def list_breakpoints(seq):
    send_dap(sock, {"seq": seq, "type": "request", "command": "x16/listBreakpoints"})
    time.sleep(0.3)
    for m in recv_dap(sock, 1):
        if m.get("command") == "x16/listBreakpoints":
            return m
    return None


lb_bp_addr = 0x3E00
lb_wp_addr = 0x3E80
evaluate(sock, "bp_add %04X" % lb_bp_addr)
evaluate(sock, "watch_add %04X" % lb_wp_addr)

r = list_breakpoints(150)
expect("x16/listBreakpoints responds", r is not None and r.get("success"))

if r and r.get("success"):
    body = r.get("body", {})
    bps = body.get("breakpoints", [])
    wps = body.get("watchpoints", [])

    entry = next((b for b in bps if b.get("pc") == lb_bp_addr), None)
    expect("the console-added breakpoint is listed", entry is not None)
    if entry:
        expect("it is owned by dapConsole", "dapConsole" in entry.get("owners", []))
        expect("it is enabled", entry.get("enabled") is True)
        expect("it has no condition", entry.get("condition") is None)
        expect("it carries an ignore count and hit count",
               "ignoreCount" in entry and "hits" in entry)
        expect("it has no DAP-source identity (not set via setBreakpoints)",
               "dapId" not in entry)

    wentry = next((w for w in wps if w.get("addr") == lb_wp_addr), None)
    expect("the console-added watchpoint is listed", wentry is not None)
    if wentry:
        expect("it is active", wentry.get("active") is True)
        expect("it is owned by dapConsole", "dapConsole" in wentry.get("owners", []))
        expect("it has no value filter", wentry.get("valueFilter") is None)

# Leave the machine as we found it.
evaluate(sock, "bp_remove %04X" % lb_bp_addr)
evaluate(sock, "watch_remove %04X" % lb_wp_addr)

# 15e. x16/spriteMuxStats and x16/spriteMuxConfig: sprite-multiplexing stats.
#
# Structural twin of x16/perfStats/x16/perfConfig above -- same self-arming
# poll, same explicit config request. The machine has been running since
# section 15b, which matters here: sprite_trace only advances on a completed
# frame, so nothing below means anything against a paused CPU.


def sprite_mux_config(seq, args=None):
    send_dap(sock, {"seq": seq, "type": "request", "command": "x16/spriteMuxConfig",
                    "arguments": args if args is not None else {}})
    time.sleep(0.3)
    for m in recv_dap(sock, 1):
        if m.get("command") == "x16/spriteMuxConfig":
            return m
    return None


def sprite_mux_stats(seq, args=None):
    send_dap(sock, {"seq": seq, "type": "request", "command": "x16/spriteMuxStats",
                    "arguments": args if args is not None else {}})
    time.sleep(0.3)
    for m in recv_dap(sock, 1):
        if m.get("command") == "x16/spriteMuxStats":
            return m
    return None


r = sprite_mux_config(160, {"enabled": True, "burstGap": 64, "reset": True})
expect("x16/spriteMuxConfig arms tracing", r is not None and r.get("success"))
expect("tracing reports itself enabled",
       r is not None and r.get("body", {}).get("enabled") is True)
expect("burstGap round-trips through config",
       r is not None and r.get("body", {}).get("burstGap") == 64)

# Frames only complete while the machine runs, and tracing was just reset, so
# poll with retries rather than a single fixed sleep.
frame = None
stats_body = None
for i in range(20):
    r = sprite_mux_stats(161 + i)
    if r and r.get("success"):
        body = r.get("body", {})
        if body.get("frame"):
            stats_body = body
            frame = body["frame"]
            break
    time.sleep(0.1)

expect("x16/spriteMuxStats eventually reports a frame", frame is not None)
if frame:
    expect("burstGap round-trips through stats", stats_body.get("burstGap") == 64)
    expect("effective sprites is a plausible count",
           0 <= frame.get("effectiveSprites", -1) <= 128)
    expect("peak line is within the visible frame", 0 <= frame.get("peakLine", -1) < 480)
    expect("slots used does not exceed the sprite table",
           0 <= frame.get("slotsUsed", -1) <= 128)

# Leave the machine as we found it: tracing allocates ~600KB of buffers.
r = sprite_mux_config(200, {"enabled": False})
expect("sprite-mux tracing can be switched back off",
       r is not None and r.get("body", {}).get("enabled") is False)

# 15f. x16/capture: a short capture with the default recorders (histogram and
# call tree). The arithmetic is pinned by tests/test_profile_cct.c and
# tests/test_profile_report.c; what only a live machine shows is that the CPU
# core's call and interrupt hooks feed the tree the same cycles the histogram
# counts -- the KERNAL's interrupt handler runs every frame, so [IRQ] cannot be
# empty -- and that the recorders are released afterwards.


def capture(seq, args):
    send_dap(sock, {"seq": seq, "type": "request", "command": "x16/capture", "arguments": args})
    time.sleep(0.3)
    for m in recv_dap(sock, 1):
        if m.get("command") == "x16/capture":
            return m
    return None


r = capture(300, {"start": {"frames": 30}})
expect("x16/capture starts", r is not None and r.get("success"))
expect("with the histogram and the call tree by default",
       r is not None and set(r.get("body", {}).get("recorders", [])) == {"hist", "tree"})
rep = None
for i in range(40):
    r = capture(301 + i, {"top": 5})
    body = r.get("body", {}) if r and r.get("success") else {}
    if body.get("state") == "done":
        rep = body.get("report")
        break
    time.sleep(0.1)
expect("the capture finishes with a report", rep is not None)
if rep:
    tree = rep.get("tree", [])
    expect("the report carries a call tree", len(tree) >= 2)
    expect("its self totals match the histogram's", rep.get("treeConsistent") is True)
    roots = {n.get("name"): i for i, n in enumerate(tree) if n.get("parent") == -1}
    expect("rooted at [main] and [IRQ]", set(roots) == {"[main]", "[IRQ]"})
    if set(roots) == {"[main]", "[IRQ]"}:
        total = rep.get("mainCycles", 0) + rep.get("irqCycles", 0)
        expect("the roots' inclusive time is every cycle",
               tree[roots["[main]"]]["incl"] + tree[roots["[IRQ]"]]["incl"] == total)
        handlers = [n for n in tree if n.get("parent") == roots["[IRQ]"]]
        expect("the KERNAL's interrupt handler is under [IRQ]",
               bool(handlers) and handlers[0].get("calls", 0) >= 30)
    expect("rows carry inclusive time and calls",
           all("inclCycles" in row and "calls" in row for row in rep.get("rows", [])))

    # The emulator writes the exports itself: collapsed stacks and JSON.
    import os
    import tempfile
    tmp = tempfile.mkdtemp()
    folded, js = os.path.join(tmp, "cap.folded"), os.path.join(tmp, "cap.json")
    r = capture(350, {"top": 0, "export": {"folded": folded, "json": js}})
    done = r.get("body", {}).get("exported", {}) if r and r.get("success") else {}
    expect("x16/capture writes collapsed stacks", done.get("folded", 0) >= 1 and os.path.exists(folded))
    if os.path.exists(folded):
        lines = open(folded).read().splitlines()
        expect("each line is a path and a cycle count",
               lines and all(l.rsplit(" ", 1)[-1].isdigit() for l in lines))
        expect("whose counts add up to every cycle",
               sum(int(l.rsplit(" ", 1)[-1]) for l in lines)
               == rep.get("mainCycles", 0) + rep.get("irqCycles", 0))
    expect("and JSON", done.get("json", 0) >= 1 and os.path.exists(js))
    if os.path.exists(js):
        import json as _json
        data = _json.load(open(js))
        expect("the JSON holds the rows and the tree",
               len(data.get("rows", [])) >= 1 and len(data.get("tree", [])) >= 2)

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
after_session.connect(('127.0.0.1', PORT))
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
sock.connect(('127.0.0.1', PORT))
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
probe.connect(('127.0.0.1', PORT))
probe.sendall(b"Content-Length: 100000\r\n\r\n")
time.sleep(0.5)
probe.close()
time.sleep(0.5)

try:
    after = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    after.connect(('127.0.0.1', PORT))
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

# 16. Bank-qualified breakpoints.
#
# A loop at $0400 maps RAM bank 1 and calls $A000, then bank 2 and calls $A000.
# Each bank holds its own INC/RTS there. A breakpoint pinned to a bank must stop
# only while that bank is mapped. This takes over the CPU, so it runs last.
bk = socket.create_connection(('127.0.0.1', PORT))
bk_buf = [b""]
bk_seq = [0]
bk_events = []

def bk_read(timeout):
    bk.settimeout(timeout)
    while True:
        b = bk_buf[0]
        if b"\r\n\r\n" in b:
            h, rest = b.split(b"\r\n\r\n", 1)
            n = int([l for l in h.decode().split("\r\n")
                     if l.startswith("Content-Length")][0].split(":")[1])
            if len(rest) >= n:
                bk_buf[0] = rest[n:]
                return json.loads(rest[:n])
        bk_buf[0] += bk.recv(65536)

def bk_req(cmd, args=None):
    bk_seq[0] += 1
    send_dap(bk, {"seq": bk_seq[0], "type": "request", "command": cmd,
                  "arguments": args or {}})
    while True:
        r = bk_read(5)
        if r.get("type") == "event":
            bk_events.append(r)
        elif r.get("request_seq") == bk_seq[0]:
            return r

def bk_wait_stop(timeout):
    end = time.time() + timeout
    while True:
        for i, e in enumerate(bk_events):
            if e.get("event") == "stopped":
                del bk_events[:i + 1]
                return e
        left = end - time.time()
        if left <= 0:
            return None
        try:
            r = bk_read(left)
        except socket.timeout:
            return None
        if r.get("type") == "event":
            bk_events.append(r)

def bk_rd(addr, n=1):
    r = bk_req("readMemory", {"memoryReference": "0x%04X" % addr, "count": n})
    return base64.b64decode(r["body"]["data"])

def bk_wr(addr, data):
    bk_req("writeMemory", {"memoryReference": "0x%04X" % addr,
                           "data": base64.b64encode(bytes(data)).decode()})

def bk_stops(n, timeout=2.0):
    """Continue n times; the RAM bank mapped at each stop, None if it ran on."""
    banks = []
    for _ in range(n):
        del bk_events[:]
        bk_req("continue", {"threadId": 1})
        if not bk_wait_stop(timeout):
            banks.append(None)
            del bk_events[:]
            bk_req("pause", {"threadId": 1})
            bk_wait_stop(3)
            break
        banks.append(bk_rd(0x0000)[0])
    return banks

def bk_instr(ref):
    r = bk_req("setInstructionBreakpoints",
               {"breakpoints": [{"instructionReference": ref}]})
    return r["body"]["breakpoints"][0]

def bk_eval(expr):
    return bk_req("evaluate", {"expression": expr}).get("body", {}).get("result", "")

bk_req("initialize", {"adapterID": "test"})
bk_req("attach", {})
bk_req("configurationDone", {})
bk_req("pause", {"threadId": 1})
bk_wait_stop(3)
bk_eval("bp_clear")

bk_wr(0x0400, [0x78,                          # SEI
               0xA9, 0x01, 0x85, 0x00,        # LDA #1 / STA $00
               0x20, 0x00, 0xA0,              # JSR $A000
               0xA9, 0x02, 0x85, 0x00,        # LDA #2 / STA $00
               0x20, 0x00, 0xA0,              # JSR $A000
               0x4C, 0x01, 0x04])             # JMP $0401
for bank, zp in ((1, 0x10), (2, 0x11), (3, 0x12)):
    bk_wr(0x0000, [bank])
    bk_wr(0xA000, [0xE6, zp, 0x60])           # INC zp / RTS
bk_wr(0x0000, [1])
bk_req("setVariable", {"variablesReference": 1, "name": "PC", "value": "$0400"})

def bk_case(name, ok):
    global passed, failed
    if ok:
        print("  PASS " + name)
        passed += 1
    else:
        print("  FAIL " + name)
        failed += 1

bk_instr("2:A000")
got = bk_stops(4)
bk_case("instruction bp 2:A000 stops only in bank 2 (got %s)" % got, got == [2, 2, 2, 2])

bk_instr("0x01A000")
got = bk_stops(4)
bk_case("instruction bp 0x01A000 stops only in bank 1 (got %s)" % got, got == [1, 1, 1, 1])

bk_instr("3:A000")
got = bk_stops(1)
bk_case("instruction bp in a bank that never runs does not stop (got %s)" % got, got == [None])

bk_instr("$A000")
got = bk_stops(4)
bk_case("instruction bp $A000 stops in every bank (got %s)" % got, sorted(set(got)) == [1, 2])

r = bk_instr("A000")
bk_case("bare hex instructionReference is hex: A000 arms $A000 (%s)" % bk_eval("bp_list"),
        r.get("verified") and "$A000" in bk_eval("bp_list"))

r = bk_instr("A0Z0")
bk_case("malformed instructionReference is refused (%s)" % r, not r.get("verified"))
bk_req("setInstructionBreakpoints", {"breakpoints": []})

res = bk_eval("bp_add 2:A000")
lst = bk_eval("bp_list")
bk_case("bp_add 2:A000 pins bank 2 (%r, %r)" % (res, lst), "02:A000" in lst)
got = bk_stops(3)
bk_case("  ...and stops only in bank 2 (got %s)" % got, got == [2, 2, 2])
res = bk_eval("bp_remove 2:A000")
bk_case("bp_remove 2:A000 removes it (%r)" % res, "02:A000" not in bk_eval("bp_list"))

res = bk_eval("bp_add 2:A0Z0")
bk_case("bp_add refuses a malformed address (%r)" % res, res.startswith("not a breakpoint address"))
bk_eval("bp_clear")

# Conditions on instruction breakpoints. In the loop above A is 1 when bank 1's
# $A000 runs and 2 when bank 2's does.
def bk_instr_bp(bp):
    r = bk_req("setInstructionBreakpoints", {"breakpoints": [bp]})
    return r["body"]["breakpoints"][0]

r = bk_instr_bp({"instructionReference": "$A000", "condition": "A == 2"})
got = bk_stops(3)
bk_case("instruction bp condition A == 2 stops only when A is 2 (got %s)" % got,
        r.get("verified") and got == [2, 2, 2])

r = bk_instr_bp({"instructionReference": "$A000", "condition": "bank == 1"})
got = bk_stops(3)
bk_case("instruction bp condition bank == 1 pins bank 1 (got %s)" % got,
        r.get("verified") and got == [1, 1, 1])

r = bk_instr_bp({"instructionReference": "$A000", "condition": "bank == 2 && A == 1"})
got = bk_stops(1)
bk_case("instruction bp bank == 2 && A == 1 never matches (got %s)" % got,
        r.get("verified") and got == [None])

r = bk_instr_bp({"instructionReference": "$A000", "x16Bank": 1})
got = bk_stops(3)
bk_case("instruction bp x16Bank field pins bank 1 (got %s)" % got,
        r.get("verified") and got == [1, 1, 1])

# Stopped on $A000 with its INC still to run. Resuming runs that INC without
# counting it, so stopping on the 3rd arrival means 3 INCs since.
before = bk_rd(0x10, 2)
r = bk_instr_bp({"instructionReference": "$A000", "hitCondition": "3"})
bk_stops(1)
after = bk_rd(0x10, 2)
incs = ((after[0] - before[0]) + (after[1] - before[1])) % 256
bk_case("instruction bp hitCondition 3 stops on the 3rd arrival (%d INCs ran)" % incs,
        r.get("verified") and incs == 3)

# State changed while the machine runs freely. The emulator skips the full
# status check on instructions where nothing can stop it (DEBUGPollStatus in
# debugger.h), so these are the cases that skip could get wrong: a request
# arriving mid-run, and a breakpoint table that changes under a running CPU.
import os, tempfile
bk_req("setInstructionBreakpoints", {"breakpoints": []})

def bk_run_free():
    del bk_events[:]
    bk_req("continue", {"threadId": 1})
    time.sleep(0.3)

bk_run_free()
t0 = time.time()
bk_req("pause", {"threadId": 1})
e = bk_wait_stop(2)
dt = time.time() - t0
# The socket is polled every DEBUG_POLL_INTERVAL (1000) instructions, about
# 0.1 ms at 8 MHz. 0.2 s is far above that but well below what a counting bug
# polling once per million instructions would take (~0.4 s).
bk_case("pause while running stops promptly (%.2fs)" % dt, e is not None and dt < 0.2)

bk_run_free()
r = bk_instr("2:A000")
e = bk_wait_stop(2)
bank = bk_rd(0x0000)[0] if e else None
bk_case("instruction bp armed while running stops on its next arrival (bank %s)" % bank,
        r.get("verified") and e is not None and bank == 2)
bk_req("setInstructionBreakpoints", {"breakpoints": []})

dump_path = os.path.join(tempfile.gettempdir(), "x16_dap_fastpath_dump.bin")
if os.path.exists(dump_path):
    os.remove(dump_path)
bk_run_free()
r = bk_instr_bp({"instructionReference": "$A000",
                 "x16Dump": {"file": dump_path, "regions": [{"addr": 0x10, "len": 2}]}})
time.sleep(0.5)
ran_on = bk_wait_stop(0.2) is None
bk_req("setInstructionBreakpoints", {"breakpoints": []})   # closes, so flushes, the file
time.sleep(0.2)
size = os.path.getsize(dump_path) if os.path.exists(dump_path) else 0
bk_case("x16Dump armed while running writes and keeps running (%d bytes)" % size,
        r.get("x16Dump") is True and ran_on and size > 0)
if os.path.exists(dump_path):
    os.remove(dump_path)
bk_req("pause", {"threadId": 1})
bk_wait_stop(3)

for bp, why in (
        ({"instructionReference": "$A000", "condition": "A == banana"}, "an unreadable condition"),
        ({"instructionReference": "$A000", "condition": "A == 1 && X == 0"}, "two value tests"),
        ({"instructionReference": "2:A000", "condition": "bank == 1"}, "two different banks"),
        ({"instructionReference": "$A000", "x16Bank": 1, "condition": "bank == 2"}, "x16Bank vs condition bank"),
        ({"instructionReference": "$0401", "condition": "bank == 2"}, "a bank on unbanked memory"),
        ({"instructionReference": "$A000", "hitCondition": "% 2"}, "an unreadable hitCondition")):
    r = bk_instr_bp(bp)
    bk_case("instruction bp refuses %s (%s)" % (why, r.get("message")),
            not r.get("verified") and r.get("message"))
bk_req("setInstructionBreakpoints", {"breakpoints": []})

bk_eval("bp_clear")
bk_req("disconnect", {"terminateDebuggee": False})
bk.close()

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
