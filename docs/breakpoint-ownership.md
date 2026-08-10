# Breakpoint ownership in `debug_core`

## What this document is

The DAP server (`src/debug_server.c`) keeps re-implementing an ownership model
that belongs in `src/debug_core.c`. It has been rewritten four times across
eight review rounds, and each rewrite fixed the reported defect while
introducing a new one in the same area. This describes the underlying gap, the
evidence that it is a single gap rather than a series of unrelated bugs, and a
design for closing it.

Everything described here as "current behaviour" is on the `dap-server` branch
at the time of writing. The DAP PR ships **with** this limitation, noted in
`src/debug_server.h`; this is the follow-up work.

## The gap, in one sentence

`debug_core` stores breakpoints in a table keyed on `(pc, bank, x16Bank)` that
**deduplicates on add and deletes on remove**, with no record of who asked for
an entry — so when several independent things want a breakpoint at the same
address, the first `remove` disarms it for all of them.

## Who the owners are

Five, and they are genuinely independent:

| Owner | Adds via | Notes |
| --- | --- | --- |
| `-bp` / `-wp` command line | `debug_bp_add` in `main.c` | present before any client connects |
| SDL debugger F9 | `debug_bp_toggle` in `debugger.c` | user at the keyboard, may toggle at any time |
| DAP source breakpoints | `dap_bps[]` | replaced wholesale per source file |
| DAP function breakpoints | `func_bp_addrs[]` | replaced wholesale per `setFunctionBreakpoints` |
| DAP instruction breakpoints | `instr_bp_addrs[]` | replaced wholesale per `setInstructionBreakpoints` |

The debugger's own step-over/step-out target is a sixth, but it deliberately
lives in `stepBreakPoint` rather than the shared table — which is exactly the
workaround this document proposes generalising.

Two more things are keyed the same way and share the same problem:

- **Condition records** (`cond_ensure` / `cond_find` in `debug_core.c`) are keyed
  on the same triple, so two owners at one address share one condition and one
  hit count. A condition set on a DAP source breakpoint currently also gates a
  function breakpoint at that address.
- **Watchpoints** (`debug_wp_*`) have the same shape, with `-wp`, the SDL
  debugger and DAP data breakpoints as owners.

## Why it keeps coming back

The server cannot answer "is this entry mine?" from outside the core, and every
attempt to approximate it has failed differently. The full sequence, because the
pattern is the point:

1. **Round 4 — no ownership at all.** Teardown removed every address the server
   had recorded. Deleted `-bp` breakpoints and F9 breakpoints outright.
2. **Round 4 fix — `server_bp_wanted_elsewhere()`.** A cross-table refcount so
   the three DAP tables stop deleting each other's shared entries. Correct as
   far as it goes, and still in place. Does not see `-bp` or F9 at all.
3. **Round 5 — `owned` flag**, set from `debug_bp_add(...) >= 0`. Intended to
   mean "we created it". **Broke the refcount**: when the table that created an
   entry is cleared it declines to remove because a survivor still wants it,
   which transfers ownership to a survivor whose `owned` is false; that survivor
   then also declines, and the entry is orphaned — armed, with no table naming
   it, unreachable by any request.
4. **Round 6 — `external` flag**, probed with
   `debug_bp_find(...) >= 0 && !server_bp_wanted_elsewhere(...)`. Intended to
   mean "someone outside the server owns it". **The predicate cannot express
   that**: `debug_bp_find` says yes both for the user's breakpoint and for a
   sibling DAP table's, so only the first table at an address recorded it
   correctly and the second recorded the user's breakpoint as its own.
5. **Round 6 fix — `ext_keys[]` registry.** Decide once per address, inherited
   by later tables. **Went stale in both directions**: nothing invalidated a
   record when the entry was removed, so a client set/clear followed by a user
   F9 at the same address left the cache saying "server's" and teardown deleted
   the user's breakpoint; and a cached "external" survived the user deleting
   their own breakpoint, leaving a server-created entry armed and orphaned.
6. **Round 7 — derive from the add result, forget on removal.** Closer. **The
   forget calls all sat inside `!external` guards**, so records saying "the
   user's" were never dropped and the stale-external direction was still live.
