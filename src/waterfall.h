// Commander X16 Emulator — frame waterfall recorder for capture mode.
//
// A timeline of each frame, one lane per resource: which routine the CPU was
// in (from the call tree), and every write to VERA, VRAM, the PSG, the YM, the
// VIAs and the rest of the I/O page -- each stamped with the instruction that
// made it, as an effective (bank, PC) key like perf_hist's, so every mark can
// be traced to source.
//
// TWO LAYERS, because a busy program writes VERA tens of thousands of times a
// frame:
//
//   1. AGGREGATES for the whole capture: writes per (source key, lane). Size
//      depends on the code, not on how long it runs. "Which routine writes the
//      palette most?"
//   2. DETAIL for a bounded set of frames: a ring of the last N, plus the
//      worst K by a score the caller supplies (work cycles, or event count),
//      kept after they leave the ring. A frame that overflows its buffer is
//      flagged truncated rather than quietly cut.
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
#include <stdint.h>

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

typedef struct {
	uint32_t    number;     // frames since the capture began, from 0
	uint64_t    start;      // perf_hist cycle count when it began
	uint64_t    score;      // what the worst-K list ranks by
	uint32_t    length;     // cycles the frame lasted
	uint16_t    start_node; // innermost tree node when the frame began
	bool        truncated;  // events past the buffer were dropped
	int         count;
	wf_event_t *events;
} wf_frame_t;

typedef struct {
	uint32_t key;   // perf_hist key
	int      lane;
	uint32_t writes;
} wf_agg_t;

// `ring` detailed frames, `worst` kept by score, up to `frame_cap` events
// each. False if out of memory.
bool wf_init(int ring, int worst, int frame_cap);
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

// A frame ended at time `t`. `score` ranks it for the worst list (0: rank by
// event count); `node` is the innermost tree node, which the next frame
// starts in.
void wf_frame_end(uint64_t t, uint64_t score, uint16_t node);

// ---- Queries -----------------------------------------------------------
int               wf_ring_count(void);
const wf_frame_t *wf_ring_at(int i);   // 0 = oldest
int               wf_worst_count(void);
const wf_frame_t *wf_worst_at(int i);  // 0 = worst
uint32_t          wf_frames_seen(void);
uint32_t          wf_dropped(void);    // writes lost to a full queue or table

// Aggregates, heaviest first. The returned array lives until the next call
// or wf_reset(). `*count` is set to its length.
const wf_agg_t *wf_aggregates(int *count);

// ---- Export walk ---------------------------------------------------------
// Turn one frame into properly nested begin/end spans plus instant events,
// two threads: 0 = main line, 1 = interrupts. Main-line spans stay open while
// an interrupt runs; interrupt spans close when the main line resumes.
// `parent_of(node)` is the tree's parent (-1 at a root); `root_irq` is the
// node that roots interrupt paths.
typedef enum { WF_OUT_BEGIN, WF_OUT_END, WF_OUT_EVENT } wf_out_kind_t;
typedef void (*wf_emit_fn)(void *ctx, wf_out_kind_t kind, int thread, uint32_t cycle,
                           int node, const wf_event_t *ev);
typedef int (*wf_parent_fn)(void *ctx, int node);
void wf_walk_frame(const wf_frame_t *f, wf_parent_fn parent_of, int root_irq, wf_emit_fn emit,
                   void *ctx);

#ifdef __cplusplus
}
#endif

#endif
