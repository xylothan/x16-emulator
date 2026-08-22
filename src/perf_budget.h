// Commander X16 Emulator — guest performance budget accounting.
//
// THE PROBLEM: "how much of my frame am I using?" has no direct answer on a
// 6502, because the CPU never idles. At 8 MHz a VGA frame is exactly
//
//     MHZ * 1e6 * (VGA_SCAN_WIDTH * SCAN_HEIGHT) / PIXEL_FREQ
//       = 8e6 * (800 * 525) / 25e6  =  134,400 cycles  (256 per scanline)
//
// and the machine burns every one of them whether the program is doing work or
// spinning on a vsync flag. Raw cycles-per-frame is therefore always 100% and
// says nothing. The number a developer actually wants is WORK versus WAITING,
// so this module has to classify cycles rather than merely count them.
//
// HOW CYCLES ARE CLASSIFIED, highest precedence first:
//
//   1. GUEST MARKERS ($9FBC). The program itself says where its frame work
//      begins and ends. Exact, and the only layer that needs no guessing --
//      but it needs the guest to cooperate, so it is opt-in.
//   2. ZONES. Address ranges named in the debugger or over DAP. A zone flagged
//      idle declares "cycles here are waiting", which is the manual override
//      for a wait loop the heuristic below cannot see.
//   3. AUTO. WAI, plus a store-free tight-loop detector. Zero setup, and right
//      for the two idioms that dominate X16 code:
//          lda $9F27 / and #1 / beq -      (poll VERA's ISR)
//          lda vsync_flag / beq -          (poll a flag an IRQ handler sets)
//
// WHAT THE AUTO LAYER CANNOT SEE: a wait loop that stores to memory or calls a
// subroutine -- `jsr GETIN / cmp #0 / beq -` is the common one -- reads as
// work, because "performs no stores" is the only cheap signal that separates a
// spin from a computation. That is not a bug to be fixed by a cleverer
// heuristic; it is why layers 1 and 2 exist. Mark such a range as an idle zone
// and it is counted correctly.
//
// The detector also re-anchors whenever an interrupt is taken, so a wait loop
// interrupted by its own vsync handler loses up to two passes before it is
// confirmed again. Two passes of a four-instruction loop is on the order of
// twenty cycles against a budget of 134,400, so the error is not worth carrying
// state to avoid.
//
// WHY THE BUDGET IS NOT THE FRAME PERIOD: `target_fps` asks how many vsyncs the
// program's frame work is allowed to span, because the frame period itself is
// fixed by VERA and cannot be changed by wanting it to be. Asking for 30 fps
// does not make frames longer; it means the work may take two of them, so the
// budget is two frames' worth of cycles.
//
// A target FASTER than the machine scans is a different question -- "this
// routine has to fit in half a frame" -- and gets a fractional budget instead.
// Between the two, a target within 5% of a whole number of vsyncs is snapped
// onto it: "60" on a 59.52 Hz machine plainly means "every frame", not "0.8%
// faster than the hardware can scan", and reporting the latter would put every
// saturated program permanently over budget for no reason a user would accept.
//
// WHY THIS FILE HAS NO EMULATOR DEPENDENCIES: it is unit-tested on its own,
// with no CPU, no memory, no video and no SDL -- see tests/test_perf_budget.c.
// Everything it needs about the machine arrives as a function argument or
// through perf_budget_configure(). It reaches for no globals, exactly as
// io_trace.c and cpu/irq_ctx.c do, and for the same reason.
//
// COST WHEN OFF: one load and one predictable branch per instruction, from the
// inline guards below. Nothing is allocated until profiling is switched on.
// COST WHEN ON: an add, one array index for the zone lookup, and a handful of
// branches -- the same order as the code_map_record_current() call the debugger
// already makes on every instruction.
//
// THREADING: the emulator steps the CPU, polls the DAP server and renders the
// debugger on one thread, so none of this needs locking. Do not call into it
// from the SDL audio callback.
#ifndef _PERF_BUDGET_H_
#define _PERF_BUDGET_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Zones the user may define. The ring keeps per-frame history for the first
// PERF_RING_ZONES of them; the rest still accumulate into the current frame and
// are reported live, they just have no history to take percentiles over. The
// split exists so that the ring stays a fixed, modest size no matter how many
// zones somebody defines.
#define PERF_MAX_ZONES   16
#define PERF_RING_ZONES  8
#define PERF_ZONE_NAME_MAX 32

