# Breakpoint ownership in `debug_core`

## What this document is

`debug_core` records which of several independent things asked for each
breakpoint and watchpoint, so that one of them letting go cannot disarm it for
the others. This describes the problem that made it necessary, the design, and
the decisions taken along the way — including the one deliberately *not* taken.

The history matters here more than it usually would: the DAP server
re-implemented this ownership model from outside the core four times across
eight review rounds, and each rewrite fixed the reported defect while
introducing a new one in the same forty lines. Reading only the final code, it
is not obvious why it is shaped this way.

## The gap that was closed

The breakpoint table was keyed on `(pc, bank, x16Bank)` and **deduplicated on
add, deleted on remove**, with no record of who asked for an entry — so when
several independent things wanted a breakpoint at the same address, the first
`remove` disarmed it for all of them.

The failure that mattered most was never a wrong answer to a client. It was an
entry left armed that no owner named: on a headless `-debugport` run with no
client attached, the machine halts and nothing can resume it.

## Who the owners are

Six, and they are genuinely independent:

| Owner | Adds via | Notes |
| --- | --- | --- |
| `DEBUG_OWNER_CLI` | `-bp` / `-wp` in `main.c` | present before any client connects |
| `DEBUG_OWNER_UI` | F9 in `debugger.c`, and the ImGui panels | user at the keyboard, may act at any time |
| `DEBUG_OWNER_DAP_SOURCE` | `setBreakpoints` | replaced wholesale per source file |
| `DEBUG_OWNER_DAP_FUNCTION` | `setFunctionBreakpoints` | replaced wholesale per request |
| `DEBUG_OWNER_DAP_INSTRUCTION` | `setInstructionBreakpoints` | replaced wholesale per request |
| `DEBUG_OWNER_DAP_CONSOLE` | `bp_add` / `watch_add` typed in the debug console | goes with the session |

