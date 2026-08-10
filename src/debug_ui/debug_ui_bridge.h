// Commander X16 Emulator — bridge for C++ ImGui panels to read C emulator
// state.
//
// The emulator core is C. C++ panels must reach C globals/functions through a
// single, documented, C++-safe surface so that linkage is correct.
//
// FLEET PANEL AUTHORS: include THIS header (not the raw C core headers) to read
// emulator state. If you need another accessor, ADD it here inside the
// extern "C" block rather than including core C headers ad hoc from a panel —
// that keeps one consistent bridge everyone shares.
//
// Why it is shaped this way:
//   * Struct/type *definitions* (POD; identical layout in C and C++) are
//     included normally — structs have no language linkage, so `struct regs`
//     is the same type in both languages. (registers.h below.)
//   * Function and variable *declarations* go inside `extern "C"` so the C++
//     compiler references the C symbol names the core actually exports.
#ifndef DEBUG_UI_BRIDGE_H
#define DEBUG_UI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

// struct regs — the CPU register file. POD; members include:
//   .pc (uint16)  .a/.xl/.yl (uint8 low bytes)  .c/.x/.y (uint16 aliases)
//   .sp .dp (uint16)  .status .k .db .e (uint8)  .is65c816 (bool)
// See src/cpu/registers.h. Resolved via the `src` include dir.
#include "cpu/registers.h"

// Disassembly: the anchored disassembler / code map (its own extern "C" guards).
#include "code_map.h"

// The real emulator headers, rather than copies of their declarations.
//
// This file used to re-declare ~20 core functions and re-define struct
// breakpoint and struct watchpoint, on the reasoning that debugger.h drags in
// SDL. The cost of that was silent drift: the copies stopped matching the core
// and nothing said so, because a duplicate declaration compiles and links
// perfectly whether or not it is true. Three examples that were live here:
//
//   * dbg_info_label_to_addr / _symbol_at / _addr_to_label were declared with
//     uint16_t* address arguments. The core writes a 32-bit dbg_addr_t through
//     that pointer, so every symbol and source lookup wrote two bytes past the
//     caller's variable.
//   * struct watchpoint was missing the core's x16Bank field, while
//     watchPoints[] was declared extern and indexed directly -- wrong field
//     offsets and a 12-byte stride over a 16-byte array.
//   * memory_get_num_rom_banks() was declared but has never existed.
//
// So the rule is now the opposite one: include the real header, and where the
// panels genuinely want a different shape, convert in a documented shim below
// where the compiler can check it.
#include "debug_core.h"   // breakpoints, watchpoints, conditions, bank rules
#include "debugger.h"     // execution control + run state (pulls SDL, which is
                          // fine: the ImGui backends are SDL2 already)
#include "dbg_info.h"     // symbols and source lines; owns dbg_addr_t
#include "memory.h"       // real_read6502, write6502, bank mapping
#include "glue.h"         // is_gen2, num_banks, num_ram_banks, MHZ, warp_mode
#include "smc.h"          // smc_requested_reset
#include "timing.h"       // absolute kHz speed control

