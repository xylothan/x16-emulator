"""Save states: a restored machine carries on exactly as the saved one did.

    python testbench/test_savestate.py <x16emu> <rom.bin> [first-port]

Runs a small program that keeps every device busy -- sprites crossing so their
collisions fire, a line interrupt changing the scroll mid-screen, PSG voices,
the PCM FIFO hovering at its almost-empty mark, a YM2151 note with noise
vibrato and both its timers, a free-running VIA timer, the real-time clock
and its NVRAM and the SMC read over I2C, the SPI controller -- with a fixed
joystick pattern, recording the machine state hash after every frame
("run A").

Then, in the same process: save, run A, run a second program ("B") that
scribbles over every one of those devices and over RAM, load, run A again. The
two runs of A must match frame for frame. Restoring into the machine just saved
from would hide a field the state misses, because the stale value would happen
to be right; B is what makes a missing field show.

Also: a process that never saves runs A identically (saving changes nothing),
save/load/save gives the same state, and a load works from a stop that is not
at a frame boundary.

Across processes: a state saved to a file is loaded by a fresh process that
never ran the program, which then runs A frame for frame as the saver did,
draws the same picture and plays the same audio. A file is refused by a machine
with another ROM or another configuration, and when it is damaged.

Local only: it launches emulators and needs a ROM.
"""
import base64, json, os, shutil, socket, subprocess, sys, tempfile, time

EXE  = sys.argv[1]
ROM  = sys.argv[2]
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 9511

START_FRAME = 1200   # well past boot
SETTLE      = 30     # frames for the program to set everything up
A_FRAMES    = 120
B_FRAMES    = 60

WORK = tempfile.mkdtemp(prefix="x16ss_")
STATE_FILE = os.path.join(WORK, "f.x16s")

passed = failed = 0

def case(name, ok):
    global passed, failed
    print("  %s %s" % ("PASS" if ok else "FAIL", name))
    if ok: passed += 1
    else:  failed += 1


# ─── A small 65C02 assembler: enough for the programs below ──────────────────

OPS = {
    # (mnemonic, mode): opcode. Modes: imp, imm, zp, abs, absx, ind, rel
    ("lda", "imm"): 0xA9, ("lda", "zp"): 0xA5, ("lda", "abs"): 0xAD, ("lda", "absx"): 0xBD,
    ("ldx", "imm"): 0xA2, ("ldy", "imm"): 0xA0,
    ("sta", "zp"): 0x85, ("sta", "abs"): 0x8D, ("sta", "absx"): 0x9D,
    ("stz", "abs"): 0x9C,
    ("inc", "zp"): 0xE6, ("inx", "imp"): 0xE8,
    ("and", "imm"): 0x29, ("ora", "imm"): 0x09, ("eor", "zp"): 0x45, ("eor", "imm"): 0x49,
    ("adc", "zp"): 0x65, ("clc", "imp"): 0x18,
    ("cmp", "imm"): 0xC9, ("cpx", "imm"): 0xE0,
    ("bit", "abs"): 0x2C,
    ("beq", "rel"): 0xF0, ("bne", "rel"): 0xD0, ("bmi", "rel"): 0x30,
    ("jmp", "abs"): 0x4C, ("jmp", "ind"): 0x6C, ("jsr", "abs"): 0x20,
    ("ldx", "zp"): 0xA6, ("ldy", "zp"): 0xA4,
    ("sei", "imp"): 0x78, ("cli", "imp"): 0x58,
    ("txa", "imp"): 0x8A,
}
SIZE = {"imp": 1, "imm": 2, "zp": 2, "rel": 2, "abs": 3, "absx": 3, "ind": 3}

