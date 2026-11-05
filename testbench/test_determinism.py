"""Determinism: the same build, seed and joystick input give the same machine.

    python testbench/test_determinism.py <x16emu> <rom.bin> [first-port]

Launches the emulator several times, each on its own debug port, and drives
every run identically over DAP with x16/step: pause, step to a fixed absolute
frame, load a small program that keeps the PCM FIFO busy, then step frame by
frame with a fixed joystick pattern, recording the machine state hash after
each frame. Runs that must agree are compared hash for hash.

The runs are aligned by absolute frame number, not by when the pause landed:
each process runs freely for a moment before it is paused, a different number
of frames each time, and stepping on to the same frame lines them up again.
"""
import base64, json, os, socket, subprocess, sys, time

EXE  = sys.argv[1]
ROM  = sys.argv[2]
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 9311

START_FRAME = 1200   # well past boot; every run is stepped to here first
FRAMES      = 300    # then this many single-frame steps, hashed

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

    def wait_stopped(self, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            if self.req("x16/status").get("body", {}).get("paused"):
                return True
            time.sleep(0.02)
        return False


# Keeps the PCM FIFO busy: whether it drains, and when, depends on the sound
# chips being clocked. A count of "FIFO full" sightings lands in $21.
PROGRAM = [
    0xA9, 0x8F,             # 0400 LDA #$8F     reset FIFO, volume 15, 8-bit mono
    0x8D, 0x3B, 0x9F,       # 0402 STA $9F3B
    0xA9, 0x20,             # 0405 LDA #$20
    0x8D, 0x3C, 0x9F,       # 0407 STA $9F3C    sample rate
    0xAD, 0x3B, 0x9F,       # 040A LDA $9F3B    loop: FIFO full?
    0x30, 0x0A,             # 040D BMI $0419
    0xE6, 0x20,             # 040F INC $20
    0xA5, 0x20,             # 0411 LDA $20
    0x8D, 0x3D, 0x9F,       # 0413 STA $9F3D    feed the FIFO
    0x4C, 0x0A, 0x04,       # 0416 JMP $040A
    0xE6, 0x21,             # 0419 INC $21      count full sightings
    0x4C, 0x0A, 0x04,       # 041B JMP $040A
]

BUTTONS = ["right", "left", "a", "b", "up", "down", "start", "select"]

def joysticks_for(k):
    held = [b for i, b in enumerate(BUTTONS) if (k >> (i % 4)) & 1 and (k // 7 + i) % 3 == 0]
    return [{"index": 0, "buttons": held}]


def run(label, extra_args, env_extra=None):
    """Launch, align, drive. Returns the per-frame hashes, or None on failure."""
    global PORT
    PORT += 1
    env = dict(os.environ)
    env.update(env_extra or {})
    cmd = [EXE, "-rom", ROM, "-debugport", str(PORT), "-hidden"] + extra_args
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        d = Dap(PORT)
        d.req("pause", {"threadId": 1})
        if not d.wait_stopped():
            print("    %s: never paused" % label)
            return None
        st = d.req("x16/status").get("body", {})
        f0 = int(st.get("frame", 0))
        if f0 >= START_FRAME:
            print("    %s: paused at frame %d, past the start frame %d" % (label, f0, START_FRAME))
            return None
        r = d.req("x16/step", {"frames": START_FRAME - f0}, timeout=120)
        if r.get("body", {}).get("frame") != START_FRAME:
            print("    %s: did not reach frame %d (%s)" % (label, START_FRAME, r))
            return None

        d.req("writeMemory", {"memoryReference": "0x0400",
                              "data": base64.b64encode(bytes(PROGRAM)).decode()})
        d.req("setVariable", {"variablesReference": 1, "name": "PC", "value": "$0400"})

        hashes = []
        for k in range(FRAMES):
            r = d.req("x16/step", {"frames": 1, "joysticks": joysticks_for(k), "hash": True})
            hashes.append(r.get("body", {}).get("hash"))
        full = d.req("readMemory", {"memoryReference": "0x0021", "count": 1})
        sightings = base64.b64decode(full.get("body", {}).get("data", ""))[:1]
        print("    %s: frames %d..%d, last hash %s, FIFO-full count %s"
              % (label, START_FRAME + 1, START_FRAME + FRAMES, hashes[-1],
                 sightings[0] if sightings else "?"))
        d.req("disconnect", {"terminateDebuggee": True})
        return hashes
    finally:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def first_difference(a, b):
    if a is None or b is None:
        return "a run failed"
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return "first differs at frame %d" % (START_FRAME + 1 + i)
    return "identical"


DUMMY = {"SDL_AUDIODRIVER": "dummy"}
DET   = ["-deterministic", "-seed", "1"]

print("Determinism (%d frames from frame %d)" % (FRAMES, START_FRAME))

a = run("warp, dummy audio", DET + ["-warp"], DUMMY)
b = run("warp, dummy audio, again", DET + ["-warp"], DUMMY)
case("replay: two runs give the same hash every frame (%s)" % first_difference(a, b),
     a is not None and a == b)

c = run("warp, no audio device", DET + ["-warp", "-sound", "none"])
case("audio: with and without a device the hashes match (%s)" % first_difference(a, c),
     a is not None and a == c)

# The audio case has to be able to fail. Without -deterministic and with no
# device, the sound chips are not clocked, the FIFO never drains, and the
# program sees a different machine.
e = run("warp, no audio device, -seed only", ["-seed", "1", "-warp", "-sound", "none"])
case("  ...and without -deterministic the same pair differs (%s)" % first_difference(a, e),
     a is not None and e is not None and a != e)

# Needs a sound card, so it is opt-in: X16_TEST_REAL_AUDIO=1.
if os.environ.get("X16_TEST_REAL_AUDIO"):
    r = run("warp, the host's audio device", DET + ["-warp"])
    case("audio: the host's device and the dummy driver match (%s)" % first_difference(a, r),
         a is not None and a == r)

w = run("no warp, dummy audio", DET, DUMMY)
case("warp: with and without -warp the hashes match (%s)" % first_difference(a, w),
     a is not None and a == w)

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
