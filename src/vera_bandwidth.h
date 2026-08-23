// Commander X16 Emulator — VERA VRAM bandwidth accounting.
//
// THE PROBLEM: src/perf_budget.h answers "how much of my frame is my CODE
// using". It says nothing about VERA, and VERA is the other half of the
// question -- a program can have cycles to spare and still tear, drop sprites
// or run out of time to push pixels through the data port. This module
// measures the VRAM bus: what the layers fetch, what the CPU pushes through
// $9F23/$9F24, and what VERA FX multiplies that into.
//
// ─── What the bus actually is ───────────────────────────────────────────────
//
// Quoted from the RTL, which for an FPGA is not a description of the hardware
// but IS the hardware. Pinned to X16Community/vera-module tag v47.0.2, the
// same commit tests/fetch_vera_rtl.py fetches; run it to check these.
//
//   * ONE VRAM bus, 32 bits wide, one access per clock. main_ram.v:7-10 takes
//     a 15-bit word address and 32 bits of data, so AN ACCESS IS FOUR BYTES.
//   * A scanline is 800 clocks. video_vga.v:27-31 -- 640 active + 16 front
//     porch + 96 sync + 48 back porch. 525 lines a frame (video_vga.v:33-37).
//   * FIXED PRIORITY, not round-robin: vram_if.v:142-157 is a combinatorial
//     if/else-if chain, if0 > if1 > if2 > if3. top.v:680-713 wires those to
//     the CPU-and-FX port ("Interface 0 - 8-bit (highest priority)"), layer 0,
//     layer 1 and the sprite renderer, in that order. SPRITES COME LAST.
//   * An access costs TWO clocks: the FETCH state raises the strobe and the
//     WAIT_FETCH state takes the ack one clock later
//     (layer_renderer.v:250-271).
//
// WHY THE HEADLINE IS PER-SCANLINE AND NOT "PERCENT OF PEAK": 25 MHz x 4 bytes
// is 100 MB/s, and a real program uses perhaps a fifth of it. Reporting that
// would produce a number that is always low and always useless -- the same
// mistake as reporting raw cycles-per-frame, which is always 100%. The 800
// clocks of ONE SCANLINE is the window that can genuinely run out, and at full
// load it exactly does:
//
//     layer 0, 8bpp bitmap 640   160 fetches x 2 = 320 clocks
//     layer 1, 8bpp bitmap 640   160 fetches x 2 = 320 clocks
//     sprites, 4bpp, full line                   = 160 clocks
//                                                  ----
//                                                   800  = H_TOTAL
//
// ─── Why this models the registers instead of counting reads ────────────────
//
// The obvious implementation -- count video_space_read() calls in the render
// path -- measures the wrong machine. The emulator's renderer is a PIXEL loop:
// it walks SCREEN_WIDTH columns and reads a byte whenever it crosses a byte
// boundary, so at 320-wide it reads the same byte repeatedly, and it never
// fetches 32 bits at once the way the hardware does. Those call counts are an
// artefact of the emulator's inner loop, not VERA's bus traffic.
//
// So the fetch count is derived instead: a closed form over the layer's mode,
// colour depth and tile geometry that reproduces the RTL's own decode tables
// (layer_renderer.v:42-51 pixels_per_word_minus1, :68-77
// words_per_line_minus1). It is both more faithful and cheaper than
// instrumenting every read.
//
// ─── WHERE THIS MODEL IS WRONG ──────────────────────────────────────────────
//
// Read this part before trusting a number out of it.
//
//   1. FETCHES ARE MODELLED, NOT OBSERVED. If the emulator's renderer and the
//      RTL disagree about what a mode fetches, this follows the RTL. That is
//      the intent -- the question is "what would VERA do", not "what did this
//      emulator's convenience code do" -- but it means a bug in the emulator's
//      renderer will NOT show up here.
//
//   2. CLOCKS ARE A LOWER BOUND; ACCESSES ARE EXACT. Two clocks per access is
//      the uncontended cost. Under real contention a lower-priority client
//      holds its strobe and waits, so it burns MORE clocks than this charges.
//      Trust the access counts; treat the clock totals as "at least this bad".
//
//   3. THE SCROLL TILE IS AN ASSUMPTION. A layer scrolled mid-tile needs one
//      more tile than the arithmetic gives. This charges that extra tile
//      whenever hscroll is not tile-aligned. The RTL's exact edge behaviour at
//      the last partially-visible tile was not pinned down, so the count can
//      be one tile high or low at the margin.
//
//   4. A LINE IS FETCHED DURING THE LINE BEFORE IT. The layer line buffer is
//      double-buffered (top.v active_line_buf_r, layer_line_buffer.v) and
//      composer.v starts the render for line N+1 as line N begins. This module
//      reports a line's fetches against the line they PAINT, not the line they
//      happen on, because that is where a developer looks for them.
//
//   5. SPRITE HEADROOM IS NOT COMPUTED HERE. This module deliberately knows
//      nothing about sprite_trace.c. It reports what the layers and the CPU
//      took, and leaves "so how much was left for sprites" to the consumer,
//      which is what keeps this file dependency-free. The Performance panel
//      crosses the two.
//
//   6. FX IS COVERED ONLY WHERE THE EMULATOR IMPLEMENTS IT. addr_data.v is
//      59 KB of pipeline; this charges the paths video.c actually has -- the
//      four-byte cache write and the affine prefetch's second fetch -- and
//      does not claim to be a whole FX performance model.
//
// WHY THIS FILE HAS NO EMULATOR DEPENDENCIES: it is unit-tested on its own,
// with no CPU, no memory, no video and no SDL -- see
// tests/test_vera_bandwidth.c. Everything it needs arrives as a function
// argument or through vera_bandwidth_configure(), exactly as perf_budget.c,
// io_trace.c and cpu/irq_ctx.c do, and for the same reason.
//
// AND NO <math.h>: lrintf and fabsf are intrinsics on MSVC but real libm
// symbols on glibc, which broke the Linux link for every target that pulls in
// memory.c. Everything here is integer arithmetic.
//
// COST WHEN OFF: one load and one predictable branch, from the inline guards
// below. The guards test the flag BEFORE evaluating anything, because C
// evaluates arguments before the callee can decline them.
//
// THREADING: same rule as perf_budget.h -- one thread steps the CPU, polls DAP
// and draws the debugger, so none of this locks. Do not call it from the SDL
// audio callback.
#ifndef _VERA_BANDWIDTH_H_
#define _VERA_BANDWIDTH_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clocks in one scanline: video_vga.v:27-31, 640 + 16 + 96 + 48. This is the
// window everything below is measured against.
#define VERA_BW_LINE_CLOCKS 800

