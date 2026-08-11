// Commander X16 Emulator — disassembly-accuracy core ("code map").
//
// THE PROBLEM: 65C02/65C816 instructions are variable length, and on the
// 65C816 the operand width of many instructions depends on the M/X bits of the
// processor status register. A naive linear disassembler therefore drifts out
// of alignment: start one byte off (or assume the wrong operand width) and
// every following instruction is garbage until it accidentally re-syncs.
//
// THE FIX: capture ground truth while the program actually runs. Every time the
// CPU fetches an instruction we record (a) that the address is a REAL
// instruction start, keyed by the bank context that actually backs it, and (b)
// the effective processor status (M/X/E folded in) at that moment. The anchored
// disassembler then aligns instruction boundaries using, in priority order:
//   1. recorded instruction starts (live execution ground truth),
//   2. cc65 .dbg span starts (compiler-emitted boundaries),
//   3. a backward self-sync heuristic (try candidate offsets, keep the one that
//      decodes cleanly onto the anchor).
// A decode is never allowed to swallow a recorded start UNLESS it is itself
// recorded -- two recorded starts that overlap are two real execution paths,
// not a bad guess straddling a good one -- so an over-wide guess is corrected
// at the next piece of hard evidence rather than drifting on.
//
// WHERE THE WIDTHS ARE STILL A GUESS, so callers know what they are getting:
// only five instructions change the 65C816 register widths -- REP, SEP, XCE,
// PLP and RTI. REP and SEP apply their own status bits exactly (the bits are in
// the operand), though the widths those imply are still subject to the E
// estimate below. XCE is exact only when both of its inputs are right, and
// neither is guaranteed: it swaps the carry and emulation flags, and the carry
// is not tracked through any data-dependent change (ADC, SBC, the compares, the
// shifts and rotates), while E is not recorded by anchors at all. The last two
// restore a status byte pulled off the stack; this linear, instruction-stream
// analysis does not recover it.
//
// None of these gaps is bounded unconditionally.
//
// A wrong STATUS estimate can be corrected when an accurate, believed anchor is
// reached -- but that anchor fixes its OWN line, and accuracy does not
// automatically carry forward: an anchor on a PLP or an RTI supplies the status
// going INTO it, and the byte it restores is still not modelled, so the very
// next line can be wrong again. A same-opcode stale anchor is "recorded" and
// hands back its old status, and there may be no accurate anchor ahead at all.
//
// A wrong E is weaker again, because anchors do not store E: an anchor fixes
// the width of its own line, and propagation then folds the stale E back in,
// which can mis-size the next unanchored line wherever the stale E and the real
// one imply different widths. It is not unrecoverable -- an XCE decoded with an
// accurate carry writes a correct E, as does an explicit CLC/SEC before one --
// but nothing guarantees such a sequence appears, so no persistent recovery is
// promised.
//
// Note the emulation-mode escape clause applies to the ESTIMATE, not the
// machine: the real CPU in emulation mode always has 8-bit widths, but the
// fold-in here uses the estimated E, so a diverged estimate can size operands
// as though native while the machine is really in emulation. Pinned by tests.
//
// This costs nothing in practice unless all of the following hold at once: the
// machine is a 65C816 (on the 65C02 widths never vary), the line being drawn
// has no currently believed anchor, and the status the estimate carries into
// that line is not the status the code there really runs with. That last one is
// broader than the unmodelled instructions above -- an address-linear walk over
// a branch or call, or a walk started away from the PC, does it too. See
// docs/code-map-width-propagation.md. Every line this could affect already
// reports `recorded = false`.
//
// Anchors are not deleted when the code under them changes -- there is no write
// hook -- so each stores the opcode byte that was executing and is ignored once
// memory no longer matches. That catches most overlay loads, a second program
// loaded over the first, and self-modifying code, without needing a hook on
// every memory write. It is a one-byte check, not a proof: replacement code
// that happens to repeat the same opcode byte at the same address keeps the old
// anchor, and with it the old recorded status. Such an anchor is still
// believed, so it can cover a genuine instruction start in the new code; and on
// a 65C816 its stale status can also make the line decode wider -- or narrower
// -- than the new code really is. Self-correcting once an accurate anchor it
// has not swallowed is reached, if there is one ahead, and pinned by tests. See
// docs/code-map-width-propagation.md.
//
// This header is plain C and is shared by the C core (recording hooks, DAP) and
// the C++ ImGui panel (via debug_ui_bridge.h).
#ifndef _CODE_MAP_H_
#define _CODE_MAP_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Record a real instruction start as the CPU executes it. Call this at every
// instruction fetch, from every execution site, so coverage is complete.
//   pc       : the instruction-start address (regs.pc before step6502()).
//   pbank    : CPU program bank (regs.k; always 0 on the 65C02). Part of the
//              key only where it actually selects memory -- on a Gen2 machine a
//              non-zero program bank maps its own flat 64K, while on gen1 the
//              program bank selects nothing even with -c816.
//   rambank  : active RAM bank  (memory_get_ram_bank())  — banks $A000-$BFFF.
//   rombank  : active ROM bank  (memory_get_rom_bank())  — banks $C000-$FFFF.
//   status   : effective processor status. The caller folds the emulation-mode
//              width bits in (E set => M/X forced to 8-bit) so the value can be
//              fed straight into disasm()'s implied_status.
// Cheap: one bitset set, one status store and one opcode read/store. Safe to
// call every instruction.
void code_map_record(uint16_t pc, uint8_t pbank, uint8_t rambank, uint8_t rombank, uint8_t status);

