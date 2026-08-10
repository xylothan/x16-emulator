// Commander X16 Emulator — bridge for C++ ImGui panels to read C emulator
// state.
//
// The emulator core is C. C++ panels reach it through this one header so that
// linkage is correct and, more importantly, so that the compiler checks every
// call against the core's real declarations.
//
// PANEL AUTHORS: include THIS header, not the raw core headers. If you need
// another accessor, add it here rather than including core headers ad hoc from
// a panel -- that keeps one consistent surface everyone shares.
//
// This header used to re-DECLARE the emulator's functions and re-DEFINE
// `struct breakpoint` rather than including the headers that own them. That is
// unsafe in a way the compiler cannot see: a declaration that has drifted from
// the definition still compiles and still links, and simply does the wrong
// thing at runtime. It had in fact drifted -- dbg_info_label_to_addr() and
// dbg_info_symbol_at() were declared taking `uint16_t *` while the core writes
// a 32-bit dbg_addr_t through that pointer, so every call wrote two bytes past
// the caller's variable. Real headers are included now, and where the core has
// been renamed since, the old names survive below as inline shims that forward
// with the right types. Drift then breaks the build, in one place, instead of
// corrupting a stack frame.
#ifndef DEBUG_UI_BRIDGE_H
#define DEBUG_UI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

// struct regs — the CPU register file. POD; members include:
//   .pc (uint16)  .a/.xl/.yl (uint8 low bytes)  .c/.x/.y (uint16 aliases)
//   .sp .dp (uint16)  .status .k .db .e (uint8)  .is65c816 (bool)
#include "cpu/registers.h"

// Disassembly: the anchored disassembler / code map (its own extern "C" guards).
#include "code_map.h"

// Breakpoints, watchpoints, bank selectors, and the BPOPERAND_*/BPCMP_* codes.
// Owns `struct breakpoint` and `breakPoints`/`numBreakpoints`.
#include "debug_core.h"

// Execution control (DEBUGContinue/StepInto/StepOver/StepOut/Pause/RunTo,
// DEBUGIsRunning/IsPaused) and the run state. Pulls SDL, which is fine here:
// the ImGui backends are SDL2 already.
#include "debugger.h"

// Address labels parsed from a cc65 .dbg. Owns dbg_addr_t.
#include "dbg_info.h"

// Machine geometry: NUM_ROM_BANKS and the memory-map constants the Memory
// panel bounds its bank selectors by.
#include "glue.h"

// smc_requested_reset — the deferred machine-reset flag driven by System > Reset.
#include "smc.h"