#ifdef __cplusplus
extern "C" {
#endif

// The global CPU register file (defined in the C core).
extern struct regs regs;

// Non-intrusive read of VERA video memory / register space.
uint8_t video_space_read(uint32_t address);

// RAM banking is sized at runtime by -ram, so num_ram_banks (glue.h) is a real
// variable. ROM is not: it is a fixed ROM[NUM_ROM_BANKS * 16384] array and has
// no accessor. The panels asked for memory_get_num_rom_banks(), which linked
// only because this header declared a function nobody defined.
static inline int
memory_get_num_rom_banks(void)
{
	return NUM_ROM_BANKS;
}

// ─── Breakpoints ───────────────────────────────────────────────────────────
// struct breakpoint, breakPoints[], numBreakpoints and the debug_bp_* API all
// come from debug_core.h. The panels were written against the older DEBUG*
// spelling, and against a two-argument (pc, bank) identity that predates bank
// awareness -- a breakpoint is now identified by (pc, bank, x16Bank), because
// the same $A000 in two RAM banks is two different breakpoints.
//
// DEBUG_BANK_ANY is the right default for the UI: it means "whatever bank is
// mapped", which is what a user clicking a gutter in a disassembly view means.
static inline int  DEBUGAddBreakPoint(struct breakpoint bp) { return debug_bp_add(bp); }
static inline bool DEBUGRemoveBreakPoint(int pc, uint8_t bank) { return debug_bp_remove(pc, bank, DEBUG_BANK_ANY); }
static inline void DEBUGForgetBreakpoint(int pc, uint8_t bank) { debug_bp_forget(pc, bank, DEBUG_BANK_ANY); }
static inline void DEBUGClearBreakpointCondition(int pc, uint8_t bank) { debug_bp_clear_condition(pc, bank, DEBUG_BANK_ANY); }
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

// The panels' operand/comparison names, as aliases of the core's codes rather
// than a second enum with its own numbering. debug_core.h states these values
// are external and must not be renumbered; two independent copies is exactly
// how that promise gets broken.
enum { BP_OPND_A = BPOPERAND_A, BP_OPND_X = BPOPERAND_X, BP_OPND_Y = BPOPERAND_Y,
       BP_OPND_SP = BPOPERAND_SP, BP_OPND_P = BPOPERAND_P,
       BP_OPND_BYTE = BPOPERAND_BYTE, BP_OPND_WORD = BPOPERAND_WORD };
enum { BP_CMP_EQ = BPCMP_EQ, BP_CMP_NE = BPCMP_NE, BP_CMP_LT = BPCMP_LT,
       BP_CMP_LE = BPCMP_LE, BP_CMP_GT = BPCMP_GT, BP_CMP_GE = BPCMP_GE };

// ─── VERA: accessors ───────────────────────────────────────────────────────
// Non-intrusive read of a VERA I/O register (0x00..0x1F). Pass debugOn=true so
// the read has no side effects (no address auto-increment, no FIFO changes).
// Used by the VERA panel to read the layer registers ($0D..$1A) that describe
// map/tile bases, color depth, scroll, and bitmap width.
uint8_t video_read(uint8_t reg, bool debugOn);

// Per-scanline history of the layer registers that were actually used to render
// each display line (video.c). Programs commonly rewrite MAPBASE/TILEBASE/scroll
// part-way down a frame from a line IRQ ("raster split"), so a single register
// snapshot only describes one band of the screen. Viewers use this to decode
// each row with the registers that produced it. out_regs receives the 7 layer
// registers L?_CONFIG..L?_VSCROLL_H; out_eff_y is the composer's effective layer
// Y for that line (before the layer's own VSCROLL). Returns false if the line is
// out of range or has not been rendered yet.
bool     video_get_layer_line_state(uint8_t layer, uint16_t line, uint8_t out_regs[7],
                                    uint16_t *out_eff_y, bool *out_enabled);
uint16_t video_get_scanline_count(void);

// Size in layer pixels of the image the composer is actually displaying: the
// active window (DC_HSTART/HSTOP, DC_VSTART/VSTOP) scaled by DC_HSCALE/VSCALE.
// Viewers use this to default their geometry to the current video mode rather
// than to a fixed guess. (video.c)
void video_get_active_layer_size(int *out_w, int *out_h);

// ─── Memory: accessors  (Memory panel — keep minimal & shared) ─────────────

// Explicit, intended debug write into VERA video memory (0..0x1FFFF). (video.h)
void video_space_write(uint32_t address, uint8_t value);

// Banked-memory mapping controls (memory.c, declared in memory.h above). The
// Memory panel drives its RAM/ROM bank selectors with these.

// Explicit debug write into the CPU address space, mirroring the bank
// semantics of real_read6502(): for banked RAM ($A000-$BFFF) / ROM
// ($C000-$FFFF), x16Bank>=0 selects the bank (-1 = current mapping). Wraps
// write6502() (save/set/restore bank for banked regions).
// NOTE: the definition currently lives in panels/memory_panel.cpp; integration
// may relocate it into the core.
void debug_ui_write6502(uint16_t address, uint8_t value, uint8_t bank, int16_t x16Bank);

// ─── Memory write watchpoints ──────────────────────────────────────────────
// struct watchpoint, MAX_WATCHPOINTS and the debug_wp_* API come from
// debug_core.h. This header used to carry its own copy of the struct, and that
// copy was missing the core's x16Bank field while watchPoints[] was declared
// extern and indexed directly -- so the panel read every field after `len` from
// the wrong offset, with a 12-byte stride over a 16-byte array. The table is
// static in debug_core.c and is now read through debug_wp_at().
static inline int  DEBUGAddWatchPoint(uint16_t addr, uint16_t len) { return debug_wp_add(addr, len, DEBUG_BANK_ANY); }
static inline bool DEBUGRemoveWatchPoint(uint16_t addr) { return debug_wp_remove(addr, DEBUG_BANK_ANY); }
static inline bool DEBUGCheckWatchPoint(uint16_t addr) { return debug_wp_covers(addr); }

// ─── Machine model (glue.h) ────────────────────────────────────────────────
// is_gen2 is true for the X16 GS / Gen-2 machine (-gs): a 65C816 with 24-bit
// addressable RAM. In gen2, the 65C816 bank byte selects a flat 64K RAM page:
// bank 0 is the classic X16 map (I/O + windowed $A000-$BFFF/$C000-$FFFF banks);
// banks 1..num_banks-1 are flat RAM. num_banks is the total number of 64K CPU
// banks (256 in gen2, 1 otherwise) — bounds the Memory panel's CPU-bank selector.
extern bool     is_gen2;
extern uint16_t num_banks;

// ─── Execution control (debugger.c) ────────────────────────────────────────
// Drive the CPU from a panel exactly like the debugger control bar does. These
// mirror the classic F5/F10/F11 handlers and the DAP commands (including the
// timing re-base on resume). Handy for e.g. a "Run to cursor" in the disasm
// panel or a "Continue" shortcut in any panel.
void DEBUGContinue(void);   // resume free-run
void DEBUGStepInto(void);   // single instruction
void DEBUGStepOver(void);   // step over calls
void DEBUGStepOut(void);    // run to return address
void DEBUGPause(void);      // halt now
void DEBUGRunTo(uint16_t pc, uint8_t bank); // run to (pc,bank), then halt
bool DEBUGIsRunning(void);  // true in RUN/SLOW
bool DEBUGIsPaused(void);   // true in STOP

// Why execution last stopped. This lived in the DAP server before, which meant
// it did not exist unless that was built; it belongs to the debugger, which is
// what actually stops. Kept under the old name so the panels read the same.
static inline const char *debug_server_last_stop_reason(void) { return DEBUGGetStopReason(); }

// Interrupt following (debugger.c). With follow-interrupts on, an interrupt
// taken while stepping stops at the handler's first instruction instead of
// being run through invisibly; break-on-interrupt stops on every entry.
void DEBUGSetFollowInterrupts(bool on);
bool DEBUGGetFollowInterrupts(void);
void DEBUGSetBreakOnInterrupt(bool on);
bool DEBUGGetBreakOnInterrupt(void);

// Publish the "run to cursor" target — the disassembly instruction row under// the mouse — for the Ctrl+F10 shortcut (consumed by debug_ui_run_to_cursor()).
// The Disassembly panel calls this every frame: valid=true for the hovered row,
// valid=false when nothing is hovered. Defined in debug_ui.cpp.
void debug_ui_set_cursor(uint16_t addr, uint8_t bank, bool valid);

// Cross-panel navigation. A panel (e.g. Symbols / Call Stack) calls
// debug_ui_request_goto() to jump the Disassembly + Memory panels to an address;
// those panels call debug_ui_peek_goto() each frame to pick it up. Defined in
// debug_ui.cpp. (One-frame latency; visible to all panels regardless of order.)
void debug_ui_request_goto(uint16_t addr, uint8_t bank);
bool debug_ui_peek_goto(uint16_t *addr, uint8_t *bank);

// "Add to watch": the watch list lives in the CPU panel, but the natural place
// to ask for a watch is wherever you found the address (the memory view's
// context menu, including over a drag-selected range). The CPU panel drains
// this once per frame and splits anything longer than one watch row. bank is an
// x16 RAM/ROM bank, or -1 for "whatever is currently mapped".
void debug_ui_request_watch(uint16_t addr, int16_t bank, uint16_t len);
bool debug_ui_take_watch_request(uint16_t *addr, int16_t *bank, uint16_t *len);

// ─── System control (main.c / smc.c / cpu) ─────────────────────────────────
// Reset is deferred to the emulator-loop boundary via the SMC reset flag (the
// same path Ctrl-R uses) so it never fires mid-instruction; machine_reset()
// also calls code_map_reset() so the disassembly re-aligns. IRQ/NMI are
// edge-triggered exactly like the RESTORE-key NMI already in video.c.
extern bool smc_requested_reset;  // set true to request a machine reset
void machine_nmi(void);           // trigger an NMI
void irq6502(void);               // trigger a maskable IRQ (honors the I flag)

// ─── Symbols / labels ──────────────────────────────────────────────────────
// dbg_info.h (included above) declares these. The panels want 16-bit addresses
// while the core deals in 24-bit dbg_addr_t, so the narrowing happens here,
// once, in shims the compiler can type-check -- rather than through a
// re-declaration with a uint16_t* parameter, which is what previously made the
// core write four bytes into the caller's two-byte variable.

// ─── Counters (status line) ─────────────────────────────────────────────────
extern uint32_t clockticks6502;      // CPU cycle counter (fake6502.c)
extern int      instruction_counter; // instructions executed (main.c)

// ─── Emulation speed / warp (timing.h above, main.c, glue.h) ────────────────
// Speed is an absolute target clock in kHz. The machine's own clock comes from
// -mhz at startup, so timing_native_khz() is what "normal speed" means for this
// run. Warp bypasses the throttle entirely and ignores the target.
void machine_toggle_warp(void);

// ─── Interrupt context (cpu/fake6502.c) ─────────────────────────────────────
// The CPU core records interrupt entry/exit so the debugger can show when
// execution is inside a handler. Depth counts nesting.
bool     cpu_in_interrupt(void);
int      cpu_irq_depth(void);
uint32_t cpu_irq_count(void);
int      cpu_irq_last_vector(void);
uint16_t cpu_irq_return_pc(void);

// ─── VERA interrupt prediction (video.c) ────────────────────────────────────
// Cycles until the next enabled VERA interrupt and the ISR bit that will cause
// it (1 = VSYNC, 2 = LINE); false when neither is enabled.
bool video_next_irq(float mhz, uint32_t *out_cycles, uint8_t *out_source);
void video_get_irq_state(uint8_t *out_ien, uint8_t *out_isr, uint16_t *out_irq_line);

#ifdef __cplusplus
} // extern "C"
#endif