// True if `pc` has been recorded as a real instruction start in the given bank
// context AND the opcode byte recorded there still matches memory.
bool code_map_is_recorded_start(uint16_t pc, uint8_t pbank, uint8_t rambank, uint8_t rombank);

// The effective status recorded at `pc`, or `fallback` if `pc` was never
// recorded in this context -- or if its anchor is no longer believed, because
// the opcode byte under it has changed.
uint8_t code_map_recorded_status(uint16_t pc, uint8_t pbank, uint8_t rambank, uint8_t rombank, uint8_t fallback);

typedef enum {
	CM_LINE_INSTRUCTION = 0, // a whole instruction: text is its disassembly
	CM_LINE_DATA        = 1, // raw bytes: text is ".byte $xx,..." over exactly
	                         // `size` bytes, and there is no effective address
} code_map_line_kind_t;

// One disassembled line produced by the anchored disassembler.
typedef struct {
	uint16_t addr;         // start address of this line
	uint8_t  size;         // bytes this line covers (always >= 1)
	uint8_t  status;       // implied_status actually used to decode it
	int32_t  eff_addr;     // effective address, or -1 if none
	bool     recorded;     // a whole instruction, backed by live-execution
	                       // coverage. False for every CM_LINE_DATA row.
	uint8_t  kind;         // code_map_line_kind_t: instruction or raw data
	bool     start_recorded; // this ADDRESS carries a currently believed anchor.
	                         // Distinguishes "no live-execution evidence here"
	                         // from "there is, but the row could not be shown
	                         // whole on this path". Not proof the current code
	                         // ran: an anchor can be evicted with its bank
	                         // context (false negative) or survive a same-opcode
	                         // overwrite (false confidence).
	uint8_t  bytes[4];     // raw bytes (first `size` valid)
	char     text[48];     // mnemonic + operands, or ".byte $xx,..."
} code_map_line_t;

// Produce an aligned disassembly window centered on `center`.
//   center            : address to center on (usually regs.pc).
//   bank              : CPU program bank (regs.k; 0 on the 65C02 / X16 gen1).
//   rambank, rombank  : bank context for banked reads and coverage lookup.
//   lines_before      : number of instructions the backward walk resolves
//                       before `center` (capped at 255; negative is treated as
//                       none). Tiling can turn one resolved instruction into
//                       several rows -- see the note on the returned count
//                       below.
//   lines_after       : number of rows from `center` onward, including the
//                       center row itself. A row count, not an instruction
//                       count: a decode forced to give way emits a
//                       CM_LINE_DATA row, which consumes one of them.
//   out               : caller-provided array receiving the lines, low->high.
//   max_out           : capacity of `out`.
//   out_center_index  : receives the index within `out` of the `center` line
//                       (-1 if it did not fit); may be NULL.
//
// The lines written TILE the range they cover: each one starts exactly where
// the previous one ended, with no gaps and no overlaps, so a caller can render
// them as consecutive rows without reconciling contradictory byte ranges.
//
// Holding that invariant occasionally costs an extra line. Where a decode has
// to be cut short -- it would swallow an address live execution proved is an
// instruction start, or it disagrees with a boundary the backward walk already
// resolved -- the bytes up to the next start are covered by a further line
// rather than left out. The count returned can therefore exceed
// `lines_before + lines_after`; it never exceeds `max_out`. Callers wanting a
// fixed row count should use the returned count, not the requested one.
//
// Returns the number of lines written (<= max_out). Only `out[0 .. returned-1]`
// are meaningful: resolving the window may use the rest of the buffer up to
// `max_out` as scratch, so a caller must not expect entries beyond the returned
// count to be preserved. Nothing is ever written past `max_out`.
int code_map_disasm_window(uint16_t center, uint8_t bank, uint8_t rambank, uint8_t rombank,
                           int lines_before, int lines_after,
                           code_map_line_t *out, int max_out, int *out_center_index);

// Disassemble a run of `count` rows starting at `start`, aligned via the flag
// map. Fills `out` low->high and returns the count written. Used by the DAP
// `disassemble` request. `next_addr` (may be NULL) receives the address just
// past the last row. Rows are normally whole instructions, but a decode forced
// to give way to a recorded start inside it comes back as CM_LINE_DATA, so a
// caller asking for N rows may get fewer than N instructions' worth of address
// range.
int code_map_disasm_forward(uint16_t start, uint8_t bank, uint8_t rambank, uint8_t rombank,
                            int count, code_map_line_t *out, int max_out, uint16_t *next_addr);

// Given a known instruction boundary `addr`, resolve the start address of the
// instruction immediately preceding it, using recorded starts, then .dbg spans,
// then a backward self-sync heuristic. Returns the predecessor address (falls
// back to addr-1 when nothing aligns cleanly).
uint16_t code_map_prev_instruction(uint16_t addr, uint8_t bank, uint8_t rambank, uint8_t rombank);

// Forget all recorded coverage and flags (e.g. on machine reset).
void code_map_reset(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _CODE_MAP_H_