def assemble(org, lines):
    """lines: label strings, (mnemonic, mode[, operand]) tuples, or bytes."""
    for pass_ in (0, 1):
        pc, out, labels = org, bytearray(), (labels if pass_ else {})
        for item in lines:
            if isinstance(item, str):
                labels[item] = pc
                continue
            if isinstance(item, (bytes, bytearray)):
                out += item
                pc += len(item)
                continue
            mn, mode = item[0], item[1]
            arg = item[2] if len(item) > 2 else None
            val = labels.get(arg, 0) if isinstance(arg, str) else arg
            if isinstance(arg, str) and arg.startswith("<"): val = labels.get(arg[1:], 0) & 0xFF
            if isinstance(arg, str) and arg.startswith(">"): val = labels.get(arg[1:], 0) >> 8
            out.append(OPS[(mn, mode)])
            if mode == "rel":
                off = (val - (pc + 2)) if pass_ else 0
                assert pass_ == 0 or -128 <= off <= 127, (mn, arg)
                out.append(off & 0xFF)
            elif SIZE[mode] == 2:
                out.append(val & 0xFF)
            elif SIZE[mode] == 3:
                out += bytes((val & 0xFF, (val >> 8) & 0xFF))
            pc += SIZE[mode]
    return bytes(out), labels


def ym_write(reg, val_mode, val, wait):
    # Wait for the YM2151 to finish its last write, then write one register.
    return [wait, ("bit", "abs", 0x9F41), ("bmi", "rel", wait),
            ("lda", "imm", reg), ("sta", "abs", 0x9F40),
            ("lda", val_mode, val), ("sta", "abs", 0x9F41)]

YM_SETUP = bytes([
    0x20, 0xC7, 0x28, 0x4A, 0x30, 0x00,                 # ch0: L+R, algorithm 7; note
    0x40, 0x01, 0x48, 0x02, 0x50, 0x03, 0x58, 0x01,     # DT1/MUL
    0x60, 0x10, 0x68, 0x18, 0x70, 0x20, 0x78, 0x10,     # TL
    0x80, 0x1F, 0x88, 0x1F, 0x90, 0x1F, 0x98, 0x1F,     # attack
    0xA0, 0x05, 0xA8, 0x05, 0xB0, 0x05, 0xB8, 0x05,     # decay 1
    0xC0, 0x02, 0xC8, 0x02, 0xD0, 0x02, 0xD8, 0x02,     # decay 2
    0xE0, 0x27, 0xE8, 0x27, 0xF0, 0x27, 0xF8, 0x27,     # D1L/RR
    0x18, 0xF0, 0x19, 0xFF, 0x1B, 0x03, 0x38, 0x70,     # vibrato from a fast noise LFO
    0x10, 0x80, 0x11, 0x01, 0x12, 0xC0,                 # timer A and B periods
    0x14, 0x0F,                                         # start both, with their IRQ flags
    0x08, 0x78,                                         # key on ch0
    0xFF])

# Sprites 0 and 1, 64x64 4bpp from VRAM $13000 (seeded noise, so plenty of set
# pixels), in front, collision mask $F; sprite 0 moves, so they cross.
SPRITES = bytes([0x80, 0x09, 100, 0, 100, 0, 0xFC, 0xF0,
                 0x80, 0x09, 200, 0, 110, 0, 0xFC, 0xF0])
# PSG voices 0 and 1: a sawtooth and noise.
PSG = bytes([0x00, 0x04, 0xFF, 0x7F,
             0x00, 0x10, 0xFF, 0xFF])

OLDV = 0x05F0
I2C_READ  = 0xFEC6   # KERNAL: X device, Y register -> A
I2C_WRITE = 0xFEC9   # KERNAL: X device, Y register, A value
RTC, SMC  = 0x6F, 0x42