// How far apart the lowest and highest PC of a candidate spin loop may be. A
// vsync poll is three or four instructions; 32 bytes covers that with room to
// spare while still excluding anything that could be called a computation.
#define PERF_SPIN_SPAN 32

// A frame boundary is detected AFTER the instruction that crossed it, so a
// frame's accounted cycles can overshoot the nominal period by up to one
// instruction. Without a tolerance, a program that simply uses the whole
// machine reads as "over budget by 3 cycles" on the frames where the boundary
// happens to land late -- which is measurement granularity reported as a
// defect. This is the longest instruction on the 65C816, in cycles.
#define PERF_BOUNDARY_SLACK 8

// Nesting depth for guest marker regions. Deep enough for a main loop that
// brackets a subsystem that brackets a routine, which is as far as anyone
// instruments by hand.
#define PERF_MARKER_STACK 8

// Frames the ring holds by default: sixty seconds at 60 Hz, which is the
// longest window the statistics offer. Exposed so that a caller restoring the
// default, and the tests, cannot drift from what the module actually uses.
#define PERF_DEFAULT_CAPACITY 3600

// The value $9FBC reads back, so guest code can detect that it is running under
// an emulator that understands the marker protocol.
#define PERF_MARKER_PROTOCOL 1

typedef enum {
	PERF_IDLE_AUTO = 0, // WAI + the spin-loop detector (default)
	PERF_IDLE_MARKERS,  // trust only guest markers and idle zones
	PERF_IDLE_NONE,     // classify nothing as idle; work == total
} perf_idle_mode_t;

// One completed frame. 56 bytes, so a 3600-sample ring -- sixty seconds at
// 60 Hz -- costs about 200 KB, and only while profiling is on.
typedef struct {
	uint32_t frame;        // monotonic ordinal, never reused
	uint32_t total;        // every cycle in the frame period
	uint32_t idle;         // classified as waiting
	uint32_t irq;          // cycles taken with an interrupt handler open
	uint32_t instructions; // instructions retired
	uint32_t host_us;      // host wall-clock spent on this frame
	// Work summed over the trailing budget period -- the quantity the budget is
	// actually measured against. Identical to (total - idle) at 60 fps, where
	// the period is one frame; at 30 fps it is this frame's work plus the
	// previous frame's, because that is the whole span the work was allowed.
	uint32_t period_work;
	uint32_t zone[PERF_RING_ZONES];
} perf_frame_t;

// work == total - idle. Derived rather than stored, so the two can never
// disagree.
static inline uint32_t
perf_frame_work(const perf_frame_t *f)
{
	return f->total - f->idle;
}

typedef struct {
	uint32_t min, mean, p50, p95, p99, max;
} perf_stat_t;

typedef struct {
	float       seconds;       // window requested; <= 0 means the whole ring
	int         frames;        // samples actually in it
	uint32_t    budget_cycles; // what each frame was measured against
	perf_stat_t work;          // work cycles per frame
	perf_stat_t budget_work;   // work per budget period; == work at 60 fps
	perf_stat_t total;         // total cycles per frame
	perf_stat_t irq;           // interrupt cycles per frame
	perf_stat_t host_us;       // host time per frame
	int         overruns;      // frames whose work exceeded the budget
	float       overrun_pct;   // as a percentage of `frames`
	uint32_t    worst_overrun; // cycles over budget, at the worst frame
	uint32_t    worst_frame;   // which frame that was
	float       mean_utilization; // work as a percentage of the budget
	float       p95_utilization;
	float       max_utilization;
} perf_window_t;