7. **Round 8 — unguarded forget, plus teardown clearing `verified` in the loop**
   (two entries at one address were vetoing each other's removal).

That is four rewrites of the same ~40 lines. Each was a correct response to the
reported defect; none could be right, because the information needed to answer
the question is not available outside the core.

## Current workaround, and what it costs

`src/debug_server.c` currently carries, purely for this:

- `ext_keys[]` / `ext_key_find` / `ext_key_note_after_add` / `ext_key_forget` /
  `ext_key_reset` — a per-address ownership registry (~50 lines).
- `server_bp_wanted_elsewhere(addr, bank, x16Bank, skip_table, skip_index)` — a
  cross-table refcount over the three DAP tables (~25 lines).
- `external` on `dap_bps[]`, plus `func_bp_external[]` and
  `instr_bp_external[]`.
- `dap_wp_addrs[]` / `dap_wp_console[]` — the same idea again for watchpoints.
- `dap_owns_pending_step` — the same idea again for the step target.
- Guards of the form `verified && !external && !server_bp_wanted_elsewhere(...)`
  duplicated at seven removal sites, each of which has been wrong at least once.

None of it is visible to `-bp`, F9 or anything else that may exist later.

## Proposed fix

Move ownership into `debug_core`, as a refcount plus an owner tag.

### API sketch

```c
// Who asked for a breakpoint. Extend freely; the core only compares them.
typedef enum {
    DEBUG_OWNER_CLI = 0,      // -bp / -wp
    DEBUG_OWNER_UI,           // SDL debugger F9
    DEBUG_OWNER_DAP_SOURCE,
    DEBUG_OWNER_DAP_FUNCTION,
    DEBUG_OWNER_DAP_INSTRUCTION,
    DEBUG_OWNER_DAP_CONSOLE,
    DEBUG_OWNER_STEP,         // the debugger's own step target
    DEBUG_OWNER_COUNT
} debug_owner_t;

// Add a reference for `owner`. Adding one that already exists for the SAME
// owner is idempotent. Returns the entry index, or -1 if the table is full.
int  debug_bp_add_for(struct breakpoint bp, debug_owner_t owner);

// Drop `owner`'s reference. The entry itself only goes when the last owner
// does. Returns true if this owner had a reference.
bool debug_bp_remove_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);

// Drop every reference held by `owner` -- the operation each DAP "set the
// complete list" request wants, and what session teardown wants.
void debug_bp_clear_owner(debug_owner_t owner);

// Does `owner` hold a reference here? For UIs that mark their own breakpoints.
bool debug_bp_has_owner(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);
```

Internally a `uint16_t owners` bitmask on `struct breakpoint` is enough for
seven owners and keeps the hot path (`debug_bp_on_arrival`) unchanged — it never
looks at owners at all.

### Conditions

Conditions should stay keyed on the address triple, **not** per owner. Two
owners wanting different conditions at one address is a genuine conflict with no
correct answer, and per-owner conditions would mean per-owner arrival
evaluation, which is a much bigger change to the hot path. Document that the
last writer wins, and have `debug_bp_clear_owner` drop the condition record only
when the last owner goes (which `cond_forget` already does on full removal).

### Watchpoints

Same treatment, same enum: `debug_wp_add_for` / `debug_wp_remove_for` /
`debug_wp_clear_owner`. This deletes `dap_wp_addrs[]` and `dap_wp_console[]`.

### The step target

`stepBreakPoint` can stay where it is — it is already correctly isolated. But
`DEBUG_OWNER_STEP` exists in the enum so that if it ever moves into the shared
table it does not reintroduce this problem. `dap_owns_pending_step` should be
replaced by asking the debugger who owns the pending step, rather than the
server tracking it.

## What this deletes

From `src/debug_server.c`: `ext_keys[]` and its four functions,
`server_bp_wanted_elsewhere`, the `external` field and both parallel arrays,
`dap_wp_addrs[]`, `dap_wp_console[]`, `dap_owns_pending_step`, and the guard
expressions at all seven removal sites. Roughly 150 lines, and with them every
defect listed in "Why it keeps coming back".

`handle_dap_set_breakpoints` becomes:

```c
debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);   // replaces the whole set
// ... then for each requested breakpoint:
debug_bp_add_for(bp, DEBUG_OWNER_DAP_SOURCE);
```

and `dap_release_session_state()` becomes four `debug_bp_clear_owner` /
`debug_wp_clear_owner` calls.

## Tests this needs

`tests/test_debug_core.c` covers add/remove/dedup today. Ownership needs its own
cases, and every one of these corresponds to a defect actually found:

1. Two owners add the same address; the first removes; the entry stays armed and
   still fires. *(Round 4.)*
2. …then the second removes; the entry goes. *(Round 5 — the orphan.)*
3. `-bp` adds; DAP adds the same address; DAP clears its owner; the `-bp`
   breakpoint survives with its condition and hit count intact. *(Round 5/6.)*
4. DAP adds; DAP removes; F9 adds at the same address; DAP adds again; DAP
   session teardown; the F9 breakpoint survives. *(Round 7 — stale registry,
   direction 1.)*
5. `-bp` adds; DAP adds; DAP removes; user deletes the `-bp`; DAP adds again;
   teardown removes it. *(Round 7 — stale registry, direction 2.)*
6. Two DAP source entries at one address (two paths with the same basename);
   teardown removes the entry exactly once and leaves nothing armed.
   *(Round 8 — the mutual veto.)*
7. `debug_bp_clear_owner` on an owner holding no references is a no-op.
8. The same for watchpoints, with `-wp` as the surviving owner.

Each should be mutation-tested: revert the ownership check and confirm a *named*
check fails. Several fixtures during this work looked like coverage and proved
nothing, so a test that has not been seen to fail should not be trusted.

## Notes for whoever picks this up

- `normalise_bank()` is already exported as `debug_normalise_bank()`. Any
  ownership key must be normalised the same way, or it will not match the entry
  the core created — that was its own defect, on gen2 with a non-zero program
  bank, where an address above `$A000` is *not* banked.
- `debug_bp_add` currently returns -1 both for "already exists" and "table
  full". `debug_bp_add_for` should distinguish them; the server has been reading
  -1 as "already exists" and would silently mis-handle a full table.
- The DAP server's three tables can then become one list of
  `(owner, addr, bank, x16Bank)`, or disappear entirely in favour of asking the
  core — worth deciding early, since it changes how much of
  `handle_dap_set_*_breakpoints` survives.
- `testbench/test_dap.py` exercises none of the ownership sequences. It is an
  integration harness against a live emulator and is the right place for at
  least case 3, which needs `-bp` on the command line.
- The failure that matters most is not a wrong answer to a client. It is an
  entry left armed that no owner names: on a headless `-debugport` run with no
  client attached, the machine halts and nothing can resume it.