PROGRAM_A, LBL = assemble(0x0400, [
    # ── setup, run once before the state is taken ──
    ("sei", "imp"),
    ("stz", "abs", 0x0001),                                         # ROM bank 0: the KERNAL
    ("lda", "abs", 0x0314), ("sta", "abs", OLDV),
    ("lda", "abs", 0x0315), ("sta", "abs", OLDV + 1),
    ("lda", "imm", "<irq"), ("sta", "abs", 0x0314),
    ("lda", "imm", ">irq"), ("sta", "abs", 0x0315),
    ("stz", "abs", 0x9F25),                                         # DCSEL 0, ADDRSEL 0
    ("lda", "abs", 0x9F29), ("ora", "imm", 0x40), ("sta", "abs", 0x9F29),   # sprites on
    ("lda", "imm", 0x00), ("sta", "abs", 0x9F20),
    ("lda", "imm", 0xFC), ("sta", "abs", 0x9F21),
    ("lda", "imm", 0x11), ("sta", "abs", 0x9F22),                  # $1FC00, increment 1
    ("ldx", "imm", 0),
    "spr", ("lda", "absx", "sprtab"), ("sta", "abs", 0x9F23), ("inx", "imp"),
           ("cpx", "imm", len(SPRITES)), ("bne", "rel", "spr"),
    ("lda", "imm", 0xC0), ("sta", "abs", 0x9F20),
    ("lda", "imm", 0xF9), ("sta", "abs", 0x9F21),                  # $1F9C0: PSG
    ("ldx", "imm", 0),
    "psg", ("lda", "absx", "psgtab"), ("sta", "abs", 0x9F23), ("inx", "imp"),
           ("cpx", "imm", len(PSG)), ("bne", "rel", "psg"),
    ("lda", "imm", 100), ("sta", "abs", 0x9F28),                   # line IRQ at 100
    ("lda", "imm", 0x0F), ("sta", "abs", 0x9F26),                  # VSYNC, LINE, SPRCOL, AFLOW
    ("lda", "imm", 0x8F), ("sta", "abs", 0x9F3B),                  # PCM: reset, 8-bit mono, vol 15
    ("lda", "imm", 0x20), ("sta", "abs", 0x9F3C),                  # PCM rate
    ("ldx", "imm", 0),
    "yml", ("lda", "absx", "ymtab"), ("cmp", "imm", 0xFF), ("beq", "rel", "ymd"),
    "ymb", ("bit", "abs", 0x9F41), ("bmi", "rel", "ymb"),
           ("sta", "abs", 0x9F40), ("inx", "imp"),
           ("lda", "absx", "ymtab"), ("sta", "abs", 0x9F41), ("inx", "imp"),
           ("jmp", "abs", "yml"),
    "ymd",
    ("lda", "imm", 0x40), ("sta", "abs", 0x9F0E),                  # VIA1: no T1 interrupt
    ("lda", "abs", 0x9F0B), ("ora", "imm", 0x40), ("sta", "abs", 0x9F0B),   # T1 free-running
    ("lda", "imm", 0x34), ("sta", "abs", 0x9F04),
    ("lda", "imm", 0x12), ("sta", "abs", 0x9F05),
    ("cli", "imp"),
    # ── run A: the main loop, which only reads ──
    "main",
    ("inc", "zp", 0x30),
    ("lda", "abs", 0x9F28), ("sta", "zp", 0x31),                   # scanline
    ("lda", "abs", 0x9F41), ("sta", "zp", 0x32),                   # YM status
    ("lda", "abs", 0x9F04), ("sta", "zp", 0x33),                   # VIA1 T1 low
    ("lda", "abs", 0x9F3F), ("sta", "zp", 0x3A),                   # SPI control
    ("ldx", "imm", RTC), ("ldy", "imm", 0x00), ("jsr", "abs", I2C_READ), ("sta", "zp", 0x3B),  # RTC seconds
    ("ldx", "imm", RTC), ("ldy", "imm", 0x20), ("jsr", "abs", I2C_READ), ("sta", "zp", 0x3C),  # RTC NVRAM
    ("ldx", "imm", SMC), ("ldy", "imm", 0x22), ("jsr", "abs", I2C_READ), ("sta", "zp", 0x3D),  # mouse id
    ("jmp", "abs", "main"),
    # ── interrupt handler, chained ahead of the KERNAL's ──
    "irq",
    ("lda", "abs", 0x9F27), ("sta", "zp", 0x34),
    ("and", "imm", 0x02), ("beq", "rel", "noline"),
    ("lda", "imm", 0x02), ("sta", "abs", 0x9F27), ("inc", "zp", 0x35),
    ("lda", "zp", 0x35), ("sta", "abs", 0x9F37),                   # L1 HSCROLL, mid-screen
    "noline",
    ("lda", "zp", 0x34), ("and", "imm", 0x04), ("beq", "rel", "nocol"),
    ("lda", "imm", 0x04), ("sta", "abs", 0x9F27), ("inc", "zp", 0x36),
    "nocol",
    ("lda", "zp", 0x34), ("and", "imm", 0x08), ("beq", "rel", "noaf"),
    ("ldx", "imm", 0),
    "af", ("txa", "imp"), ("eor", "zp", 0x30), ("sta", "abs", 0x9F3D), ("inx", "imp"),
          ("bne", "rel", "af"),                                    # 256 bytes into the FIFO
    "noaf",
    ("lda", "zp", 0x34), ("and", "imm", 0x01), ("beq", "rel", "out"),
    ("lda", "imm", 0x02), ("sta", "abs", 0x9F20),
    ("lda", "imm", 0xFC), ("sta", "abs", 0x9F21),
    ("lda", "imm", 0x01), ("sta", "abs", 0x9F22),                  # sprite 0 X
    ("inc", "zp", 0x37), ("lda", "zp", 0x37), ("sta", "abs", 0x9F23),
    *ym_write(0x14, "imm", 0x3F, "yw1"),                           # clear YM timer flags
    ("lda", "zp", 0x37), ("and", "imm", 0x0F), ("bne", "rel", "out"),
    "yw2", ("bit", "abs", 0x9F41), ("bmi", "rel", "yw2"),
    ("lda", "imm", 0x08), ("sta", "abs", 0x9F40),
    ("lda", "zp", 0x37), ("and", "imm", 0x10), ("beq", "rel", "koff"),
    ("lda", "imm", 0x78),
    "koff", ("sta", "abs", 0x9F41),                                # key on/off every 16 frames
    "out", ("jmp", "ind", OLDV),
    "sprtab", SPRITES,
    "psgtab", PSG,
    "ymtab", YM_SETUP,
])
assert 0x0400 + len(PROGRAM_A) <= OLDV, hex(0x0400 + len(PROGRAM_A))

