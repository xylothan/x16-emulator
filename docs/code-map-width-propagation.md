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
| `REP #imm` | yes, exactly | the bits to clear are in the operand |
| `SEP #imm` | yes, exactly | the bits to set are in the operand |
| `XCE`      | yes, exactly | swaps carry and emulation, both tracked |
| `PLP`      | **no** | restores a status byte pulled off the stack |
| `RTI`      | **no** | restores the status the interrupt pushed |

For `PLP` and `RTI` the file leaves the running estimate untouched. Operand
widths for lines decoded after one of them can therefore be wrong, and with
them the instruction boundaries that follow.

## Why it is not fixed

The value is not recoverable from the instruction stream. What `PLP` restores
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

- the machine is a 65C816 in native mode (in emulation mode the widths are
  forced to 8-bit and are always right);
- the code in question has never executed (once it runs, `code_map_record()`
  captures the real effective status at every instruction, which is ground
  truth and outranks the estimate);
- a `PLP` or `RTI` sits between the last recorded anchor and the line being
  drawn.

That last point is the important one: the mechanism that would recover the value
is the same mechanism that makes the gap irrelevant. Code that has executed has
anchors; code that has not has no interrupt frame to consult either.

Every line this can affect already reports `recorded == false`, so a UI can say
so, and the next anchor re-establishes both the boundary and the width — the
decode is not allowed to swallow a recorded start, so a wrong guess cannot run
on past the next piece of hard evidence.

## What is pinned by tests

`tests/test_code_map.c` covers the bound rather than the missing value, for both
instructions:

- `PLP`: "reports a line decoded from a guessed width as unrecorded",
  "re-syncs onto the next anchor after a bad width guess", "recovers the real
  width from the anchor it landed on".
- `RTI`: "leaves the width estimate alone across RTI instead of guessing",
  "reports a line decoded across RTI as a guess, not as known", "reaches the
  anchor after RTI and recovers the real width".

The first `RTI` check is deliberately written to fail if someone substitutes a
plausible width without also building the known/unknown machinery. It is a
tripwire, not a claim that the current behaviour is ideal — if the widths are
ever tracked properly, that check should be replaced, not deleted quietly.
