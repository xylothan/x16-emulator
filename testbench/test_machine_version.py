"""Machine-version guard: fixed workloads must keep their recorded hashes.

    python testbench/test_machine_version.py <x16emu> <rom.bin> [--update] [--port N]

A save state names the machine version that wrote it (SAVESTATE_MACHINE_VERSION
in src/savestate.c), and a build loads states of its own version. That is only
safe while every build of a version runs the machine identically, and this is
what checks it: each workload starts from power-on, paused before the first
instruction, runs a fixed program for a fixed number of frames, and the state
hash at each checkpoint is compared with testbench/machine_golden.json.

  * Same version, a hash differs: the machine changed. Bump
    SAVESTATE_MACHINE_VERSION, empty legacy_builds, and regenerate.
  * The version differs from the goldens': regenerate (--update) and commit.

Every workload runs twice, so a workload that is not deterministic says so
instead of looking like a machine change, and each checks that its program
really drove the chips it is meant to (counters it leaves in RAM).

The hash skips the build ID, so the goldens do not depend on which build or
toolchain recorded them, only on the machine and on the ROM, whose SHA-256 is
recorded too. CI fetches that ROM by release tag and checks the hash.
"""
import base64, hashlib, json, os, shutil, socket, subprocess, sys, tempfile, time

HERE   = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(HERE, "machine_golden.json")

args   = [a for a in sys.argv[1:] if not a.startswith("--")]
UPDATE = "--update" in sys.argv
PORT   = 9411
if "--port" in sys.argv:
    PORT = int(sys.argv[sys.argv.index("--port") + 1])
    args.remove(str(PORT))
if len(args) != 2:
    print(__doc__)
    sys.exit(2)
EXE, ROM = args

with open(ROM, "rb") as f:
    ROM_BYTES = f.read()
ROM_SHA = hashlib.sha256(ROM_BYTES).hexdigest()
RESET   = ROM_BYTES[0x3FFC] | ROM_BYTES[0x3FFD] << 8

WORK = tempfile.mkdtemp(prefix="x16mv_")
PATTERN = bytes((i * 37 + 11) & 0xFF for i in range(64))
with open(os.path.join(WORK, "F.PRG"), "wb") as f:
    f.write(bytes([0x01, 0x08]) + PATTERN)

passed = failed = 0

def case(name, ok):
    global passed, failed
    print("  %s %s" % ("PASS" if ok else "FAIL", name))
    if ok: passed += 1
    else:  failed += 1


# --- a two-pass assembler, just enough for the programs below -----------------
# Items: an int is a byte; ("L", name) defines a label; ("B", name) is a branch
# offset to it; ("W", name or int) is a little-endian word.
def asm(org, items):
    def emit(labels):
        out, pc = bytearray(), org
        for it in items:
            if isinstance(it, int):
                out.append(it); pc += 1
            elif it[0] == "L":
                labels[it[1]] = pc
            elif it[0] == "B":
                off = labels.get(it[1], pc + 1) - (pc + 1)
                if labels.get(it[1]) is not None and not -128 <= off <= 127:
                    raise ValueError("branch to %s out of range" % it[1])
                out.append(off & 0xFF); pc += 1
            elif it[0] == "W":
                v = it[1] if isinstance(it[1], int) else labels.get(it[1], 0)
                out += bytes([v & 0xFF, v >> 8]); pc += 2
        return out
    labels = {}
    emit(labels)
    return bytes(emit(labels))

LDA_I, LDX_I, LDY_I = 0xA9, 0xA2, 0xA0
STA, LDA, STX, LDX  = 0x8D, 0xAD, 0x8E, 0xAE
STA_Z, LDA_Z, INC_Z = 0x85, 0xA5, 0xE6
LDX_Z               = 0xA6
JSR, JMP, RTS       = 0x20, 0x4C, 0x60
BEQ, BNE, BMI, BPL  = 0xF0, 0xD0, 0x30, 0x10
BIT, AND_I, ORA_I   = 0x2C, 0x29, 0x09
CLC, ADC_I, ADC_Z   = 0x18, 0x69, 0x65
SEI, CLD, TAX, TXA  = 0x78, 0xD8, 0xAA, 0x8A
STA_X               = 0x9D