// ─── Audio: accessors ───────────────────────────────────────────────────────
// The three audio sources each expose a small, side-effect-free debug surface
// for the PSG / YM2151 / PCM panels. Unlike debugger.h (which drags in SDL),
// these headers are plain and carry their own extern "C" guards, so panels get
// the POD debug structs *and* the correctly-linked declarations by including
// them here — hence they are included rather than re-declared.
//
// Threading: psg_render(), pcm_render() and YM_stream_update() all run from
// audio_render() on the emulator main thread, which is the same thread that
// calls debug_ui_render(). The scope ring buffers are therefore
// single-producer/single-consumer on one thread and need no locking.
//
// Scope capture is SELF-ARMING: each *_scope_read() call re-arms capture for
// about half a second, and the audio cores stop capturing on their own once
// nothing reads. Panels just call the read every frame while their Scope tab is
// visible — there is no enable/disable to balance, and no cost when hidden.
#include "vera_psg.h" // PSG voices, registers, scope   (psg_debug_*)
#include "vera_pcm.h" // VERA PCM FIFO + scope          (pcm_debug_*)
#include "ymglue.h"   // YM2151 registers, EG, scope    (YM_debug_*)

// Follow the current X16 bank (mirrors USE_CURRENT_X16_BANK from memory.h).
#define DEBUG_UI_CURRENT_BANK (-1)

