// Commander X16 Emulator — frame waterfall recorder for capture mode.
//
// A timeline of each frame, one lane per resource: which routine the CPU was
// in (from the call tree), and every write to VERA, VRAM, the PSG, the YM, the
// VIAs and the rest of the I/O page -- each stamped with the instruction that
// made it, as an effective (bank, PC) key like perf_hist's, so every mark can
// be traced to source.
//
// THREE LAYERS, because a busy program writes VERA tens of thousands of times a
// frame:
//
//   1. AGGREGATES for the whole capture: writes per (source key, lane). Size
//      depends on the code, not on how long it runs. "Which routine writes the
//      palette most?"
//   2. A SUMMARY of every frame -- its work, writes, length -- always kept.
//      This is what "worst", "median" and "90th percentile" are computed from.
//   3. DETAIL (every event) for every frame, within a memory budget. Each
//      frame's events are kept at their own size. If a long capture would go
//      over, detail is thinned evenly (every 2nd frame, then every 4th, ...)
//      but never for the WF_PROTECT busiest and quietest frames by either
//      measure. A frame that overflows the per-frame buffer while recording is
//      flagged truncated rather than quietly cut; one whose detail could not be
//      kept still has its summary.
//
// TIMING. Writes happen inside an instruction, before its cycles are known.
// They are queued, and wf_flush() -- called from perf_hist_step_ with the
// instruction's start time and key -- stamps them. Routine changes come from
// the call tree (profile_cct.h) once per instant, with the final innermost
// node. Times are perf_hist's 64-bit running cycle count; within a frame,
// events carry cycles since the frame began.
//
// Pure: no emulator globals. Unit-tested in tests/test_waterfall.c.
#ifndef WATERFALL_H
#define WATERFALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	WF_LANE_CPU = 0,     // target = call-tree node now innermost
	WF_LANE_VERA_REG,    // VERA registers except the data ports, PCM and SPI
	WF_LANE_PSG,         // VRAM $1F9C0-$1F9FF
	WF_LANE_PALETTE,     // VRAM $1FA00-$1FBFF
	WF_LANE_SPRITES,     // VRAM $1FC00-$1FFFF (sprite attributes)
	WF_LANE_L0_MAP,
	WF_LANE_L0_TILES,    // tiles, font, or bitmap
	WF_LANE_L1_MAP,
	WF_LANE_L1_TILES,
	WF_LANE_VRAM_OTHER,  // VRAM no layer's map or tiles cover
	WF_LANE_PCM,         // $9F3D audio FIFO
	WF_LANE_SPI,         // $9F3E/$9F3F: the SD card
	WF_LANE_YM,          // target = YM register
	WF_LANE_VIA1,
	WF_LANE_VIA2,
	WF_LANE_IO_OTHER,    // emulator registers, MIDI, expansion
	WF_LANES
} wf_lane_t;

const char *wf_lane_name(int lane);

#define WF_F_VRAM_HI 0x01 // VRAM address bit 16 (target holds bits 0-15)
#define WF_F_FX4     0x02 // a VERA FX cache write: four bytes from one store
#define WF_F_IRQ     0x04 // made inside an interrupt handler

// 12 bytes. `key_lane` = perf_hist key (22 bits) | lane << 24.
typedef struct {
	uint32_t cycle;    // since the frame began
	uint32_t key_lane;
	uint16_t target;   // register, VRAM offset, or tree node (CPU lane)
	uint8_t  value;
	uint8_t  flags;    // WF_F_*
} wf_event_t;

static inline uint32_t wf_event_key(const wf_event_t *e) { return e->key_lane & 0xFFFFFFu; }
static inline int      wf_event_lane(const wf_event_t *e) { return (int)(e->key_lane >> 24); }

// Not a device: marks in the CPU's own story, kept with the events but never
// counted as writes. `target` says which.
#define WF_LANE_MARK 0xFF
enum {
	WF_MARK_HELD_ON = 0, // an interrupt was requested but the I flag held it off...
	WF_MARK_HELD_OFF,    // ...until here (taken, withdrawn, or the frame ended)
};
static inline bool wf_event_is_mark(const wf_event_t *e) { return wf_event_lane(e) == WF_LANE_MARK; }

typedef struct {
	uint32_t    number;     // frames since the capture began, from 0
	uint64_t    start;      // perf_hist cycle count when it began
	uint64_t    work;       // CPU cycles working, not waiting (0 if not measured)
	uint32_t    writes;     // device writes in the frame, including any not kept
	uint32_t    length;     // cycles the frame lasted
	uint16_t    start_node; // innermost tree node when the frame began
	uint8_t     video_mode; // VERA DC_VIDEO output mode (0 off, 1 VGA, 2 NTSC, 3 RGB)
	bool        truncated;  // events past the buffer were dropped
	int         count;      // events recorded
	wf_event_t *events;     // NULL when this frame's detail was not kept
} wf_frame_t;

// How many of the busiest and of the quietest frames, by each measure, always
// keep their detail.
#define WF_PROTECT 8

typedef struct {
	uint32_t key;   // perf_hist key
	int      lane;
	uint32_t writes;
} wf_agg_t;

// Record up to `frame_cap` events a frame, and keep detail within
// `budget_bytes` in all. False if out of memory.
bool wf_init(int frame_cap, size_t budget_bytes);
void wf_free(void);
void wf_reset(void); // forget everything; keep the sizes

