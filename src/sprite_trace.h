// Commander X16 Emulator — sprite multiplexing trace
//
// VERA has 128 hardware sprite slots. A program that needs more rewrites the
// sprite attribute table part-way down the frame from a raster IRQ, so one slot
// shows a different sprite in each horizontal band of the screen. The static
// sprite viewer in the VERA panel reads the attribute table as it stands when
// you happen to look at it, which for a multiplexed frame is whatever the last
// band left behind -- the other 900-odd sprites the player saw are simply not
// in the data any more. This module records them.
//
// ─── What is recorded, and from where ──────────────────────────────────────
//
// Two streams, deliberately separate, because they answer different questions.
//
//   The renderer stream is ground truth. render_sprite_line() already walks all
//   128 slots for every scanline and already enforces VERA's per-line sprite
//   fetch budget, so it knows exactly which slot held which value when the beam
//   passed, which of those actually put pixels on screen, and which were cut
//   off when the budget ran out. Every "what did the frame contain" number here
//   comes from there, and none of it is reconstructed.
//
//   The mutation stream is the cause: the CPU's writes into $1FC00-$1FFFF, with
//   the cycle and scanline each landed on. That is what makes the CPU cost of
//   the multiplexing measurable, and what makes a write that missed its own
//   raster window detectable.
//
// Crossing the two is where the useful diagnostics live: a slot value the CPU
// wrote but the beam never drew is a wasted write, and a slot value written
// after the beam had already passed its Y position is a late write -- the
// classic multiplexer bug, and invisible in any view that only reads the
// attribute table.
//
// ─── Generations ───────────────────────────────────────────────────────────
//
// A "generation" is one value a slot held for one span of the frame. A sprite
// that never moves is one generation covering the whole frame; a slot recycled
// eight times is eight. Generations are opened lazily, when the renderer next
// reads a slot the CPU has dirtied, rather than on the write itself. That is
// what makes a partial update honest: a multiplexer storing bytes 2..5 across a
// scanline boundary leaves the renderer looking at a half-updated sprite for
// one line, and recording at read time captures the half-updated value that was
// really displayed instead of a coherent one that never existed.
//
// Per-generation totals are derived in sprite_trace_frame_end() by scanning the
// residency map, not accumulated as the frame runs. render_line() can be called
// more than once for the same scanline (a mid-line register split re-renders
// the line, and the last call wins), so running totals would count those lines
// twice; a single pass over the finished map cannot.

#ifndef SPRITE_TRACE_H
#define SPRITE_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPRITE_TRACE_SLOTS 128
#define SPRITE_TRACE_LINES 480

// Clocks the sprite renderer gets per scanline, matching render_time_done in
// sprite_renderer.v:43 and SPRITE_RENDER_TIME in video.c. Kept here so the
// panel can draw the ceiling without reaching into video.c.
//
// It is WALL CLOCK, not bus accesses: render_time_r increments every clock
// (sprite_renderer.v:46-56), including the STATE_RENDER clocks when the
// renderer is painting the line buffer and holding no bus strobe at all. So
// this is the outer envelope a line's sprite work must finish inside, not a
// share of VRAM bandwidth -- see src/vera_bandwidth.h, which charges the same
// 800-clock line window that this 798 is measured against.
#define SPRITE_TRACE_LINE_BUDGET 798

// Marks a generation that was already in the attribute table when the frame
// started, rather than written during it.
#define SPRITE_TRACE_NO_LINE 0xFFFFu

// Per (line, slot) residency flags.
enum {
	SPRITE_TRACE_EVALUATED = 1u << 0, // the renderer reached this slot on this line
	SPRITE_TRACE_ONSCREEN  = 1u << 1, // enabled (z != 0) and the line fell inside its Y span
	SPRITE_TRACE_DREW      = 1u << 2, // put at least one non-transparent pixel on the line
	SPRITE_TRACE_CUT       = 1u << 3, // the line's budget ran out part-way through this sprite
};