// Convenience wrapper mirroring memory.h's debug_read6502() macro, for use from
// C++ panels: read one CPU byte for display with no side effects.
static inline uint8_t
debug_ui_read6502(uint16_t address, uint8_t bank, int16_t x16Bank)
{
    return real_read6502(address, bank, true, x16Bank);
}

// VERA: accessors — bulk copy a run of VERA video memory into `dest`. Wraps the
// 17-bit VRAM space ($00000..$1FFFF) so a viewer can never read out of bounds.
// Convenient for decoding tiles/sprites/bitmaps a whole tile-row at a time.
static inline void
debug_ui_vram_read_range(uint8_t *dest, uint32_t address, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        dest[i] = video_space_read((address + i) & 0x1FFFF);
    }
}

// ─── Symbol / source lookups, narrowed ─────────────────────────────────────
// The core addresses code with a 24-bit dbg_addr_t; the panels work in the
// 16-bit CPU address space. Each shim narrows in one place, under the
// compiler's supervision. Passing a uint16_t* straight to the core -- which is
// what this header used to declare -- silently corrupted two bytes of the
// caller's stack on every call.

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

static inline bool
debug_ui_addr_to_label_nearest(uint16_t addr, const char **name, uint16_t *sym_addr)
{
	dbg_addr_t wide = 0;
	if (!dbg_info_addr_to_label_nearest(addr, name, &wide)) return false;
	*sym_addr = (uint16_t)(wide & 0xFFFF);
	return true;
}

static inline bool
debug_ui_name_to_value(const char *name, uint16_t *addr, int *out_kind)
{
	dbg_addr_t wide = 0;
	if (!dbg_info_name_to_value(name, &wide, out_kind)) return false;
	*addr = (uint16_t)(wide & 0xFFFF);
	return true;
}

#endif // DEBUG_UI_BRIDGE_H