# B: interrupts off, then write over every device and the RAM A uses. VIA1's
# ports are left alone: they are the I2C bus to the SMC, which could power the
# machine off.
PROGRAM_B, LBL_B = assemble(0x0600, [
    ("sei", "imp"),
    "loop",
    ("inc", "zp", 0x38),
    ("ldx", "imm", 0),
    "vera", ("txa", "imp"), ("clc", "imp"), ("adc", "zp", 0x38),
            ("sta", "absx", 0x9F20), ("inx", "imp"),
            ("cpx", "imm", 0x20), ("bne", "rel", "vera"),          # $9F20-$9F3F
    ("ldx", "imm", 4),
    "via", ("txa", "imp"), ("eor", "zp", 0x38), ("sta", "absx", 0x9F00), ("inx", "imp"),
           ("cpx", "imm", 0x0C), ("bne", "rel", "via"),            # $9F04-$9F0B: timers, SR, ACR
    "ywb", ("bit", "abs", 0x9F41), ("bmi", "rel", "ywb"),
    ("lda", "zp", 0x38), ("sta", "abs", 0x9F40),
    ("eor", "imm", 0x55), ("sta", "abs", 0x9F41),                 # any YM register
    ("ldx", "imm", 0),
    "ram", ("txa", "imp"), ("eor", "zp", 0x38),
           ("sta", "absx", 0x0400), ("sta", "absx", 0x0500),
           ("sta", "absx", 0xA000), ("inx", "imp"),
           ("bne", "rel", "ram"),
    ("ldx", "imm", RTC), ("ldy", "imm", 0x20), ("lda", "zp", 0x38),
    ("jsr", "abs", I2C_WRITE),                                     # RTC NVRAM
    ("ldx", "imm", SMC), ("ldy", "imm", 0x20), ("lda", "zp", 0x38), ("and", "imm", 0x04),
    ("jsr", "abs", I2C_WRITE),                                     # SMC: mouse id 0 or 4
    ("lda", "zp", 0x38), ("sta", "zp", 0x00),                      # RAM bank
    ("sta", "zp", 0x30), ("sta", "zp", 0x31), ("sta", "zp", 0x34), ("sta", "zp", 0x37),
    ("jmp", "abs", "loop"),
])