// Per-generation flags.
enum {
	SPRITE_GEN_CARRIED = 1u << 0, // held over from before the frame, never written during it
	SPRITE_GEN_DREW    = 1u << 1, // drew on at least one line
	SPRITE_GEN_CUT     = 1u << 2, // was cut short by the budget on at least one line
	SPRITE_GEN_LATE    = 1u << 3, // written after the beam had passed its own Y span
};

// One value one slot held for one span of the frame.
typedef struct {
	uint8_t  attr[8];        // the attribute bytes as the renderer first read them
	uint8_t  slot;           // 0..127
	uint8_t  flags;          // SPRITE_GEN_*
	uint16_t index_in_slot;  // 0 for the slot's first value this frame, then 1, 2, ...
	uint16_t write_line;     // scanline the CPU dirtied it on, or SPRITE_TRACE_NO_LINE
	uint32_t write_cycle;    // clockticks6502 at that write
	// Attribute bytes whose value actually changed to produce this generation.
	// Not the number of stores the CPU made: a handler that rewrites all eight
	// bytes when only three differ shows 3 here and 8 in summary.mutations. The
	// gap between those two is redundant work the guest paid for.
	uint16_t writes;
	uint16_t first_line;     // first scanline it drew on (SPRITE_TRACE_NO_LINE if never)
	uint16_t last_line;      // last scanline it drew on
	uint16_t lines_resident; // scanlines it was the slot's value, drawn or not
	uint16_t lines_drawn;    // scanlines it actually put pixels on
	uint32_t budget_cycles;  // VERA sprite-fetch cycles it consumed over the frame
	// Decoded once at open, so consumers do not each re-derive it.
	int16_t  x, y;
	uint16_t width, height;
	uint32_t address;
	uint8_t  zdepth, palette_offset, color_mode, collision_mask;
	bool     hflip, vflip;
} sprite_gen_t;

// One CPU write into the sprite attribute table.
typedef struct {
	uint32_t cycle;   // clockticks6502 when it landed
	uint16_t line;    // display scanline the beam was on, or SPRITE_TRACE_NO_LINE off-screen
	uint8_t  slot;
	uint8_t  offset;  // 0..7, byte within the sprite's attributes
	uint8_t  old_val;
	uint8_t  new_val;
} sprite_mutation_t;

// What one scanline's sprite rendering cost and whether it fit.
typedef struct {
	uint16_t budget_used;   // clocks actually used, never above the ceiling
	uint32_t demand;        // clocks the line wanted, which may exceed it
	uint16_t evaluated;     // slots the renderer got to before time ran out
	uint16_t drawn;         // slots that put pixels on this line
	uint16_t dropped;       // slots on this line that time ran out before drawing
	uint16_t cut_slot;      // first slot time ran out in, or SPRITE_TRACE_NO_LINE
	uint32_t cpu_cycles;    // CPU cycles the guest spent while the beam was on this line
	bool     exhausted;     // the line ran out of time, so later slots were dropped
	bool     rendered;      // render_sprite_line() ran for this line at all
} sprite_line_stat_t;

// Frame-level totals, all derived in sprite_trace_frame_end().
typedef struct {
	uint32_t frame;              // frame_count this describes
	uint32_t cpu_cycles;         // CPU cycles spanned by the frame

	uint16_t gen_count;          // generations recorded (>= number of slots used)
	uint16_t effective_sprites;  // generations that actually drew: the real answer to
	                             // "how many sprites did the player see this frame"
	uint16_t phantom_writes;     // generations written during the frame that never drew
	uint16_t late_writes;        // generations written after their own Y span had passed
	uint16_t slots_used;         // slots that drew at least once
	uint16_t slots_multiplexed;  // slots that held more than one drawn value
	uint16_t max_gens_in_slot;   // deepest multiplexing on any one slot

	uint32_t mutations;          // attribute-byte writes seen
	uint32_t mutations_onscreen; // ...of which landed while the beam was in the display
	uint32_t mutation_bursts;    // runs of writes separated by <= burst_gap cycles
	uint32_t mutation_cycles;    // CPU cycles spanned by those runs
	uint32_t burst_gap;          // the gap threshold those two were computed with

	uint16_t lines_exhausted;    // scanlines that ran out of sprite render time
	uint32_t sprites_dropped;    // slot/line pairs that time ran out before drawing
	uint32_t budget_used_total;  // summed sprite render clocks over the frame
	uint16_t peak_line;          // scanline that wanted the most render time
	uint16_t peak_budget;        // that line's clocks actually used
	uint32_t peak_demand;        // that line's clocks wanted, ceiling or not
	bool     multiplexing;       // any slot held more than one drawn value
} sprite_frame_summary_t;