// Bytes moved by one VRAM access. main_ram.v:7-10 is 32 bits wide, so a "fetch"
// is always four bytes even when the client wanted one.
#define VERA_BW_BYTES_PER_ACCESS 4

// Clocks one access occupies the bus for: FETCH raises the strobe, WAIT_FETCH
// takes the ack on the next clock (layer_renderer.v:250-271). Uncontended --
// see limit 2 in the header comment.
#define VERA_BW_ACCESS_CLOCKS 2

// Active display lines, and the ceiling on lines we keep per-frame detail for.
// VERA scans 525 (video_vga.v:33-37) but only 480 of them fetch.
#define VERA_BW_LINES 480

// Pixels the layer renderer always produces before it stops, whatever the
// display scale: layer_renderer.v:488, lb_wridx_r[9:7] == 3'b101 is 640.
#define VERA_BW_LAYER_PIXELS 640

// One layer's configuration for one scanline, in the terms the RTL decodes.
// Deliberately not video.c's struct video_layer_properties: this module is
// tested without video.c, and taking only these five fields is what makes that
// possible.
typedef struct {
	bool    enabled;     // layer switched on in DC_VIDEO
	bool    bitmap_mode;
	uint8_t color_depth; // 0..3, meaning 1, 2, 4 and 8 bpp
	uint8_t tile_width;  // 0 => 8px tiles / 320px bitmap, 1 => 16px / 640px
	bool    scrolled;    // hscroll is not tile-aligned, so one extra tile shows
} vera_bw_layer_t;

