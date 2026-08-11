# code_map: where the 65C816 operand widths are still a guess

Status: **known limitation, deliberately not "fixed"**. This note exists so the
next person to look at it does not mistake it for an oversight, and so that the
tempting fix is on record as the wrong one.

## The gap

`cm_propagate()` in `src/code_map.c` predicts the effective processor status of
the next instruction from the current opcode. Exactly five instructions change
the 65C816 register widths:

| Instruction | Modelled? | Why |
|---|---|---|
| `REP #imm` | its status bits, exactly | the bits to clear are in the operand |
| `SEP #imm` | its status bits, exactly | the bits to set are in the operand |
| `XCE`      | only if C and E are right | swaps carry and the emulation flag |
| `PLP`      | **no** | restores a status byte pulled off the stack |
| `RTI`      | **no** | restores the status the interrupt pushed |

`XCE` is exact only when **both** of its inputs are right, and neither is
guaranteed.

*The carry* is not tracked through any data-dependent change. `CLC` and `SEC`
are modelled, and `REP`/`SEP`/`XCE` naturally update it as part of what they do,
but `ADC`, `SBC`, the compares, and the shifts and rotates all move the carry
without the estimate noticing. So an `LSR A` that really clears the carry,
followed by `XCE`, makes the model predict a switch into emulation mode that
does not happen, forcing the widths to 8-bit for every following line that has
no anchor of its own. That needs no `PLP` or `RTI` at all;
`tests/test_code_map.c` pins it under `KNOWN LIMITATION`.

*The emulation flag* is not recorded by anchors at all — they store the
effective status byte but **not** `E`. The running `E` is seeded from the live
`regs.e`, so landing on an anchor re-syncs that line's widths but not `E`, and
propagating past the anchor folds the stale `E` back in
(`if (e) status |= INDEX|MEMORY`) — which can mis-size the next unanchored line,
where the stale `E` and the real one imply different widths. This is why
recovery from a bad `E` is anchor-local rather than persistent, and it is pinned
by a test.

Most X16 software sets `E` once during startup and leaves it alone, which is why
this is latent rather than routine. What defeats that reassurance is not a
deliberate mode switch by the guest: it is enough that the walk *crosses* an
`XCE` — the one from startup, one on a path the program never takes, or a `$FB`
byte that is really data — with a carry the estimate did not track. `run_e` is
written in exactly one place, `case 0xFB`, so without an `XCE` in the walked
stream the estimate keeps the seeded `E`.

For `PLP` and `RTI` the file leaves the running estimate untouched. Operand
widths for lines decoded after one of them can therefore be wrong, and with
them the instruction boundaries that follow.

## Why it is not fixed

The value is not recoverable by this analysis, which sees only the instruction
stream. What `PLP` restores
depends on what was pushed, which depends on the path taken to get there; what
`RTI` restores is the status at the moment an interrupt was taken, which is not
in the code at all.

Two avenues were considered and rejected:

- **Walk back from `PLP` to the matching `PHP`.** Needs the routine boundaries
  and a guarantee that no other stack traffic intervened. Neither is available,
  and a wrong pairing produces a confident wrong width rather than an admitted
  unknown.
- **Record the status pushed at each interrupt and consult it for `RTI`.** The
  emulator does see every interrupt, and `src/cpu/irq_ctx.c` already tracks
  entries and exits — but it records the vector, the interrupted PC and the
  stack pointer, not the pushed status. Adding it is easy; *using* it is not.
  `cm_propagate()` runs over arbitrary addresses while drawing a disassembly
  window, and has no way to know which interrupt frame a particular `RTI` on
  screen will return through. The innermost live frame is not necessarily the
  right one. So the result would still be a guess, just a more expensive one.

Both need a notion of "known vs unknown" per status bit, so that a failed
attempt reports uncertainty instead of substituting another guess. That does not
exist, and adding it is a larger change than the defect warrants.

## Why the gap is bounded

It costs nothing unless *all* of these hold at once:

- the machine is a 65C816 (on the 65C02 operand widths never vary, so the
  estimate cannot change a length);