// A complete recording of one frame. Returned by sprite_trace_last_frame() and
// stable until the next frame is finished.
typedef struct {
	sprite_frame_summary_t summary;
	const sprite_gen_t    *gens;
	uint16_t               gen_count;
	const sprite_mutation_t *mutations;
	uint32_t                 mutation_count;
	uint32_t                 mutations_dropped; // lost to the ring buffer's capacity
	const sprite_line_stat_t *lines;            // [SPRITE_TRACE_LINES]
	// Residency map. line_gen[line * SPRITE_TRACE_SLOTS + slot] indexes gens[],
	// or is SPRITE_TRACE_NO_GEN when the renderer never reached that slot.
	const uint16_t *line_gen;
	const uint8_t  *line_flags;   // SPRITE_TRACE_* per (line, slot)
	const uint16_t *line_budget;  // per (line, slot) sprite-fetch cycles
} sprite_trace_frame_t;

#define SPRITE_TRACE_NO_GEN 0xFFFFu

// Set by sprite_trace_set_enabled(). Exposed directly so the renderer's inner
// loop can branch on a load instead of a call: render_sprite_line() consults it
// 128 times per scanline, 480 scanlines per frame.
extern bool sprite_trace_active;

// Turning tracing on allocates the maps (about 300 KB per buffer, two buffers);
// turning it off frees them. A build that never opens the panel therefore pays
// nothing. Returns false if the allocation failed, leaving tracing off.
bool sprite_trace_set_enabled(bool on);
void sprite_trace_reset(void);

// Bytes currently held by the trace, for the panel to report.
size_t sprite_trace_memory_usage(void);

// The gap, in CPU cycles, below which two consecutive attribute writes are
// treated as part of the same burst. Changing it re-derives mutation_bursts and
// mutation_cycles on the next frame.
void     sprite_trace_set_burst_gap(uint32_t cycles);
uint32_t sprite_trace_get_burst_gap(void);

// ─── Recording hooks, called from video.c ──────────────────────────────────

// Finish the frame in progress and start a new one. attrs is the live attribute
// table, used to seed each slot's carried-over generation.
void sprite_trace_frame_advance(uint32_t frame, uint32_t cpu_cycle,
                                const uint8_t attrs[SPRITE_TRACE_SLOTS][8]);

// A CPU write into the attribute table. Call after the byte has been stored.
void sprite_trace_note_write(uint8_t slot, uint8_t offset, uint8_t old_val,
                             uint8_t new_val, uint16_t line, uint32_t cpu_cycle);

// render_sprite_line() is starting on this line. Clears the line, so that a
// line rendered more than once records only its final pass -- matching the
// display, where the last pass is what remains on screen.
void sprite_trace_line_begin(uint16_t line, uint32_t cpu_cycle);

// The renderer read this slot on this line. attr is the live attribute table
// row, budget_used is what this sprite consumed, flags is SPRITE_TRACE_*.
void sprite_trace_note_slot(uint16_t line, uint8_t slot, const uint8_t attr[8],
                            uint16_t budget_used, uint8_t flags);

// render_sprite_line() has finished this line. `result` carries the whole line
// tally; the caller fills in what it measured and leaves cpu_cycles and
// rendered to the trace.
void sprite_trace_line_end(uint16_t line, const sprite_line_stat_t *result);

// ─── Reading ───────────────────────────────────────────────────────────────

// The most recently completed frame, or NULL if none has finished since tracing
// was enabled. Stable until the next frame boundary.
const sprite_trace_frame_t *sprite_trace_last_frame(void);

#ifdef __cplusplus
}
#endif

#endif // SPRITE_TRACE_H