// What one scanline cost. Written per line and REPLACED, never accumulated: a
// mid-line register split makes video.c render the same line more than once and
// the last pass is what the display keeps, exactly as sprite_trace does.
typedef struct {
	uint16_t layer_fetches[2]; // 32-bit accesses, per layer
	uint16_t port_accesses;    // CPU data-port accesses that landed on this line
	bool     rendered;         // this line was drawn at all
} vera_bw_line_t;

// Clocks a line's traffic occupied the bus for, against VERA_BW_LINE_CLOCKS.
// Derived rather than stored, so it cannot drift out of step with the counts it
// is made of -- the same reason perf_frame_work() is derived.
static inline uint32_t
vera_bw_line_clocks(const vera_bw_line_t *l)
{
	return ((uint32_t)l->layer_fetches[0] + l->layer_fetches[1] + l->port_accesses) *
	       VERA_BW_ACCESS_CLOCKS;
}

// One completed frame.
typedef struct {
	uint32_t frame;

	// ─ Layers ─
	uint32_t layer_fetches[2]; // 32-bit accesses over the frame
	uint32_t layer_bytes[2];   // the same, times four
	uint16_t layer_peak_line[2];
	uint16_t layer_peak_fetches[2];

	// ─ The CPU's data port, which is the part a program controls ─
	uint32_t port_reads;   // bytes the guest read through $9F23/$9F24
	uint32_t port_writes;  // bytes the guest wrote
	uint32_t port_accesses;// VRAM accesses those cost
	uint32_t port_vram_bytes; // bytes that actually reached VRAM (FX multiplies)

	// ─ VERA FX ─
	uint32_t fx_cache_writes;   // one store that wrote four bytes
	uint32_t fx_affine_fetches; // the extra map read an affine read costs

	// ─ Per-scanline pressure, which is the headline ─
	uint16_t peak_line;    // busiest scanline
	uint16_t peak_clocks;  // its clocks, against VERA_BW_LINE_CLOCKS
	uint16_t p95_clocks;   // 95th percentile over the active lines
	uint16_t mean_clocks;
	// Lines with any bus traffic at all: rendered by a layer, touched by the
	// CPU port, or both. NOT just "lines a layer drew" -- a program that
	// switches the layers off and blasts VRAM through $9F23 is still using the
	// bus, and a denominator that ignored it would report that frame as idle.
	uint16_t lines_active;
	uint16_t lines_over;   // lines whose layer+CPU demand exceeded the window
	uint32_t total_clocks;
	uint32_t total_fetches;
} vera_bw_frame_t;

// ─── The model itself ───────────────────────────────────────────────────────

// VRAM accesses one layer performs for one scanline. Pure: no state, no
// globals, safe to call from anywhere, and the reason this module is testable.
//
// Tile modes fetch a map entry every two tiles, because a 32-bit word holds two
// 16-bit entries (layer_renderer.v:113, and the every-other-tile FETCH_MAP at
// :305), plus words_per_line_minus1 + 1 words of pixel data per tile
// (layer_renderer.v:68-77).
//
// Bitmap modes fetch no map at all, just the pixel words the width needs at
// pixels_per_word_minus1 + 1 pixels each (layer_renderer.v:42-51).
uint16_t vera_bandwidth_layer_line_fetches(const vera_bw_layer_t *layer);

// Pixels one 32-bit word holds in a given mode: layer_renderer.v:42-51.
// Exposed because the tests check it directly against the RTL's table.
uint8_t vera_bandwidth_pixels_per_word(bool bitmap_mode, uint8_t color_depth,
                                       uint8_t tile_width);

// Words of pixel data per tile row: layer_renderer.v:68-77.
uint8_t vera_bandwidth_words_per_tile_line(bool bitmap_mode, uint8_t color_depth,
                                           uint8_t tile_width);