// ---- Recording ---------------------------------------------------------
// A device write during the current instruction; stamped by wf_flush().
void wf_note(int lane, uint16_t target, uint8_t value, uint8_t flags);

extern int wf_pending; // queued writes; read by perf_hist_step_ on every step

// Stamp queued writes with the instruction that made them: its key, whether
// it ran in an interrupt handler, and the time it started.
void wf_flush_(uint32_t key, bool irq, uint64_t t);
static inline void
wf_flush(uint32_t key, bool irq, uint64_t t)
{
	if (wf_pending)
		wf_flush_(key, irq, t);
}

// The innermost call-tree node changed at time `t`.
void wf_cpu(uint16_t node, uint64_t t);

// An interrupt line is asserted after instruction number `instr`, which ended
// at time `t`; `held` when the I flag kept it from being taken. Called for
// every such instruction, so a gap in `instr` means the line was withdrawn.
// Only main-line code holds an interrupt off in this sense: the caller does not
// report a line still up inside a handler (that is the request being served).
void wf_irq_line(bool held, uint64_t t, uint32_t instr);
// Close a held-off stretch if instruction `instr` (the latest) did not see the
// line held: called at a frame's end, before wf_frame_end().
void wf_irq_poll(uint32_t instr);

// VERA's output mode for the frame being recorded (DC_VIDEO bits 0-1). It
// decides what a scanline is: VGA and NTSC time their frames differently.
// Sticky: carries to later frames until set again.
void wf_frame_set_video_mode(uint8_t mode);

// A frame ended at time `t`, having done `work` CPU cycles of work (not
// waiting; 0 when not measured). `node` is the innermost tree node, which the
// next frame starts in.
void wf_frame_end(uint64_t t, uint64_t work, uint16_t node);

// ---- Queries -----------------------------------------------------------
int               wf_frame_count(void);   // every frame ended since the reset
const wf_frame_t *wf_frame(int i);       // 0 = first; events NULL when not kept
uint32_t          wf_frames_seen(void);  // more than wf_frame_count() only if memory ran out
int               wf_detail_count(void); // frames whose events are kept
size_t            wf_detail_bytes(void);
uint32_t          wf_dropped(void);      // writes lost to a full queue or table

// Picking frames. A measure to rank by, and which frames to pick: every one,
// the WF_PROTECT worst (highest) or best (lowest) first, or the one frame at
// the median, at the 90th percentile, or closest to the mean. Ties go to the
// earlier frame. Writes frame indices into `out` and returns how many.
enum { WF_BY_WORK = 0, WF_BY_WRITES };
enum { WF_PICK_ALL = 0, WF_PICK_WORST, WF_PICK_BEST, WF_PICK_MEDIAN, WF_PICK_P90, WF_PICK_AVERAGE };
uint64_t wf_metric_of(const wf_frame_t *f, int metric);
int      wf_pick(int pick, int metric, int *out, int max);
// The nearest frame to `i` whose detail is kept (i itself if it is), or -1.
int      wf_nearest_detail(int i);

// ─── Saving and loading (a saved profile run) ───────────────────────────────
// Write everything the recorder holds: every frame's summary and kept detail,
// and the aggregates. False on a write error.
bool wf_write(FILE *f);
// Read what wf_write() wrote, from a buffer holding exactly that, into a copy
// nothing uses yet; NULL if it is malformed. wf_install() then replaces the
// recorder's state with it and frees it -- the memory budget is not applied,
// the run is shown as it was saved. wf_staged_free() discards it instead.
// wf_init() must have succeeded before wf_install().
typedef struct wf_staged wf_staged_t;
wf_staged_t *wf_read(const uint8_t *buf, size_t len);
bool         wf_install(wf_staged_t *s);
void         wf_staged_free(wf_staged_t *s);

// Aggregates, heaviest first. The returned array lives until the next call
// or wf_reset(). `*count` is set to its length.
const wf_agg_t *wf_aggregates(int *count);

// ---- Export walk ---------------------------------------------------------
// Turn one frame into properly nested begin/end spans plus instant events,
// two threads: 0 = main line, 1 = interrupts. Main-line spans stay open while
// an interrupt runs; interrupt spans close when the main line resumes.
// `parent_of(node)` is the tree's parent (-1 at a root); `root_irq` is the
// node that roots interrupt paths.
typedef enum { WF_OUT_BEGIN, WF_OUT_END, WF_OUT_EVENT, WF_OUT_MARK } wf_out_kind_t;
typedef void (*wf_emit_fn)(void *ctx, wf_out_kind_t kind, int thread, uint32_t cycle,
                           int node, const wf_event_t *ev);
typedef int (*wf_parent_fn)(void *ctx, int node);
void wf_walk_frame(const wf_frame_t *f, wf_parent_fn parent_of, int root_irq, wf_emit_fn emit,
                   void *ctx);

// ---- A range of one frame ------------------------------------------------
// What happened in cycles [c0, c1) of a frame: for each call-tree node that
// was innermost there, the cycles it was innermost for (in time order, one
// call per contiguous stretch); and every device write in the range. Either
// callback may be NULL.
typedef void (*wf_range_node_fn)(void *ctx, int node, uint32_t cycles);
typedef void (*wf_range_write_fn)(void *ctx, const wf_event_t *ev);
void wf_frame_range(const wf_frame_t *f, uint32_t c0, uint32_t c1, wf_range_node_fn on_node,
                    wf_range_write_fn on_write, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
