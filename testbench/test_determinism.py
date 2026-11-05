"""Determinism: the same build, seed and input give the same machine.

    python testbench/test_determinism.py <x16emu> <rom.bin> [first-port]

Launches the emulator several times, each on its own debug port, and drives
every run identically over DAP with x16/step: pause, step to a fixed absolute
frame, then load a small program that LOADs a file from the host filesystem,
notes the scanline it finished on, and keeps the PCM FIFO busy. Step it frame
by frame with a fixed joystick pattern, recording the machine state hash after
each frame. Each run also records its
audio output with -wav. Runs that must agree are compared hash for hash and
sample for sample.

The runs are aligned by absolute frame number, not by when the pause landed:
each process runs freely for a moment before it is paused, a different number
of frames each time, and stepping on to the same frame lines them up again.

Local only: it launches several emulators and needs a ROM.
"""
import base64, json, os, shutil, socket, subprocess, sys, tempfile, time

EXE  = sys.argv[1]
ROM  = sys.argv[2]
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 9311

START_FRAME = 1200   # well past boot; every run is stepped to here first
FRAMES      = 300    # then this many single-frame steps of the program

WORK = tempfile.mkdtemp(prefix="x16det_")
# A PRG for the hostfs LOAD: load address $0801, then a recognisable pattern.
PATTERN = bytes((i * 37 + 11) & 0xFF for i in range(64))
with open(os.path.join(WORK, "F.PRG"), "wb") as f:
    f.write(bytes([0x01, 0x08]) + PATTERN)

passed = failed = 0

