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
// A decode is never allowed to swallow a recorded start, so an over-wide guess
// is corrected at the next piece of hard evidence rather than drifting on.
//
// WHAT THIS DOES NOT DO, so callers know what they are getting:
//   - Where no coverage exists the 65C816 register widths are only estimated,
//     from a deliberately small model of the opcodes that change them (CLC, SEC,
//     REP, SEP, XCE). A width change made by PLP, RTI or an interrupt is not
//     followed, so a gap between anchors can still be decoded at the wrong
//     width. Recorded status always wins where it exists.
//
// Anchors do not outlive the code they describe: each one stores the opcode byte
// that was executing, and is ignored once memory no longer matches. That covers
// overlay loads, a second program loaded over the first, and self-modifying
// code, without needing a hook on every memory write.
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
// Cheap: one bitset set plus one status store. Safe to call every instruction.
void code_map_record(uint16_t pc, uint8_t pbank, uint8_t rambank, uint8_t rombank, uint8_t status);

// True if `pc` has been recorded as a real instruction start in the given bank
// context.
bool code_map_is_recorded_start(uint16_t pc, uint8_t pbank, uint8_t rambank, uint8_t rombank);

// The effective status recorded at `pc` (or `fallback` if `pc` was never
// recorded in this context).
uint8_t code_map_recorded_status(uint16_t pc, uint8_t pbank, uint8_t rambank, uint8_t rombank, uint8_t fallback);

// One disassembled line produced by the anchored disassembler.
typedef struct {
	uint16_t addr;         // instruction start address
	uint8_t  size;         // instruction length in bytes (always >= 1)
	uint8_t  status;       // implied_status actually used to decode it
	int32_t  eff_addr;     // effective address, or -1 if none
	bool     recorded;     // backed by live-execution coverage
	uint8_t  bytes[4];     // raw instruction bytes (first `size` valid)
	char     text[48];     // mnemonic + operands
} code_map_line_t;

// Produce an aligned disassembly window centered on `center`.
//   center            : address to center on (usually regs.pc).
//   bank              : CPU program bank (regs.k; 0 on the 65C02 / X16 gen1).
//   rambank, rombank  : bank context for banked reads and coverage lookup.
//   lines_before      : number of instructions to resolve before `center`.
//   lines_after       : number of instructions after `center` (incl. center).
//   out               : caller-provided array receiving the lines, low->high.
//   max_out           : capacity of `out`.
//   out_center_index  : receives the index within `out` of the `center` line
//                       (-1 if it did not fit); may be NULL.
// Returns the number of lines written (<= max_out).
int code_map_disasm_window(uint16_t center, uint8_t bank, uint8_t rambank, uint8_t rombank,
                           int lines_before, int lines_after,
                           code_map_line_t *out, int max_out, int *out_center_index);

// Disassemble a run of `count` instructions starting at `start`, aligned via
// the flag map. Fills `out` low->high and returns the count written. Used by the
// DAP `disassemble` request. `next_addr` (may be NULL) receives the address just
// past the last decoded instruction.
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