- the line being drawn has no currently believed anchor — usually because that
  code has never executed, but also if its anchor was evicted with its bank
  context (the cache is capped, see `CM_MAX_CONTEXTS`) or is no longer believed
  because the opcode byte under it changed;
- the status the estimate carries into that line is not the status the code
  there really runs with.

That third condition is the general one, and it is *not* only the unmodelled
instructions. It happens whenever:

- one of the unmodelled cases lies between the last anchor and the line — a
  `PLP`, an `RTI`, a stale carry feeding an `XCE`, or a stale `E`;
- **the walk crossed a branch or a call.** Propagation is address-linear: it
  applies whatever opcode sits at the next *address*. Given
  `BRA +2 / REP #$20 / LDA #`, execution jumps over the `REP` and the `LDA` is
  two bytes, while the walk applies the `REP` and sizes it three. The same goes
  for a `JSR` whose callee changes `M`/`X` and returns. `REP` and `SEP` are
  applied exactly *as instructions*; that does not make the resulting estimate
  right for a path the program never took;
- **the walk started somewhere the seed does not describe.**
  `code_map_disasm_forward` seeds from `regs.status`/`regs.e`, which is the
  state at the *current PC*, not at `start`. That is the DAP `disassemble`
  path, which takes an arbitrary address, so disassembling never-executed code
  that runs at a different width is wrong from the first line — with no anchor
  and no unmodelled instruction anywhere in sight.

Note what is **not** on that list: native mode. The real CPU in emulation mode
does force 8-bit widths and is always right — but the fold-in here
(`if (e) status |= INDEX|MEMORY`) uses the *estimated* `E`, not the machine's,
and nothing re-seeds or repairs that estimate mid-walk. `run_e` is seeded once
per forward run and thereafter written only by `XCE`, from the estimate's own
carry; an anchor overrides the status but never the `E`. Once the estimate
diverges it will size operands as though native while the machine is really in
emulation. A test pins exactly that.

The mechanism that would recover the value is largely the same mechanism that
makes the gap irrelevant: code that has executed has anchors. The correspondence
is not perfect — a handler paused before its own, not-yet-executed `RTI` does
have a live interrupt frame — but associating a particular on-screen `RTI` with
a particular frame is exactly the part that is not solved.

Every line this can affect already reports `recorded == false`, so a UI can say
so. **The strength of the recovery differs by which input was wrong**, and none
of it is unconditional:

- a wrong **status** estimate is corrected once an accurate, believed anchor is
  reached, which supplies both the boundary and the width and then carries
  forward. Not simply "the next recorded anchor": a same-opcode stale anchor is
  still believed and hands back its *old* status, and there is no guarantee an
  accurate anchor exists ahead at all;
- a wrong **E** is weaker still. Anchors store the effective status byte but not
  `E`, so an anchor fixes the width of its own line and propagation past it
  folds the stale `E` straight back in, mis-sizing the next unanchored line
  again. Recovery from a bad `E` is anchor-local, not persistent.

In both cases the decode is not allowed to swallow a recorded start (unless it
is itself recorded — see the overlap policy in `cm_fill`), so a wrong guess
cannot silently run past hard evidence about *boundaries*, even where it keeps
getting *widths* wrong.

## What is pinned by tests

`tests/test_code_map.c` covers the bound rather than the missing value, for all
three cases:

- `PLP`: "reports a line decoded from a guessed width as unrecorded",
  "re-syncs onto the next anchor after a bad width guess", "recovers the real
  width from the anchor it landed on".
- `RTI`: "leaves the width estimate alone across RTI instead of guessing",
  "reports a line decoded across RTI as a guess, not as known", "reaches the
  anchor after RTI and recovers the real width".
- `XCE` with a stale carry: "KNOWN LIMITATION: a stale carry into XCE
  mispredicts the widths", "an anchor recovers the width of its own line after a
  bad XCE", and — pinning the *weaker* bound described above — "KNOWN
  LIMITATION: past that anchor a stale E mis-sizes again".