def case(name, ok):
    global passed, failed
    print("  %s %s" % ("PASS" if ok else "FAIL", name))
    if ok: passed += 1
    else:  failed += 1


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

    def wait_paused(self, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            if self.req("x16/status").get("body", {}).get("paused"):
                return True
            time.sleep(0.02)
        return False

    def mem(self, addr, n):
        r = self.req("readMemory", {"memoryReference": "0x%04X" % addr, "count": n})
        return base64.b64decode(r.get("body", {}).get("data", ""))


# $0400: LOAD "F.PRG" from device 8 to the address in the file, then note the
# scanline VERA is on. The CPU is busy here, not waiting for an interrupt, so if
# the load is charged the host's time, the scanline moves with it.
LOADER = [
    0xA9, 0x05,             # 0400 LDA #5        name length
    0xA2, 0x40,             # 0402 LDX #<$0440
    0xA0, 0x04,             # 0404 LDY #>$0440
    0x20, 0xBD, 0xFF,       # 0406 JSR SETNAM
    0xA9, 0x01,             # 0409 LDA #1
    0xA2, 0x08,             # 040B LDX #8        device 8: hostfs
    0xA0, 0x01,             # 040D LDY #1        load to the file's own address
    0x20, 0xBA, 0xFF,       # 040F JSR SETLFS
    0xA9, 0x00,             # 0412 LDA #0        load, not verify
    0x20, 0xD5, 0xFF,       # 0414 JSR LOAD
    0xAD, 0x28, 0x9F,       # 0417 LDA $9F28     SCANLINE_L
    0x85, 0x22,             # 041A STA $22
    0x4C, 0x80, 0x04,       # 041C JMP $0480
]
FILENAME = b"F.PRG"         # at $0440

# $0480: keeps the PCM FIFO busy. Whether it drains, and when, depends on the
# sound chips being clocked. A count of "FIFO full" sightings lands in $21.
PCM = [
    0xA9, 0x8F,             # 0480 LDA #$8F     reset FIFO, volume 15, 8-bit mono
    0x8D, 0x3B, 0x9F,       # 0482 STA $9F3B
    0xA9, 0x20,             # 0485 LDA #$20
    0x8D, 0x3C, 0x9F,       # 0487 STA $9F3C    sample rate
    0xAD, 0x3B, 0x9F,       # 048A LDA $9F3B    loop: FIFO full?
    0x30, 0x0A,             # 048D BMI $0499
    0xE6, 0x20,             # 048F INC $20
    0xA5, 0x20,             # 0491 LDA $20
    0x8D, 0x3D, 0x9F,       # 0493 STA $9F3D    feed the FIFO
    0x4C, 0x8A, 0x04,       # 0496 JMP $048A
    0xE6, 0x21,             # 0499 INC $21      count full sightings
    0x4C, 0x8A, 0x04,       # 049B JMP $048A
]

BUTTONS = ["right", "left", "a", "b", "up", "down", "start", "select"]

def joysticks_for(k):
    held = [b for i, b in enumerate(BUTTONS) if (k >> (i % 4)) & 1 and (k // 7 + i) % 3 == 0]
    return [{"index": 0, "buttons": held}]


class Run:
    """One emulator process, driven through the fixed script."""
    n = 0

    def __init__(self, label, extra_args, env_extra=None, hostfs=True):
        global PORT
        PORT += 1
        Run.n += 1
        self.label = label
        self.hashes = None
        self.loaded = None
        self.wav = os.path.join(WORK, "run%d.wav" % Run.n)
        env = dict(os.environ)
        env.update(env_extra or {})
        cmd = [EXE, "-rom", ROM, "-debugport", str(PORT), "-hidden",
               "-fsroot", WORK, "-wav", self.wav] + extra_args
        proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            self._drive(Dap(PORT), hostfs)
        finally:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    def _drive(self, d, hostfs):
        d.req("pause", {"threadId": 1})
        if not d.wait_paused():
            print("    %s: never paused" % self.label)
            return
        f0 = int(d.req("x16/status").get("body", {}).get("frame", 0))
        if f0 >= START_FRAME:
            print("    %s: paused at frame %d, past the start frame %d" % (self.label, f0, START_FRAME))
            return
        r = d.req("x16/step", {"frames": START_FRAME - f0}, timeout=120)
        if r.get("body", {}).get("frame") != START_FRAME:
            print("    %s: did not reach frame %d (%s)" % (self.label, START_FRAME, r))
            return

        def poke(addr, data):
            d.req("writeMemory", {"memoryReference": "0x%04X" % addr,
                                  "data": base64.b64encode(bytes(data)).decode()})
        poke(0x0400, LOADER)
        poke(0x0440, FILENAME)
        poke(0x0480, PCM)
        d.req("setVariable", {"variablesReference": 1, "name": "PC",
                              "value": "$0400" if hostfs else "$0480"})
        hashes = []
        for k in range(FRAMES):
            r = d.req("x16/step", {"frames": 1, "joysticks": joysticks_for(k), "hash": True})
            hashes.append(r.get("body", {}).get("hash"))
        if hostfs:
            self.loaded = d.mem(0x0801, len(PATTERN))
        zp = d.mem(0x0021, 2)
        print("    %s: last hash %s, FIFO-full count %s, scanline after LOAD %s"
              % (self.label, hashes[-1], zp[0] if zp else "?",
                 zp[1] if hostfs and len(zp) > 1 else "-"))
        self.hashes = hashes
        d.req("disconnect", {"terminateDebuggee": True})

    def samples(self):
        try:
            with open(self.wav, "rb") as f:
                data = f.read()
        except OSError:
            return b""
        i = data.find(b"data")
        return data[i + 8:] if i >= 0 else b""


def first_difference(a, b):
    if a.hashes is None or b.hashes is None:
        return "a run failed"
    for i, (x, y) in enumerate(zip(a.hashes, b.hashes)):
        if x != y:
            return "first differs %d frames after frame %d" % (i + 1, START_FRAME)
    return "identical"

def same(a, b):
    return a.hashes is not None and a.hashes == b.hashes

def audio_matches(a, b):
    sa, sb = a.samples(), b.samples()
    n = min(len(sa), len(sb))
    # Both record from boot, and the samples depend only on emulated time, so
    # the shorter recording must be the start of the longer. At least a second.
    return n >= 48000 * 4 and sa[:n] == sb[:n], n // 4


DUMMY = {"SDL_AUDIODRIVER": "dummy"}
DET   = ["-deterministic", "-seed", "1"]

print("Determinism (%d frames from frame %d: a hostfs LOAD, then PCM)" % (FRAMES, START_FRAME))

a = Run("warp, dummy audio", DET + ["-warp"], DUMMY)
case("hostfs LOAD read the file into RAM", a.loaded == PATTERN)

b = Run("warp, dummy audio, again", DET + ["-warp"], DUMMY)
case("replay: two runs give the same hash every frame (%s)" % first_difference(a, b), same(a, b))
ok, n = audio_matches(a, b)
case("replay: and the same audio output (%d samples compared)" % n, ok)

c = Run("warp, no audio device", DET + ["-warp", "-sound", "none"])
case("audio: with and without a device the hashes match (%s)" % first_difference(a, c), same(a, c))
ok, n = audio_matches(a, c)
case("audio: and so does the audio output (%d samples compared)" % n, ok)

# Needs a sound card, so it is opt-in: X16_TEST_REAL_AUDIO=1.
if os.environ.get("X16_TEST_REAL_AUDIO"):
    r = Run("warp, the host's audio device", DET + ["-warp"])
    case("audio: the host's device and the dummy driver match (%s)" % first_difference(a, r), same(a, r))

w = Run("no warp, dummy audio", DET, DUMMY)
case("warp: with and without -warp the hashes match (%s)" % first_difference(a, w), same(a, w))
ok, n = audio_matches(a, w)
case("warp: and so does the audio output (%d samples compared)" % n, ok)

# Each fix has to be able to fail: without -deterministic, the same pair differs.
h = Run("warp, dummy audio, -seed only", ["-seed", "1", "-warp"], DUMMY)
case("  without -deterministic, a hostfs LOAD charges host time (%s)" % first_difference(a, h),
     a.hashes is not None and h.hashes is not None and a.hashes != h.hashes)

a0 = Run("warp, dummy audio, no hostfs", DET + ["-warp"], DUMMY, hostfs=False)
e  = Run("warp, no audio device, -seed only, no hostfs", ["-seed", "1", "-warp", "-sound", "none"],
         hostfs=False)
case("  without -deterministic, no audio device means a different machine (%s)" % first_difference(a0, e),
     a0.hashes is not None and e.hashes is not None and a0.hashes != e.hashes)

shutil.rmtree(WORK, ignore_errors=True)
print("\n%d passed, %d failed" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