// A bucket cycles are attributed to. Two kinds share the table so that the
// debugger and DAP have ONE breakdown to show rather than two that have to be
// read side by side: an address range the user drew around some code, and a
// region the guest itself opened with a marker write. They differ only in how
// membership is decided -- a PC test versus an explicit enter/leave -- which is
// not a difference worth duplicating the whole reporting surface over.
typedef struct {
	char     name[PERF_ZONE_NAME_MAX];
	uint16_t start;     // inclusive; unused for a marker region
	uint16_t end;       // exclusive; unused for a marker region
	int16_t  bank;      // < 0 matches any bank
	bool     is_idle;   // cycles here are waiting, not work
	bool     is_marker; // opened by the guest rather than bounded by address
	uint8_t  marker_id; // which $9FBC region id, when is_marker
	bool     used;
} perf_zone_t;

// What the DAP overrun event reports. Drained rather than polled, so the
// server can coalesce a burst of bad frames into one message.
typedef struct {
	uint32_t frames;   // frames elapsed since the previous drain
	uint32_t overruns;
	uint32_t worst_overrun;
	uint32_t worst_frame;
	float    worst_utilization;
} perf_overrun_report_t;

// ─── The hot path ───────────────────────────────────────────────────────────
// Read directly by the inline guards, so flipping it takes effect on the next
// instruction. Use perf_budget_set_enabled(), which also allocates the ring.
extern bool perf_budget_enabled;

void perf_budget_step_(uint32_t clocks, uint16_t pc, uint8_t pbank, bool waiting, int irq_depth);
void perf_budget_note_store_(void);

// Call once per instruction, AFTER the step, with the PC of the instruction
// that just ran -- not regs.pc, which by then addresses the next one.
// `irq_depth` is cpu_irq_depth(); `waiting` is the CPU's WAI state.
static inline void
perf_budget_step(uint32_t clocks, uint16_t pc, uint8_t pbank, bool waiting, int irq_depth)
{
	if (perf_budget_enabled)
		perf_budget_step_(clocks, pc, pbank, waiting, irq_depth);
}

// Call on every CPU store. The spin detector needs to know that a loop had a
// side effect, which is what separates waiting from computing.
static inline void
perf_budget_note_store(void)
{
	if (perf_budget_enabled)
		perf_budget_note_store_();
}

// Close the current frame and push it into the ring. `host_us` is the host
// wall-clock time the frame took, or 0 where nobody measured it.
void perf_budget_frame_end(uint32_t host_us);

// ─── Configuration ──────────────────────────────────────────────────────────
// Who has asked for profiling. It runs while ANY owner wants it, so a DAP
// client polling statistics is not switched off the moment somebody closes the
// debugger panel, and closing the panel does not strand a client with a dead
// feed. This mirrors the ownership rule the core already applies to
// breakpoints, and for the same reason: two consumers, one piece of state, and
// neither is entitled to revoke the other's.
typedef enum {
	PERF_OWNER_UI = 0,  // the ImGui Performance panel
	PERF_OWNER_DAP,     // a debug adapter client
	PERF_OWNER_COUNT
} perf_owner_t;

// Returns false only if the ring could not be allocated.
bool perf_budget_arm(perf_owner_t owner, bool want);
bool perf_budget_owner_wants(perf_owner_t owner);

// The unconditional form, ignoring ownership. Switching this on allocates the
// ring; off frees it and discards history. Returns false if the allocation
// failed, in which case profiling stays off.
bool perf_budget_set_enabled(bool enabled);
bool perf_budget_is_enabled(void);