The first `RTI` check is deliberately written to fail if someone substitutes a
plausible width without also building the known/unknown machinery. It is a
tripwire, not a claim that the current behaviour is ideal — if the widths are
ever tracked properly, that check should be replaced, not deleted quietly.

---

# Related: a stale anchor that kept its opcode byte

**Known limitation, pinned by tests, not fixed.**

An anchor is believed while the opcode byte it recorded is still in memory. That
one-byte check catches overlay loads, a second program over the first, and
self-modifying code without needing a hook on every write — but replacement code
that happens to repeat the same opcode byte at the same address keeps the old
anchor, and with it the old recorded **status**.

On a 65C816 that stale status can imply a different operand width, so the line
decodes wider than the new code really is. Because a recorded line is exempt
from the interior-anchor clamp (see the overlap policy in `cm_fill`), it can
then swallow a genuinely fresh anchor inside it:

```
old: $8000  A9 xx xx   LDA #$xxxx   recorded with 16-bit A -> 3 bytes
new: $8000  A9 xx      LDA #$xx     8-bit A, and $8002 freshly executed
```

The opcode is still `$A9`, so the stale anchor survives and wins.

**It cuts both ways.** The example above decodes too *wide* and swallows a fresh
anchor, which is the loud failure. The opposite is quieter and just as
reachable: an anchor recorded when the accumulator was 8 bits sizes its `LDA #`
at two bytes, so if the replacement code runs with a 16-bit accumulator the row
stops a byte early and the *next* row starts inside the real instruction's
operand. Nothing is swallowed and the rows still tile perfectly, so there is no
visible seam — the disassembly is simply misaligned from there until an accurate
anchor is reached. Both directions are pinned by tests.

Why it is accepted rather than fixed:

- It needs the replacement code to repeat the same opcode byte at the same
  address, which the one-byte check then cannot see through.
- The damage is self-correcting *if* an accurate anchor is reached that this
  line — or another stale one — has not swallowed. That is not guaranteed: right
  after an overlay load the new code typically has no valid anchors at all, and
  the stale status then propagates to the end of the window. (An anchor whose
  opcode no longer matches re-syncs nothing; it is rejected by `cm_anchor_ok()`
  and simply stops being evidence.)
- The alternative — always clamping — costs the far more common case, the
  `.byte $2C` skip idiom, where two overlapping starts are both genuinely real
  and clamping renders a whole known instruction as a fragment.

**Scope: the wrong-*width* flavour is 65C816-only; the swallowing is not.** On a
65C02 operand widths never vary, so a stale anchor cannot decode wider than the
new code. But it does not have to: a stale anchor whose opcode byte survived is
still believed, is still exempt from the interior-anchor clamp, and can still
cover a fresh start inside it. An old `BIT abs` at `$8000` overlaid by new code
that also begins `$2C` will swallow a genuine instruction start at `$8001` on
any CPU. `tests/test_code_map.c` covers both flavours.

**The stale case is not distinguishable from a legitimate one.** The recorded
state above is byte-for-byte identical to a routine that genuinely executed at
two different accumulator widths — an anchor at `$8000` with a 16-bit status and
an anchor at `$8002` with an 8-bit one, over memory starting `$A9`. In that
benign case emitting the 3-byte `LDA` is exactly correct.