// ─── The hot path ───────────────────────────────────────────────────────────
// Read directly by the inline guards, so flipping it takes effect immediately.
extern bool vera_bandwidth_active;

void vera_bandwidth_line_(uint16_t line, const vera_bw_layer_t *l0,
                          const vera_bw_layer_t *l1);
void vera_bandwidth_note_port_(bool write, uint8_t vram_bytes, uint16_t line);
void vera_bandwidth_note_fx_cache_write_(uint16_t line);
void vera_bandwidth_note_fx_affine_(uint16_t line);

// A scanline was rendered with these two layer configurations. Replaces
// whatever the line held, so re-rendering a line after a mid-line register
// write records the final pass rather than the sum of the passes.
static inline void
vera_bandwidth_line(uint16_t line, const vera_bw_layer_t *l0, const vera_bw_layer_t *l1)
{
	if (vera_bandwidth_active)
		vera_bandwidth_line_(line, l0, l1);
}

// One byte moved through $9F23/$9F24. `vram_bytes` is what actually reached
// VRAM, which is 1 for an ordinary access and 4 for an FX cache write.
//
// Do NOT call this for the debugger's own reads. video.c short-circuits those
// on debugOn, and charging them would mean opening a memory view changed the
// number the developer is trying to read.
static inline void
vera_bandwidth_note_port(bool write, uint8_t vram_bytes, uint16_t line)
{
	if (vera_bandwidth_active)
		vera_bandwidth_note_port_(write, vram_bytes, line);
}

// An FX cache write: one CPU store that put four bytes into VRAM.
static inline void
vera_bandwidth_note_fx_cache_write(uint16_t line)
{
	if (vera_bandwidth_active)
		vera_bandwidth_note_fx_cache_write_(line);
}

// An affine-mode read, which costs a second fetch to translate the tile map.
static inline void
vera_bandwidth_note_fx_affine(uint16_t line)
{
	if (vera_bandwidth_active)
		vera_bandwidth_note_fx_affine_(line);
}

// Close the frame in progress and publish it. Cheap enough to call
// unconditionally; it returns immediately when accounting is off.
void vera_bandwidth_frame_end(uint32_t frame);

// ─── Configuration ──────────────────────────────────────────────────────────
// Ownership mirrors perf_budget: accounting runs while ANY owner wants it, so
// closing the panel does not strand a DAP client and a client disconnecting
// does not switch off a panel somebody is reading.
typedef enum {
	VERA_BW_OWNER_UI = 0,
	VERA_BW_OWNER_DAP,
	VERA_BW_OWNER_COUNT
} vera_bw_owner_t;

void vera_bandwidth_arm(vera_bw_owner_t owner, bool want);
bool vera_bandwidth_owner_wants(vera_bw_owner_t owner);

void vera_bandwidth_set_enabled(bool enabled);
bool vera_bandwidth_is_enabled(void);
void vera_bandwidth_reset(void);

// Lines the machine actually scans, so a consumer can report the right
// denominator without reaching into video.c.
void     vera_bandwidth_configure(uint16_t scanlines_per_frame);
uint16_t vera_bandwidth_scanlines_per_frame(void);

// ─── Reading ────────────────────────────────────────────────────────────────
// The most recently completed frame. False when none has finished since
// accounting was switched on.
bool vera_bandwidth_last_frame(vera_bw_frame_t *out);

// The frame still being drawn, for a live readout that does not wait for the
// boundary. Percentiles over a partial frame cover the lines drawn so far.
bool vera_bandwidth_current_frame(vera_bw_frame_t *out);

// Per-line detail for the most recently completed frame. Stable until the next
// frame boundary. Returns NULL when there is no completed frame.
//
// This covers the ACTIVE lines only. Vertical blanking is real bus time and the
// CPU can use it -- that traffic is in the frame totals, but it has no line to
// be drawn against, so it is not in here.
const vera_bw_line_t *vera_bandwidth_last_lines(uint16_t *count);

#ifdef __cplusplus
}
#endif

#endif // _VERA_BANDWIDTH_H_