// The machine's own parameters. `machine_khz` is the native clock (NOT the
// current speed override -- a budget measured against a machine slowed to
// 1 MHz for debugging would be meaningless), `cycles_per_frame` the cycles in
// one vsync period, `scanlines_per_frame` the total including blanking.
void perf_budget_configure(uint32_t machine_khz, uint32_t cycles_per_frame,
                           uint16_t scanlines_per_frame);

void  perf_budget_set_target_fps(float fps);
float perf_budget_get_target_fps(void);
float perf_budget_vsync_hz(void);

uint32_t perf_budget_cycles_per_frame(void);
uint32_t perf_budget_cycles_per_scanline(void);
uint32_t perf_budget_budget_cycles(void);  // what work is measured against
int      perf_budget_frames_per_budget(void); // vsyncs the budget spans

void             perf_budget_set_idle_mode(perf_idle_mode_t mode);
perf_idle_mode_t perf_budget_get_idle_mode(void);

// Ring capacity in frames. Resizing discards history. Returns false on a failed
// allocation, leaving the previous ring in place.
bool perf_budget_set_capacity(int frames);
int  perf_budget_capacity(void);

void perf_budget_reset(void); // forget all history; keep the configuration

// ─── Guest markers ($9FBC) ──────────────────────────────────────────────────
//   $01-$3F  enter region n            $80|n  leave region n
//   $00      leave the innermost       $F0    frame work begins
//   $F1      frame work ends           $FE    reset all statistics
//
// Any marker write puts the module in marker mode for the rest of the session,
// which suppresses the heuristic entirely: a program that says where its work
// is should not also be guessed at. A region id first seen here claims a zone
// slot named "region <n>", so marker regions and address zones appear in one
// breakdown rather than two.
//
// Unbalanced writes are tolerated rather than rejected, because the alternative
// is a debugging aid that itself needs debugging: leaving a region that is not
// open is ignored, and any region still open when the frame ends is closed by
// the frame boundary. A guest that forgets a leave therefore mis-attributes one
// frame, not every frame after it.
void    perf_budget_marker_write(uint8_t value);
uint8_t perf_budget_marker_read(void);

// ─── Zones ──────────────────────────────────────────────────────────────────
// Returns the zone index, or -1 if the table is full or the range is empty.
int  perf_budget_zone_add(const char *name, uint16_t start, uint16_t end,
                          int16_t bank, bool is_idle);
bool perf_budget_zone_remove(int index);
void perf_budget_zone_clear(void);
int  perf_budget_zone_count(void);   // highest index in use + 1
bool perf_budget_zone_get(int index, perf_zone_t *out);

// ─── Queries ────────────────────────────────────────────────────────────────
// All of these are read-only and safe to call while the machine runs. The
// percentiles are computed on demand, so a window nobody asks for costs
// nothing.
bool perf_budget_last_frame(perf_frame_t *out);
bool perf_budget_current_frame(perf_frame_t *out); // the frame in progress
int  perf_budget_frame_count(void);                // samples retained
uint32_t perf_budget_total_frames(void);           // frames ever completed

// One retained sample, indexed from the NEWEST: 0 is the frame just completed,
// 1 the one before it. Indexing from the newest rather than the oldest is what
// a caller actually wants -- "the last 240 frames" does not change meaning as
// the ring fills, and does not shift under a reader between calls.
bool perf_budget_frame_at(int newest_index, perf_frame_t *out);

// `seconds` <= 0 means the whole ring. False when there is no history.
bool perf_budget_window(float seconds, perf_window_t *out);

// Per-zone cycles over a window. Only zones below PERF_RING_ZONES have history;
// higher ones report their current frame and zeroes elsewhere.
bool perf_budget_zone_window(int zone, float seconds, perf_stat_t *out);

// Overruns since the previous drain. False when there have been none.
bool perf_budget_drain_overruns(perf_overrun_report_t *out);

#ifdef __cplusplus
}
#endif

#endif // _PERF_BUDGET_H_