That rules out the obvious fix. A per-address recording *epoch* ("prefer the
newer anchor") does not resolve this: applied to the benign case it would clamp
a legitimately recorded 3-byte `LDA` back to 2 bytes purely because the other
width ran more recently, reintroducing the defect this branch fixed. It trades
one wrong answer for another.

Widening the staleness check — storing two or more bytes per anchor instead of
one — is also not the clean win it looks like. It would catch *this* example
(the operand bytes differ), but it is the same class of heuristic, so it only
narrows the window rather than closing it; replacement code with identical bytes
stays invisible either way. It also misfires on one-byte instructions, which are
extremely common (`NOP`, `INX`, `RTS`, …): for those the second byte compared is
not part of the instruction at all but the *next* instruction's opcode, so
changing the following instruction would invalidate a perfectly good anchor.
Storing the instruction's real length would avoid that — it does not need a full
`disasm()` call, an opcode length table plus the small M/X-dependent set would
do — but it is per-instruction classification plus another byte of storage per
address, and it still does not close the identical-bytes hole.

What does resolve it is invalidation keyed on the **write** that replaced the
code, because that is the event which actually differs between the two cases.
That does not necessarily mean a hook on every CPU write: the dominant source of
stale anchors is a host-side load, and those already funnel through the KERNAL
LOAD/MACPTR interception in `main.c` (the bytes themselves land in `ieee.c`), so
even a conservative `code_map_reset()` when an intercepted load completes would
be sound for that case and needs no range or bank bookkeeping. Targeted range
invalidation keeps more coverage but is more work. Self-modifying code and
in-guest decompressors would still need the general write path in `memory.c`.
Either way it is a change to the recording side rather than the disassembly
side.

(It would *not* help the PLP/RTI width gap described earlier in this document.
That one is about status bytes restored from the stack, which is unrelated to
code being replaced.)

`tests/test_code_map.c` pins the current behaviour under checks labelled
`KNOWN LIMITATION`, so it is visible rather than folklore. If it is ever fixed
those checks will fail — update them, do not delete them.

One caveat on that tripwire: it fires for a fix *inside* code_map (a wider
staleness check, a stored instruction length, an epoch), but **not** for the
write-invalidation fix recommended above. The unit test models the overlay with
its own `poke()`, which writes the harness's memory arrays directly, and
code_map exposes no write-notification entry point for it to call. Anyone
adding one should route `poke()` through it, or these checks will stay green
while this note quietly becomes a description of behaviour that no longer
exists.

---

# Related: a window-straddling instruction is decoded through one bank

**Not fixed. Not covered by a passing test.** Found while adding banking
coverage for `cm_x16bank_for()`.

`cm_fill()` reads the raw bytes of an instruction one at a time, each through
the window backing that byte's own address, so an instruction straddling the
`$BFFF`/`$C000` boundary shows the right bytes. `cm_decode()` does not: it hands
`disasm()` a *single* `x16Bank`, derived from the instruction's **start**
address, and `disasm()` uses that one bank for every operand read.

So for a `STA $1234` whose opcode is at `$BFFF` in RAM bank 5 and whose operand
lives at `$C000`/`$C001` in ROM bank 2, the operand is read as *ROM bank 5*.
With the test fixture that means `bytes[]` correctly reads `8D 34 12` while
`text` reads `sta $eaea` and `eff_addr` is `$EAEA`. This is a *debugger-side*
mis-read only: the CPU's own fetch goes through the real banking hardware and
is correct, so the program runs fine — it is the disassembly shown to the user
that is wrong. The same wrong bytes come back from the host-side read on a real
machine's memory image (`ROM[rambank * 16384 + …]`; for a bank number ≥ 32 it is
open bus, or the cartridge's bytes when one supplies that bank).

Note the width propagation had the *same* bug and **is** fixed: `cm_propagate()`
now reads a REP/SEP operand through the window backing the operand's own
address. That one is code_map's own read, so fixing it touched nothing shared.

The test in `tests/test_code_map.c` asserts the displayed **bytes** only, and
says so in a comment; it does not assert the text, because the text is wrong.

Fixing it properly means giving `disasm()` a per-byte, window-aware reader (or
having `cm_decode()` detect a straddle and re-render the operand from the bytes
`cm_fill()` already read correctly). `disasm()` is shared with the classic SDL
debugger and the DAP server, so that is a wider change than the code_map
defects this work covers.

Scope: one instruction per window boundary, and only when code is laid across
one. There are two such boundaries, not one — `$9FFF`/`$A000` as well as
`$BFFF`/`$C000` — since an opcode below `$A000` follows the live bank while its
operand in the RAM window needs the caller's RAM bank. Rare, but it renders as
confidently wrong text rather than as an admitted unknown, which is the worst
failure mode this file has.