#ifdef __cplusplus
extern "C" {
#endif

// The global CPU register file (defined in the C core).
extern struct regs regs;

// Non-intrusive debug read of the CPU address space.
//   address : 16-bit CPU address
//   bank    : RAM/ROM bank byte
//   debugOn : pass true from the debugger (guarantees no side effects)
//   x16Bank : -1 (DEBUG_UI_CURRENT_BANK) to follow the current X16 bank
uint8_t real_read6502(uint16_t address, uint8_t bank, bool debugOn, int16_t x16Bank);

// Non-intrusive read of VERA video memory / register space.
uint8_t video_space_read(uint32_t address);

// Non-intrusive read of a VERA I/O register (0x00..0x1F). Pass debugOn=true so
// the read has no side effects (no address auto-increment, no FIFO changes).
uint8_t video_read(uint8_t reg, bool debugOn);

// Explicit, intended debug write into VERA video memory (0..0x1FFFF).
void video_space_write(uint32_t address, uint8_t value);

// Active banks currently mapped at $A000-$BFFF (RAM) and $C000-$FFFF (ROM),
// and the controls the Memory panel drives its selectors with.
uint8_t memory_get_ram_bank(void);
uint8_t memory_get_rom_bank(void);
void    memory_set_ram_bank(uint8_t bank);
void    memory_set_rom_bank(uint8_t bank);

// RAM banking is sized at runtime by -ram, so num_ram_banks is a real variable.
// ROM is not: it is a fixed ROM[NUM_ROM_BANKS * 16384] array, and no accessor
// for it exists. The panel asked for one anyway, which linked only because the
// old bridge declared it without anyone providing it.
static inline int
memory_get_num_rom_banks(void)
{
	return NUM_ROM_BANKS;
}

// Core CPU write path. The *intended*/explicit write: runs I/O side effects
// (VERA/VIA/YM) and fires memory watchpoints.
void write6502(uint16_t address, uint8_t bank, uint8_t value);

// Explicit debug write into the CPU address space, mirroring the bank
// semantics of real_read6502(). Defined in panels/memory_panel.cpp.
void debug_ui_write6502(uint16_t address, uint8_t value, uint8_t bank, int16_t x16Bank);

// Publish the "run to cursor" target — the disassembly row under the mouse —
// for the Ctrl+F10 shortcut. Defined in debug_ui.cpp.
void debug_ui_set_cursor(uint16_t addr, uint8_t bank, bool valid);

// Cross-panel navigation. Defined in debug_ui.cpp. One-frame latency; visible
// to all panels regardless of draw order.
void debug_ui_request_goto(uint16_t addr, uint8_t bank);
bool debug_ui_peek_goto(uint16_t *addr, uint8_t *bank);

// ─── System control ────────────────────────────────────────────────────────
// Reset is deferred to the emulator-loop boundary via the SMC reset flag (the
// same path Ctrl-R uses) so it never fires mid-instruction. IRQ/NMI are
// edge-triggered exactly like the RESTORE-key NMI already in video.c.
// smc_requested_reset (smc.h), machine_nmi() and num_ram_banks (glue.h) come
// from the real headers included above; re-declaring them here is how the
// previous bridge drifted out of sync with the emulator, so they are
// deliberately not repeated.
void irq6502(void);               // trigger a maskable IRQ (honors the I flag)

// ─── Counters (status line) ─────────────────────────────────────────────────
extern uint32_t clockticks6502;      // CPU cycle counter (fake6502.c)
extern int      instruction_counter; // instructions executed (main.c)

#ifdef __cplusplus
} // extern "C"
#endif

// Follow the current X16 bank (mirrors USE_CURRENT_X16_BANK from memory.h).
#define DEBUG_UI_CURRENT_BANK (-1)

// ─── Compatibility shims ───────────────────────────────────────────────────
// The core's breakpoint and watchpoint API was renamed and given an explicit
// bank selector when it moved into debug_core. The panels still use the older
// spelling; these forward, and are the single place a future rename has to be
// dealt with. Each passes DEBUG_BANK_ANY where the old API had no selector,
// which is what the old behaviour amounted to.

static inline int  DEBUGAddBreakPoint(struct breakpoint bp) { return debug_bp_add(bp); }
static inline bool DEBUGRemoveBreakPoint(int pc, uint8_t bank) { return debug_bp_remove(pc, bank, DEBUG_BANK_ANY); }
static inline void DEBUGClearBreakpointCondition(int pc, uint8_t bank) { debug_bp_clear_condition(pc, bank, DEBUG_BANK_ANY); }
static inline void DEBUGForgetBreakpoint(int pc, uint8_t bank) { debug_bp_forget(pc, bank, DEBUG_BANK_ANY); }
static inline uint32_t DEBUGGetBreakpointHits(int pc, uint8_t bank) { return debug_bp_get_hits(pc, bank, DEBUG_BANK_ANY); }
static inline void DEBUGResetBreakpointHits(int pc, uint8_t bank) { debug_bp_reset_hits(pc, bank, DEBUG_BANK_ANY); }

static inline void
DEBUGSetBreakpointCondition(int pc, uint8_t bank, int x16Bank, int operand,
                            uint16_t operand_addr, int op, uint32_t value)
{
	debug_bp_set_condition(pc, bank, x16Bank, operand, operand_addr, op, value);
}