def w(a): return ("W", a)
def lda_sta(val, addr): return [LDA_I, val, STA, w(addr)]

def vera_addr(a, incr=1):
    # ADDR_H: increment index in bits 4-7, bit 16 of the address in bit 0.
    return (lda_sta(a & 0xFF, 0x9F20) + lda_sta((a >> 8) & 0xFF, 0x9F21)
            + lda_sta((incr << 4) | (a >> 16), 0x9F22))


# --- workload: hostfs LOAD, then the PCM FIFO, KERNAL interrupts running ------
# The same program as test_determinism.py: a LOAD from device 8, then keeps the
# PCM FIFO fed and counts how often it was full. Joysticks change every frame,
# and the KERNAL's interrupt handler, which scans them, keeps running.
HOSTFS = asm(0x0400, [
    LDA_I, 5, LDX_I, 0x40, LDY_I, 0x04, JSR, w(0xFFBD),       # SETNAM "F.PRG" at $0440
    LDA_I, 1, LDX_I, 8, LDY_I, 1, JSR, w(0xFFBA),             # SETLFS 1,8,1
    LDA_I, 0, JSR, w(0xFFD5),                                 # LOAD
    LDA, w(0x9F28), STA_Z, 0x22,                              # scanline it finished on
    JMP, w(0x0480),
])
HOSTFS_NAME = b"F.PRG"
HOSTFS_PCM = asm(0x0480, [
    LDA_I, 0x8F, STA, w(0x9F3B), LDA_I, 0x20, STA, w(0x9F3C),
    ("L", "loop"),
    BIT, w(0x9F3B), BMI, ("B", "full"),
    INC_Z, 0x20, LDA_Z, 0x20, STA, w(0x9F3D), JMP, w("loop"),
    ("L", "full"),
    INC_Z, 0x21, JMP, w("loop"),
])

BUTTONS = ["right", "left", "a", "b", "up", "down", "start", "select"]