BUTTONS = ["right", "left", "a", "b", "up", "down", "start", "select"]

def joys_a(k):
    held = [b for i, b in enumerate(BUTTONS) if (k >> (i % 4)) & 1 and (k // 5 + i) % 3 == 0]
    return [{"index": 0, "buttons": held}]

def joys_b(k):
    return [{"index": 0, "buttons": BUTTONS[k % 8:k % 8 + 2]},
            {"index": 1, "buttons": ["a", "start"] if k & 1 else []}]


# ─── DAP ─────────────────────────────────────────────────────────────────────

class Dap:
    def __init__(self, port):
        end = time.time() + 20
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.05)
        self.buf = b""
        self.seq = 0

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

    def req(self, cmd, args=None, timeout=60):
        self.seq += 1
        body = json.dumps({"seq": self.seq, "type": "request", "command": cmd,
                           "arguments": args or {}}).encode()
        self.s.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        while True:
            m = self.read(timeout)
            if m.get("type") == "response" and m.get("request_seq") == self.seq:
                return m

    def status(self):
        return self.req("x16/status").get("body", {})

    def poke(self, addr, data):
        self.req("writeMemory", {"memoryReference": "0x%04X" % addr,
                                 "data": base64.b64encode(bytes(data)).decode()})

    def set_pc(self, pc):
        self.req("setVariable", {"variablesReference": 1, "name": "PC", "value": "$%04X" % pc})

    def run(self, frames, joys):
        hashes = []
        for k in range(frames):
            r = self.req("x16/step", {"frames": 1, "joysticks": joys(k), "hash": True})
            hashes.append(r.get("body", {}).get("hash"))
        return hashes


def launch_bare(port, rom=ROM, extra=(), wav=None):
    """A process that boots and is paused at once, wherever it has got to."""
    env = dict(os.environ, SDL_AUDIODRIVER="dummy")
    cmd = [EXE, "-rom", rom, "-debugport", str(port), "-hidden", "-warp",
           "-deterministic", "-seed", "1"] + list(extra)
    if wav:
        cmd += ["-wav", wav]
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    d = Dap(port)
    d.req("pause", {"threadId": 1})
    end = time.time() + 5
    while not d.status().get("paused") and time.time() < end:
        time.sleep(0.02)
    return proc, d

def launch(port, wav=None):
    """A process stepped to START_FRAME with both programs loaded and A set up."""
    proc, d = launch_bare(port, wav=wav)
    f0 = int(d.status().get("frame", 0))
    d.req("x16/step", {"frames": START_FRAME - f0}, timeout=120)
    d.poke(0x0400, PROGRAM_A)
    d.poke(0x0600, PROGRAM_B)
    d.set_pc(0x0400)
    d.req("x16/step", {"frames": SETTLE}, timeout=60)
    return proc, d

def close(proc, d):
    try:
        d.req("disconnect", {"terminateDebuggee": True})
    except Exception:
        pass
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()

def wav_tail(path, n_bytes):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return b""
    i = data.find(b"data")
    data = data[i + 8:] if i >= 0 else b""
    return data[-n_bytes:] if len(data) >= n_bytes else b""

def screen(d):
    return d.req("x16/status", {"screenHash": True}).get("body", {}).get("screenHash")

def first_difference(a, b):
    if not a or not b or None in a or None in b:
        return "a run failed"
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return "first differs at frame %d of %d" % (i + 1, len(a))
    return "identical"


print("Save states (%d frames of A from frame %d, B for %d)" % (A_FRAMES, START_FRAME + SETTLE, B_FRAMES))

# A process that never saves: the reference. Its recording ends with A.
REF_WAV = os.path.join(WORK, "ref.wav")
proc, d = launch(PORT + 1, wav=REF_WAV)
zp = None
h0 = d.run(A_FRAMES, joys_a)
scr0 = screen(d)
zp = d.req("readMemory", {"memoryReference": "0x0030", "count": 8}).get("body", {}).get("data", "")
zp = base64.b64decode(zp)
close(proc, d)
print("    the program ran: main loop %d, line IRQs %d, collisions %d, frames %d"
      % (zp[0], zp[5], zp[6], zp[7]) if len(zp) >= 8 else "    (no zero page read)")
case("the program's line and collision interrupts both fired", len(zp) >= 8 and zp[5] > 0 and zp[6] > 0)

proc, d = launch(PORT + 2)
sv = d.req("x16/saveState", {"slot": "f", "hash": True})
sb = sv.get("body", {})
case("x16/saveState at a frame boundary (%s bytes, frame %s)" % (sb.get("size"), sb.get("frame")),
     sv.get("success") and sb.get("size", 0) > 0 and sb.get("hash"))
print("    (state size %.1f KB)" % (sb.get("size", 0) / 1024.0))
fv = d.req("x16/saveState", {"path": STATE_FILE, "hash": True})
fb = fv.get("body", {})
case("x16/saveState to a file writes the same state (%s bytes)" % fb.get("size"),
     fv.get("success") and fb.get("path") == STATE_FILE and fb.get("hash") == sb.get("hash")
     and os.path.exists(STATE_FILE) and os.path.getsize(STATE_FILE) == sb.get("size"))

h1 = d.run(A_FRAMES, joys_a)
scr1 = screen(d)
case("saving changes nothing: A after a save matches A without one (%s)" % first_difference(h0, h1),
     h0 == h1 and None not in h1)
case("  and draws the same picture", scr1 is not None and scr1 == scr0)

d.set_pc(0x0600)
d.run(B_FRAMES, joys_b)
s_b = d.status()

t0 = time.perf_counter()
ld = d.req("x16/loadState", {"slot": "f", "hash": True})
load_ms = (time.perf_counter() - t0) * 1000.0
lb = ld.get("body", {})
case("x16/loadState returns to the saved frame and cycle (%s -> %s)" % (s_b.get("frame"), lb.get("frame")),
     ld.get("success") and lb.get("frame") == sb.get("frame") and lb.get("cycles") == sb.get("cycles"))
case("the loaded machine hashes the same as the saved one", lb.get("hash") == sb.get("hash"))
st = d.status()
case("after a load the machine is paused at a frame boundary",
     st.get("paused") is True and st.get("atFrameBoundary") is True)
case("framesTotal keeps counting across a load (%s, frame %s)" % (st.get("framesTotal"), st.get("frame")),
     st.get("framesTotal", 0) > st.get("frame", 0))

h2 = d.run(A_FRAMES, joys_a)
case("perturb then restore: A after B and a load matches A (%s)" % first_difference(h1, h2), h1 == h2)
case("  and draws the same picture", screen(d) == scr1)

# Save, load, save: the same state.
d.req("x16/loadState", {"slot": "f"})
sv2 = d.req("x16/saveState", {"slot": "g", "hash": True}).get("body", {})
case("save, load, save gives the same state (%s bytes)" % sv2.get("size"),
     sv2.get("hash") == sb.get("hash") and sv2.get("size") == sb.get("size"))

# A load from a stop that is not a frame boundary.
d.set_pc(0x0600)
d.req("continue", {"threadId": 1})
time.sleep(0.05)
d.req("pause", {"threadId": 1})
end = time.time() + 5
while not d.status().get("paused") and time.time() < end:
    time.sleep(0.02)
mid = d.status()
refused = d.req("x16/saveState", {"slot": "mid"})
case("x16/saveState refuses a stop that is not a frame boundary (%s)" % refused.get("message"),
     mid.get("atFrameBoundary") is False and not refused.get("success"))
ld3 = d.req("x16/loadState", {"slot": "f"})
h3 = d.run(A_FRAMES, joys_a)
case("a load from mid-frame, then A, matches A (%s)" % first_difference(h1, h3),
     ld3.get("success") and h1 == h3)

# Load time: a state is about the size of the machine's RAM, so it is memcpy-bound.
N = 50
t0 = time.perf_counter()
for _ in range(N):
    d.req("x16/loadState", {"slot": "f"})
per = (time.perf_counter() - t0) * 1000.0 / N
print("    (x16/loadState round trip: %.2f ms average over %d; first was %.2f ms)" % (per, N, load_ms))
case("a load round trip takes under 10 ms (%.2f ms)" % per, per < 10.0)

close(proc, d)

# ─── Across processes ────────────────────────────────────────────────────────
#
# A fresh process that has only booted loads the file and runs A. Its recording
# ends with A just as the reference's does, so the two must end alike: those
# samples come from the restored machine alone.
Q_WAV = os.path.join(WORK, "q.wav")
proc, d = launch_bare(PORT + 3, wav=Q_WAV)
before = d.status()
scr_boot = screen(d)
case("the fresh process shows a different picture before the load", scr_boot and scr_boot != scr0)
ld = d.req("x16/loadState", {"path": STATE_FILE, "hash": True})
lb = ld.get("body", {})
case("a fresh process loads the file (from frame %s to %s)" % (before.get("frame"), lb.get("frame")),
     ld.get("success") and lb.get("frame") == sb.get("frame") and lb.get("hash") == sb.get("hash"))
hq = d.run(A_FRAMES, joys_a)
case("cross-process: A after loading the file matches A in the process that saved it (%s)"
     % first_difference(h1, hq), h1 == hq)
case("  and draws the same picture", screen(d) == scr0)

# A refused load leaves the machine as it was.
h_before = d.req("x16/status", {"hash": True}).get("body", {})
with open(STATE_FILE, "rb") as f:
    data = f.read()
short = os.path.join(WORK, "short.x16s")
with open(short, "wb") as f:
    f.write(data[:len(data) // 2])
junk = os.path.join(WORK, "junk.x16s")
with open(junk, "wb") as f:
    f.write(bytes((i * 131 + 7) & 0xFF for i in range(len(data))))
r_short = d.req("x16/loadState", {"path": short})
r_junk  = d.req("x16/loadState", {"path": junk})
r_none  = d.req("x16/loadState", {"path": os.path.join(WORK, "missing.x16s")})
r_both  = d.req("x16/loadState", {"path": STATE_FILE, "slot": "f"})
h_after = d.req("x16/status", {"hash": True}).get("body", {})
case("a truncated file is refused (%s)" % r_short.get("message"), not r_short.get("success"))
case("a file that is not a state is refused (%s)" % r_junk.get("message"), not r_junk.get("success"))
case("a missing file is refused (%s)" % r_none.get("message"), not r_none.get("success"))
case("a slot and a path together are refused (%s)" % r_both.get("message"), not r_both.get("success"))
case("  and after all of those the machine is untouched",
     h_after.get("hash") == h_before.get("hash") and h_after.get("frame") == h_before.get("frame"))
close(proc, d)

n = 72000 * 4   # about 1.5 s of 16-bit stereo, inside A's 2 s
ta, tq = wav_tail(REF_WAV, n), wav_tail(Q_WAV, n)
case("cross-process: and the same audio as the reference (%d samples compared)" % (len(ta) // 4),
     len(ta) == n and ta == tq and any(ta))

# Another ROM: one byte changed.
rom2 = os.path.join(WORK, "rom2.bin")
with open(ROM, "rb") as f:
    rb = bytearray(f.read())
rb[len(rb) // 2] ^= 0xFF
with open(rom2, "wb") as f:
    f.write(rb)
proc, d = launch_bare(PORT + 4, rom=rom2)
r = d.req("x16/loadState", {"path": STATE_FILE})
case("a machine with another ROM refuses the file (%s)" % r.get("message"),
     not r.get("success") and "ROM" in (r.get("message") or ""))
close(proc, d)

# Another configuration.
proc, d = launch_bare(PORT + 5, extra=["-ram", "1024"])
r = d.req("x16/loadState", {"path": STATE_FILE})
case("a machine with more banked RAM refuses the file (%s)" % r.get("message"),
     not r.get("success") and "banked RAM" in (r.get("message") or ""))
close(proc, d)

shutil.rmtree(WORK, ignore_errors=True)
print("\n%d passed, %d failed" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