static inline void
DEBUGSetBreakpointIgnore(int pc, uint8_t bank, int x16Bank, uint32_t ignore)
{
	debug_bp_set_ignore(pc, bank, x16Bank, ignore);
}

static inline bool
DEBUGGetBreakpointCondition(int pc, uint8_t bank, int *has_cond, int *operand,
                            uint16_t *operand_addr, int *op, uint32_t *value,
                            uint32_t *ignore)
{
	return debug_bp_get_condition(pc, bank, DEBUG_BANK_ANY, has_cond, operand,
	                              operand_addr, op, value, ignore);
}

static inline int  DEBUGAddWatchPoint(uint16_t addr, uint16_t len) { return debug_wp_add(addr, len, DEBUG_BANK_ANY); }
static inline bool DEBUGRemoveWatchPoint(uint16_t addr) { return debug_wp_remove(addr, DEBUG_BANK_ANY); }
static inline bool DEBUGCheckWatchPoint(uint16_t addr) { return debug_wp_covers(addr); }

// The panels' operand/comparison names, as aliases of the core's codes rather
// than a second enum with its own numbering -- debug_core.h says these are
// external and must not be renumbered, and two independent copies is exactly
// how that promise gets broken.
enum { BP_OPND_A = BPOPERAND_A, BP_OPND_X = BPOPERAND_X, BP_OPND_Y = BPOPERAND_Y,
       BP_OPND_SP = BPOPERAND_SP, BP_OPND_P = BPOPERAND_P,
       BP_OPND_BYTE = BPOPERAND_BYTE, BP_OPND_WORD = BPOPERAND_WORD };
enum { BP_CMP_EQ = BPCMP_EQ, BP_CMP_NE = BPCMP_NE, BP_CMP_LT = BPCMP_LT,
       BP_CMP_LE = BPCMP_LE, BP_CMP_GT = BPCMP_GT, BP_CMP_GE = BPCMP_GE };

// Labels: the core deals in 24-bit dbg_addr_t, the panels in 16-bit addresses.
// Narrowing happens here, once, instead of through a mis-declared pointer.
static inline bool
debug_ui_label_to_addr(const char *name, uint16_t *addr)
{
	dbg_addr_t wide = 0;
	if (!dbg_info_label_to_addr(name, &wide)) return false;
	*addr = (uint16_t)(wide & 0xFFFF);
	return true;
}

static inline bool
debug_ui_source_to_addr(const char *file, int line, uint16_t *addr)
{
	dbg_addr_t wide = 0;
	if (!dbg_info_source_to_addr(file, line, &wide)) return false;
	*addr = (uint16_t)(wide & 0xFFFF);
	return true;
}

static inline bool
debug_ui_symbol_at(int index, const char **name, uint16_t *addr)
{
	dbg_addr_t wide = 0;
	if (!dbg_info_symbol_at(index, name, &wide)) return false;
	*addr = (uint16_t)(wide & 0xFFFF);
	return true;
}

// Convenience wrapper mirroring memory.h's debug_read6502() macro, for use from
// C++ panels: read one CPU byte for display with no side effects.
static inline uint8_t
debug_ui_read6502(uint16_t address, uint8_t bank, int16_t x16Bank)
{
	return real_read6502(address, bank, true, x16Bank);
}

// Bulk copy a run of VERA video memory into `dest`. Wraps the 17-bit VRAM space
// ($00000..$1FFFF) so a viewer can never read out of bounds.
static inline void
debug_ui_vram_read_range(uint8_t *dest, uint32_t address, uint32_t size)
{
	for (uint32_t i = 0; i < size; ++i) {
		dest[i] = video_space_read((address + i) & 0x1FFFF);
	}
}

#endif // DEBUG_UI_BRIDGE_H