def joysticks_for(k):
    held = [b for i, b in enumerate(BUTTONS) if (k >> (i % 4)) & 1 and (k // 7 + i) % 3 == 0]
    return [{"index": 0, "buttons": held}]


# --- workload: the YM2151 -----------------------------------------------------
# All eight channels get a voice. Timer A runs, and each time its flag comes up
# the program clears it, keys a channel on or off and changes its pitch. Between
# times it counts polls, so the count depends on exactly when the timer fires
# and how long the chip stays busy after each write.
def ym_setup():
    regs = []
    for ch in range(8):
        regs += [(0x20 + ch, 0xC7), (0x28 + ch, 0x4A + ch * 4), (0x30 + ch, ch << 2), (0x38 + ch, 0)]
    for slot in range(32):
        regs += [(0x40 + slot, 0x01 + (slot & 3)), (0x60 + slot, 0x10 + slot), (0x80 + slot, 0x1F),
                 (0xA0 + slot, 0x05), (0xC0 + slot, 0x03), (0xE0 + slot, 0x27)]
    regs += [(0x10, 0xC0), (0x11, 0x00), (0x14, 0x15)]           # timer A: load, flag on
    out = []
    for r, v in regs:
        out += [LDA_I, r, LDX_I, v, JSR, w("ym_write")]
    return out

YM = asm(0x1000, [SEI, CLD] + ym_setup() + [
    ("L", "loop"),
    LDA, w(0x9F41), AND_I, 0x01, BNE, ("B", "fired"),
    INC_Z, 0x41, BNE, ("B", "loop"), INC_Z, 0x43, JMP, w("loop"),  # polls, 16 bits
    ("L", "fired"),
    LDA_I, 0x14, LDX_I, 0x15, JSR, w("ym_write"),            # clear the flag, keep running
    INC_Z, 0x40,                                             # timer count
    LDA_Z, 0x40, AND_I, 0x07, STA_Z, 0x42,                   # channel
    LDA_Z, 0x40, AND_I, 0x08, BNE, ("B", "off"),
    LDA_Z, 0x42, ORA_I, 0x78, TAX, JMP, w("key"),
    ("L", "off"),
    LDX_Z, 0x42,
    ("L", "key"),
    LDA_I, 0x08, JSR, w("ym_write"),
    LDA_Z, 0x42, CLC, ADC_I, 0x28, LDX_Z, 0x40, JSR, w("ym_write"),
    JMP, w("loop"),
    ("L", "ym_write"),                                       # A = register, X = value
    BIT, w(0x9F41), BMI, ("B", "ym_write"),
    STA, w(0x9F40), STX, w(0x9F41), RTS,
])


# --- workload: VERA, the PSG, PCM and a VIA timer -----------------------------
# Sixteen PSG voices sound, 32 overlapping 64x64 sprites are on (so they
# collide), a line interrupt is set, and the PCM FIFO is kept fed. Polling VERA's
# ISR, the program moves the sprites and scrolls a layer at every VSYNC, changes
# the background colour at the line, and samples the VIA 1 timer and the scanline
# into RAM each frame. Interrupts are off, so the KERNAL takes no part.
def vera_setup():
    out = vera_addr(0x1F9C0)
    for v in range(16):
        freq = 0x0200 + v * 0x0123
        for b in (freq & 0xFF, freq >> 8, 0xC0 | (0x10 + v), ((v & 3) << 6) | 0x20):
            out += lda_sta(b, 0x9F23)
    out += vera_addr(0x1FC00)
    for s in range(32):
        x, y = 40 + s * 17, 30 + (s * 29) % 400
        # Pixels from the charset at $1F000, so they are not transparent; one
        # collision mask for all, so any two that overlap collide.
        for b in (0x80, 0x0F, x & 0xFF, x >> 8, y & 0xFF, y >> 8, 0xFC, 0xF0):
            out += lda_sta(b, 0x9F23)
    out += lda_sta(0x00, 0x9F25)                                # DCSEL 0
    out += [LDA, w(0x9F29), ORA_I, 0x40, STA, w(0x9F29)]        # sprites on
    out += lda_sta(100, 0x9F28) + lda_sta(0x06, 0x9F26)         # line 100; line and collision
    out += lda_sta(0x40, 0x9F0B)                                # VIA 1 T1 free-running
    out += lda_sta(0x34, 0x9F04) + lda_sta(0x12, 0x9F05)
    out += lda_sta(0x8F, 0x9F3B) + lda_sta(0x10, 0x9F3C)        # PCM reset, volume 15
    return out

def move_sprites():
    out = vera_addr(0x1FC02, incr=4)                            # incr 8: each sprite's X
    for s in range(32):
        out += [LDA_Z, 0x51, CLC, ADC_I, (s * 13) & 0xFF, STA, w(0x9F23)]
    return out

VERA = asm(0x1000, [SEI, CLD] + vera_setup() + [
    ("L", "loop"),
    LDA, w(0x9F27), STA_Z, 0x50,
    AND_I, 0x01, BNE, ("B", "vsync"), JMP, w("notv"),
    ("L", "vsync"),
    STA, w(0x9F27), INC_Z, 0x51,                                # frames
    LDX_Z, 0x51, LDA, w(0x9F04), STA_X, w(0x0600), LDA, w(0x9F28), STA_X, w(0x0700),
    LDA_Z, 0x51, STA, w(0x9F37),                                # layer 1 H scroll
    ] + move_sprites() + vera_addr(0x1FA00) + [
    LDA_I, 0x00, STA, w(0x9F23), STA, w(0x9F23),                # background black again
    ] + vera_addr(0x1F9C0) + [LDA_Z, 0x51, STA, w(0x9F23), # PSG voice 0 pitch
    ("L", "notv"),
    LDA_Z, 0x50, AND_I, 0x02, BEQ, ("B", "notl"),
    LDA_I, 0x02, STA, w(0x9F27), INC_Z, 0x53,
    ] + vera_addr(0x1FA00) + [LDA_Z, 0x51, STA, w(0x9F23),      # background colour at the line
    ("L", "notl"),
    LDA_Z, 0x50, AND_I, 0x04, BEQ, ("B", "nots"),
    LDA_I, 0x04, STA, w(0x9F27), INC_Z, 0x54, LDA_Z, 0x50, STA_Z, 0x55,
    ("L", "nots"),
    BIT, w(0x9F3B), BMI, ("B", "pcmfull"),
    LDA_Z, 0x51, STA, w(0x9F3D), INC_Z, 0x56,
    ("L", "pcmfull"),
    JMP, w("loop"),
])


# --- DAP ---------------------------------------------------------------------
class Dap:
    def __init__(self, port):
        end = time.time() + 30
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.05)
        self.buf, self.seq = b"", 0

    def read(self, timeout):
        self.s.settimeout(timeout)
        while True:
            if b"\r\n\r\n" in self.buf:
                h, rest = self.buf.split(b"\r\n\r\n", 1)
                n = int([l for l in h.decode().split("\r\n")
                         if l.startswith("Content-Length")][0].split(":")[1])
                if len(rest) >= n:
                    self.buf = rest[n:]
                    return json.loads(rest[:n])
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("emulator closed the connection")
            self.buf += chunk

    def req(self, cmd, a=None, timeout=120):
        self.seq += 1
        body = json.dumps({"seq": self.seq, "type": "request", "command": cmd,
                           "arguments": a or {}}).encode()
        self.s.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        while True:
            m = self.read(timeout)
            if m.get("type") == "response" and m.get("request_seq") == self.seq:
                if not m.get("success", True):
                    raise RuntimeError("%s refused: %s" % (cmd, m.get("message")))
                return m.get("body", {})

    def mem(self, addr, n):
        r = self.req("readMemory", {"memoryReference": "0x%04X" % addr, "count": n})
        return base64.b64decode(r.get("data", ""))

    def poke(self, addr, data):
        self.req("writeMemory", {"memoryReference": "0x%04X" % addr,
                                 "data": base64.b64encode(bytes(data)).decode()})


START = 1200                      # frames from power-on before a program is loaded
BOOT_CHECKPOINTS = [1, 2, 5, 10, 30, 60, 120, 240, 480, 900, 1200]

def launch():
    """A machine paused before its first instruction, and its debug session."""
    global PORT
    PORT += 1
    env = dict(os.environ, SDL_AUDIODRIVER="dummy")
    cmd = [EXE, "-rom", ROM, "-debugport", str(PORT), "-hidden", "-warp", "-sound", "none",
           "-deterministic", "-seed", "1", "-fsroot", WORK, "-bp", "%04X" % RESET]
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    d = Dap(PORT)
    d.req("initialize", {"adapterID": "x16", "clientID": "machine-version"})
    d.req("launch", {"stopOnEntry": False})                # -bp has already stopped it
    d.req("configurationDone")
    end = time.time() + 20
    while not d.req("x16/status").get("paused"):
        if time.time() > end:
            raise RuntimeError("never stopped at the reset vector")
        time.sleep(0.02)
    return proc, d

def step_to(d, frame, **kw):
    now = int(d.req("x16/status").get("frame", 0))
    if frame <= now:
        raise RuntimeError("at frame %d, past %d" % (now, frame))
    r = d.req("x16/step", dict(frames=frame - now, hash=True, **kw))
    if r.get("frame") != frame:
        raise RuntimeError("stepped to frame %s, not %d" % (r.get("frame"), frame))
    return r["hash"]

def run_program(d, pieces, pc, frames, joysticks=False):
    step_to(d, START)
    for addr, data in pieces:
        d.poke(addr, data)
    d.req("setVariable", {"variablesReference": 1, "name": "PC", "value": "$%04X" % pc})
    points, hashes = [], []
    for k in range(frames):
        a = {"frames": 1, "hash": (k + 1) % 10 == 0}
        if joysticks:
            a["joysticks"] = joysticks_for(k)
        r = d.req("x16/step", a)
        if a["hash"]:
            points.append(int(r["frame"])); hashes.append(r["hash"])
    return points, hashes

def counter(d, addr, n=1):
    return int.from_bytes(d.mem(addr, n), "little")

def w_boot(d):
    return BOOT_CHECKPOINTS, [step_to(d, f) for f in BOOT_CHECKPOINTS], {}

def w_hostfs(d):
    pts, hs = run_program(d, [(0x0400, HOSTFS), (0x0440, HOSTFS_NAME), (0x0480, HOSTFS_PCM)],
                          0x0400, 300, joysticks=True)
    return pts, hs, {"file loaded": int(d.mem(0x0801, len(PATTERN)) == PATTERN),
                     "PCM bytes fed": counter(d, 0x20), "FIFO seen full": counter(d, 0x21)}

def w_ym(d):
    pts, hs = run_program(d, [(0x1000, YM)], 0x1000, 300)
    return pts, hs, {"timer A overflows": counter(d, 0x40), "polls": counter(d, 0x41)}

def w_vera(d):
    pts, hs = run_program(d, [(0x1000, VERA)], 0x1000, 300)
    return pts, hs, {"VSYNCs": counter(d, 0x51), "line interrupts": counter(d, 0x53),
                     "sprite collisions": counter(d, 0x54), "PCM bytes fed": counter(d, 0x56)}

WORKLOADS = [("boot", w_boot), ("hostfs-pcm", w_hostfs), ("ym2151", w_ym), ("vera-psg-via", w_vera)]

def run(fn):
    proc, d = launch()
    try:
        pts, hs, checks = fn(d)
        size = d.req("x16/saveState", {"slot": "mv", "hash": False}).get("size")
        machine = d.req("x16/status").get("machine")
        d.req("disconnect", {"terminateDebuggee": True})
        return {"frames": pts, "hashes": hs, "stateSize": size}, checks, machine
    finally:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


print("Machine version: %d workloads from power-on, ROM %s" % (len(WORKLOADS), ROM_SHA[:16]))
results, machine = {}, None
for name, fn in WORKLOADS:
    a, checks, machine = run(fn)
    b, _, _ = run(fn)
    case("%s: runs the same twice (last hash %s)" % (name, a["hashes"][-1]), a == b)
    for what, n in checks.items():
        case("%s: %s (%d)" % (name, what, n), n > 0)
    results[name] = a

current = {"machine": machine, "rom_sha256": ROM_SHA, "workloads": results}
try:
    with open(GOLDEN) as f:
        golden = json.load(f)
except (OSError, ValueError):
    golden = None

if UPDATE:
    with open(GOLDEN, "w", newline="\n") as f:
        json.dump(current, f, indent=1)
        f.write("\n")
    print("  wrote %s" % GOLDEN)
elif golden is None:
    case("goldens exist (%s); record them with --update" % GOLDEN, False)
elif golden.get("rom_sha256") != ROM_SHA:
    case("the ROM is the one the goldens were recorded with (%s, not %s)"
         % (golden.get("rom_sha256"), ROM_SHA), False)
elif golden.get("machine") != machine:
    case("SAVESTATE_MACHINE_VERSION is %s and the goldens are for %s: regenerate them with "
         "--update and commit testbench/machine_golden.json" % (machine, golden.get("machine")), False)
else:
    for name, now in results.items():
        was = golden["workloads"].get(name)
        if was == now:
            case("%s: the same as recorded for machine %s" % (name, machine), True)
            continue
        if was is None:
            case("%s: has goldens (regenerate with --update)" % name, False)
            continue
        diff = next((f for f, x, y in zip(now["frames"], now["hashes"], was["hashes"]) if x != y), None)
        how = ("first differs at frame %d" % diff if diff is not None
               else "state size %s, was %s" % (now["stateSize"], was["stateSize"]))
        case("%s: the same as recorded for machine %s (%s). The machine has changed: "
             "bump SAVESTATE_MACHINE_VERSION in src/savestate.c, empty legacy_builds, "
             "and regenerate with --update" % (name, machine, how), False)

if failed and not UPDATE:
    print("\nWhat this build gives, for testbench/machine_golden.json:")
    print(json.dumps(current, indent=1))

shutil.rmtree(WORK, ignore_errors=True)
print("\n%d passed, %d failed" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