`DEBUG_OWNER_STEP` exists for the debugger's own step-over/step-out target. That
target deliberately lives in `stepBreakPoint` rather than the shared table, but
it carries an owner for the same reason everything else does — see
[The step target](#the-step-target).

## Why it kept coming back

The server could not answer "is this entry mine?" from outside the core, and
every attempt to approximate it failed differently. The full sequence, because
the pattern is the point:

1. **Round 4 — no ownership at all.** Teardown removed every address the server
   had recorded. Deleted `-bp` breakpoints and F9 breakpoints outright.
2. **Round 4 fix — `server_bp_wanted_elsewhere()`.** A cross-table refcount so
   the three DAP tables stopped deleting each other's shared entries. Correct as
   far as it went. Did not see `-bp` or F9 at all.
3. **Round 5 — `owned` flag**, set from `debug_bp_add(...) >= 0`. Intended to
   mean "we created it". **Broke the refcount**: when the table that created an
   entry was cleared it declined to remove because a survivor still wanted it,
   which transferred ownership to a survivor whose `owned` was false; that
   survivor then also declined, and the entry was orphaned — armed, with no
   table naming it, unreachable by any request.
4. **Round 6 — `external` flag**, probed with
   `debug_bp_find(...) >= 0 && !server_bp_wanted_elsewhere(...)`. Intended to
   mean "someone outside the server owns it". **The predicate could not express
   that**: `debug_bp_find` said yes both for the user's breakpoint and for a
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
the question was not available outside the core.

The same gap produced a fifth reconstruction elsewhere: the ImGui breakpoints
panel kept `s_tracked`, a "panel-owned superset of `breakPoints[]`", because the
core could not represent a *disabled* breakpoint. Its own header noted the
consequence — a disabled breakpoint showed no gutter marker anywhere, because it
was simply absent from the table. That table is now gone too; see
[Enable and disable](#enable-and-disable).

## The design

### Ownership

`struct breakpoint` carries a `uint16_t owners` bitmask. Seven owners fit
comfortably, the set operations wanted are exactly bit operations, and the hot
path (`debug_bp_on_arrival`) never looks at it.

```c
debug_add_result_t debug_bp_add_for(struct breakpoint bp, debug_owner_t owner);
bool debug_bp_remove_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);
int  debug_bp_clear_owner(debug_owner_t owner);
bool debug_bp_delete(int pc, uint8_t bank, int x16Bank);
bool debug_bp_has_owner(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);
void debug_bp_toggle_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);
```

Two owners at one address share a single entry — there is only ever one place
the CPU can stop — and the entry survives until the last of them lets go.
Watchpoints get the same treatment: `debug_wp_add_for` and friends.

`debug_add_result_t` distinguishes `DEBUG_ADD_CREATED`, `DEBUG_ADD_EXISTED` and
`DEBUG_ADD_FULL`. The old entry point returned -1 for both "already exists" and
"table full", and the server read -1 as "already exists" — so a full table
silently armed nothing while reporting a verified breakpoint.

The unowned mutators (`debug_bp_add`, `debug_bp_remove`, `debug_bp_toggle`,
`debug_wp_add`, `debug_wp_remove`) are gone from the header. Leaving them
exposed is the footgun that caused this entire class of bug: any future caller
could disarm someone else's breakpoint in one line.

### The human's delete is authoritative

`debug_bp_delete` — F9, and `bp_remove` typed in the console — takes the
breakpoint away whoever asked for it. It is deliberately not a refcount
operation.

Under strict refcounting, F9 on a breakpoint a DAP client had set would drop
only the UI's own reference and leave the entry armed: the user would press the
key and watch nothing happen. That is worse than the bug being fixed. The
keyboard wins; the client's next `setBreakpoints` reconciles its own view.

For the same reason, the console's `bp_clear` and `watch_clear` clear
everything, `-bp` and F9 included. They are a person saying "all of them".

### Conditions

Conditions stay keyed on the address triple, **not** per owner. Two owners
wanting different conditions at one address is a genuine conflict with no
correct answer, and per-owner conditions would mean per-owner arrival
evaluation, on the hot path. DAP itself models only one condition per
breakpoint. The last writer wins.

Owner-scoped removal **preserves** the condition and hit count. An earlier draft
of this document had `debug_bp_clear_owner` drop the condition record when the
last owner went, "which `cond_forget` already does on full removal" — it does
not. `debug_bp_remove` deliberately left the condition behind, documented in
`debug_core.h` as how a UI implements an enable/disable toggle without losing
its count; the server only lost it because it called `debug_bp_forget`
explicitly at all eight removal sites.

That distinction matters more than it looks. An editor re-sends a source file's
entire breakpoint list on every edit, so `clear_owner` followed by re-add runs
constantly. Discarding hit counts there would make `hitCondition` unusable.

The consequence is a requirement on callers, met in `dap_apply_bp_condition()`:
**state the condition in full on every set, including clearing it.** A
breakpoint re-sent without a condition would otherwise inherit the one it had
before.

### Enable and disable

`struct breakpoint` carries `enabled`. A disabled breakpoint keeps its entry,
its owners, its condition and its hit count, and simply does not stop the
machine. `debug_bp_on_arrival` and `debug_bp_is_set` honour it, while
`debug_bp_at()` still shows it — so a UI can draw it greyed instead of losing
the marker entirely.

This is what retired the ImGui panel's `s_tracked`. That list is now a view of
the core's table, rebuilt each frame from `debug_bp_count()` / `debug_bp_at()`,
rather than a superset with a lifetime of its own — so a breakpoint disabled
from the panel still shows in the Disassembly and Source gutters.

Re-adding a disabled breakpoint re-arms it: asking for a breakpoint is asking
for it to be armed, and a client that set one, disabled it, and set it again
should not be silently ignored.

### The step target

`stepBreakPoint` stays in `debugger.c`, outside the shared table — it was
already correctly isolated. It now carries `stepOwner`, and the debugger answers
`DEBUGStepOwner()` / `DEBUGCancelStepFor(owner)`.

Previously the DAP server tracked this itself in `dap_owns_pending_step`, which
is the same mistake in miniature: state about the debugger's step, kept
somewhere other than the debugger. `DEBUGStepOver` and `DEBUGStepOut` now take
the owner directly, so there is no window in which a step is armed but unowned.

## What this deleted

From `src/debug_server.c`: `ext_keys[]` and its four functions,
`server_bp_wanted_elsewhere`, the `external` field and both parallel arrays,
`dap_wp_addrs[]`, `dap_wp_console[]`, `dap_clear_own_watchpoints`,
`dap_owns_pending_step`, and the guard expressions at all eight removal sites.

`handle_dap_set_function_breakpoints` and
`handle_dap_set_instruction_breakpoints` now begin with a single
`debug_bp_clear_owner(...)`, and `dap_release_session_state()` is six
`clear_owner` calls and one `DEBUGCancelStepFor`.

Two defects went with them that were not in the original list:

- Console `bp_remove` had no ownership guard at all, so it deleted the user's
  breakpoints outright.
- A full `dap_bps[]` table armed the breakpoint in the core anyway and did not
  record it, so it outlived the session that asked for it — the orphan case,
  reached without any ownership confusion at all.

From `src/debug_ui/panels/breakpoints_panel.cpp`: `s_tracked`'s independent
lifetime, `tracked_find()` and `active_bp_exists()`, and the delete-and-remember
implementation of enable/disable.

## One thing deliberately not done: bidirectional sync

The core is the single source of truth for what is **armed**. Each front end
still owns its own **view**, and those can diverge: a breakpoint set with F9
does not appear in a connected client's UI, and one deleted with F9 may be sent
again by the client.

DAP has a `breakpoint` event with `reason` `new` / `removed` that looks like the
answer. It was investigated and rejected:

- The specification imposes no obligation on clients. Its own wording is that
  "clients should continue to use the breakpoint's original properties when
  updating a source's breakpoints", and `setBreakpoints` "clears all previous
  breakpoints in that source" from the client's own model. The client is
  authoritative over the view by design.
- VS Code does in fact honour both `removed` and `new`. But that is one client's
  implementation rather than a contract, `removed` is silently dropped if the
  adapter never returned an `id`, and behaviour in other clients is unknown.
- Peer adapters do not attempt it. Neither cpptools/MIEngine nor CodeLLDB syncs
  console-created breakpoints back to the editor's UI.

Building on unguaranteed client behaviour is exactly how this area generated
eight review rounds. The divergence is documented for users instead — in
`README.md` and in `src/debug_server.h` — rather than papered over in the one
client where it happens to work.

## Tests

`tests/test_debug_core.c` covers ownership directly, and every case corresponds
to a defect actually found:

1. Two owners add one address; the first removes; the entry stays armed and
   still fires. *(Round 4.)*
2. …then the second removes; the entry goes. *(Round 5 — the orphan.)*
3. `-bp` adds; DAP adds the same address; DAP clears its owner; the `-bp`
   breakpoint survives with its condition and hit count intact. *(Rounds 5/6.)*
4. DAP adds, clears, the user F9s the same address, DAP adds again, teardown;
   the F9 breakpoint survives. *(Round 7 — stale registry, direction 1.)*
5. `-bp` adds; DAP adds; both let go; a later DAP breakpoint there is not
   mistaken for the user's. *(Round 7 — stale registry, direction 2.)*
6. One owner asking twice is idempotent, and one clear retires it exactly once.
   *(Round 8 — the mutual veto.)*
7. `debug_bp_clear_owner` on an owner holding no references is a no-op.
8. The watchpoint equivalent, with `-wp` as the surviving owner.

Plus the new behaviour: authoritative delete, the F9 toggle round-tripping,
normalised ownership keys, disable keeping the entry and its count, re-add
re-arming, and a full table reported distinctly from a duplicate.

Every one of these has been **mutation-tested**: the ownership rule is reverted,
the suite rebuilt, and a *named* check confirmed to fail. Several fixtures
during the original work looked like coverage and proved nothing, so a test that
has not been seen to fail should not be trusted.

`testbench/test_dap.py` covers what a unit test cannot — a real session, a real
teardown and a real reconnect — and reports a clear skip when the emulator was
not started with `-bp`.

## Notes for anyone extending this

- `debug_normalise_bank()` must be applied to any ownership key, or it will not
  match the entry the core created. That was its own defect, on gen2 with a
  non-zero program bank, where an address above `$A000` is *not* banked.
- The hot path must stay free of ownership. `debug_bp_on_arrival` reads
  `enabled` and nothing else new.
- `breakPoints` / `numBreakpoints` remain visible for the ImGui panels and the
  server's `bp_list`, but they are read-only for everyone outside
  `debug_core.c`. Prefer `debug_bp_count()` / `debug_bp_at()`.
- Adding an owner means adding one enum value. If the count ever exceeds 16,
  widen `owners`; nothing else needs to change.
