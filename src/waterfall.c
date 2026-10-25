// Commander X16 Emulator — frame waterfall recorder. See waterfall.h.

#include "waterfall.h"

#include <stdlib.h>
#include <string.h>

#define MAX_QUEUE 4         // writes from one instruction (RMW: two)
#define AGG_SIZE  (1 << 16) // aggregate table slots
#define MAX_DEPTH 256       // tree path length the walk tracks

int wf_pending = 0;

static struct {
	int      lane;
	uint16_t target;
	uint8_t  value, flags;
} queue[MAX_QUEUE];

static int        ring_n, worst_n, frame_cap;
static wf_frame_t cur;                // the frame being recorded
static uint64_t   cur_start;
static wf_frame_t *ring;              // ring_n slots
static int        ring_head, ring_count;
static wf_frame_t *worst;             // worst_n slots, worst first
static int        worst_count;
static uint32_t   frames_seen, dropped;

static struct {
	uint32_t key_lane, writes;
} *agg;
static wf_agg_t *agg_sorted;

static const char *const lane_names[WF_LANES] = {
	"CPU",     "VERA reg",  "PSG",      "Palette",  "Sprites",    "L0 map", "L0 tiles", "L1 map",
	"L1 tiles", "VRAM other", "PCM",     "SPI/SD",   "YM2151",     "VIA1",   "VIA2",     "I/O other",
};

const char *
wf_lane_name(int lane)
{
	return lane >= 0 && lane < WF_LANES ? lane_names[lane] : "?";
}

// ─── Frames ─────────────────────────────────────────────────────────────────

static bool
frame_alloc(wf_frame_t *f)
{
	memset(f, 0, sizeof *f);
	f->events = malloc(sizeof *f->events * (size_t)frame_cap);
	return f->events != NULL;
}

static void
frame_copy(wf_frame_t *dst, const wf_frame_t *src)
{
	wf_event_t *keep = dst->events;
	*dst             = *src;
	dst->events      = keep;
	memcpy(dst->events, src->events, sizeof *src->events * (size_t)src->count);
}

static void
append(uint32_t cycle, uint32_t key_lane, uint16_t target, uint8_t value, uint8_t flags)
{
	if (cur.count >= frame_cap) {
		cur.truncated = true;
		return;
	}
	cur.events[cur.count++] = (wf_event_t){cycle, key_lane, target, value, flags};
}

// ─── Aggregates ─────────────────────────────────────────────────────────────

static void
agg_add(uint32_t key_lane)
{
	uint32_t h = (key_lane * 0x9E3779B1u) >> 16;
	for (int probe = 0; probe < AGG_SIZE; probe++, h = (h + 1) & (AGG_SIZE - 1)) {
		if (agg[h].writes && agg[h].key_lane == key_lane) {
			agg[h].writes++;
			return;
		}
		if (!agg[h].writes) {
			agg[h].key_lane = key_lane;
			agg[h].writes   = 1;
			return;
		}
	}
	dropped++;
}

static int
cmp_agg(const void *a, const void *b)
{
	const wf_agg_t *x = a, *y = b;
	if (x->writes != y->writes)
		return x->writes < y->writes ? 1 : -1;
	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->lane - y->lane;
}

// ─── API ────────────────────────────────────────────────────────────────────

bool
wf_init(int ring_frames, int worst_frames, int cap)
{
	wf_free();
	ring_n    = ring_frames > 0 ? ring_frames : 1;
	worst_n   = worst_frames > 0 ? worst_frames : 0;
	frame_cap = cap > 16 ? cap : 16;
	ring      = calloc((size_t)ring_n, sizeof *ring);
	worst     = calloc((size_t)(worst_n ? worst_n : 1), sizeof *worst);
	agg       = calloc(AGG_SIZE, sizeof *agg);
	bool ok   = ring && worst && agg && frame_alloc(&cur);
	for (int i = 0; ok && i < ring_n; i++)
		ok = frame_alloc(&ring[i]);
	for (int i = 0; ok && i < worst_n; i++)
		ok = frame_alloc(&worst[i]);
	if (!ok) {
		wf_free();
		return false;
	}
	wf_reset();
	return true;
}

void
wf_free(void)
{
	for (int i = 0; ring && i < ring_n; i++)
		free(ring[i].events);
	for (int i = 0; worst && i < worst_n; i++)
		free(worst[i].events);
	free(ring);
	free(worst);
	free(cur.events);
	free(agg);
	free(agg_sorted);
	ring = worst = NULL;
	agg        = NULL;
	agg_sorted = NULL;
	memset(&cur, 0, sizeof cur);
	ring_n = worst_n = 0;
	wf_pending = 0;
}

void
wf_reset(void)
{
	if (!agg)
		return;
	memset(agg, 0, sizeof *agg * AGG_SIZE);
	cur.count     = 0;
	cur.truncated = false;
	cur.number     = 0;
	cur.start      = 0;
	cur.video_mode = 1; // VGA until told otherwise
	cur.start_node = 0;
	cur_start      = 0;
	ring_head = ring_count = worst_count = 0;
	frames_seen = dropped = 0;
	wf_pending            = 0;
}

void
wf_note(int lane, uint16_t target, uint8_t value, uint8_t flags)
{
	if (!agg)
		return;
	if (wf_pending == MAX_QUEUE) {
		dropped++;
		return;
	}
	queue[wf_pending].lane   = lane;
	queue[wf_pending].target = target;
	queue[wf_pending].value  = value;
	queue[wf_pending].flags  = flags;
	wf_pending++;
}

void
wf_flush_(uint32_t key, bool irq, uint64_t t)
{
	const uint32_t cycle = t > cur_start ? (uint32_t)(t - cur_start) : 0;
	for (int i = 0; i < wf_pending; i++) {
		const uint32_t kl = (key & 0xFFFFFFu) | ((uint32_t)queue[i].lane << 24);
		append(cycle, kl, queue[i].target, queue[i].value,
		       (uint8_t)(queue[i].flags | (irq ? WF_F_IRQ : 0)));
		agg_add(kl);
	}
	wf_pending = 0;
}

void
wf_cpu(uint16_t node, uint64_t t)
{
	if (!agg)
		return;
	const uint32_t cycle = t > cur_start ? (uint32_t)(t - cur_start) : 0;
	append(cycle, (uint32_t)WF_LANE_CPU << 24, node, 0, 0);
}

void
wf_frame_set_video_mode(uint8_t mode)
{
	cur.video_mode = mode & 3;
}

void
wf_frame_end(uint64_t t, uint64_t score, uint16_t node)
{
	if (!agg)
		return;
	// A write still queued belongs to an instruction that never got stamped
	// (the capture is ending mid-step): count it, do not misattribute it.
	dropped += (uint32_t)wf_pending;
	wf_pending = 0;

	cur.length = (uint32_t)(t - cur_start);
	cur.score  = score ? score : (uint64_t)cur.count;

	// Into the ring, over the oldest.
	frame_copy(&ring[ring_head], &cur);
	ring_head = (ring_head + 1) % ring_n;
	if (ring_count < ring_n)
		ring_count++;

	// Into the worst list if it beats the least bad one there.
	if (worst_n) {
		int at = worst_count < worst_n ? worst_count : -1;
		if (at < 0 && cur.score > worst[worst_n - 1].score)
			at = worst_n - 1;
		if (at >= 0) {
			while (at > 0 && worst[at - 1].score < cur.score) {
				// Shift down, swapping buffers rather than copying events.
				wf_event_t *buf = worst[at].events;
				worst[at]        = worst[at - 1];
				worst[at - 1].events = buf;
				at--;
			}
			frame_copy(&worst[at], &cur);
			if (worst_count < worst_n)
				worst_count++;
		}
	}

	frames_seen++;
	cur.number     = frames_seen;
	cur.start      = t;
	cur.count      = 0;
	cur.truncated  = false;
	cur.start_node = node;
	cur_start      = t;
}

int
wf_ring_count(void)
{
	return ring_count;
}

const wf_frame_t *
wf_ring_at(int i)
{
	if (i < 0 || i >= ring_count)
		return NULL;
	const int oldest = (ring_head - ring_count + ring_n) % ring_n;
	return &ring[(oldest + i) % ring_n];
}

int
wf_worst_count(void)
{
	return worst_count;
}

const wf_frame_t *
wf_worst_at(int i)
{
	return (i >= 0 && i < worst_count) ? &worst[i] : NULL;
}

uint32_t wf_frames_seen(void) { return frames_seen; }
uint32_t wf_dropped(void)     { return dropped; }

const wf_agg_t *
wf_aggregates(int *count)
{
	*count = 0;
	free(agg_sorted);
	agg_sorted = NULL;
	if (!agg)
		return NULL;
	int n = 0;
	for (int i = 0; i < AGG_SIZE; i++)
		n += agg[i].writes != 0;
	agg_sorted = malloc(sizeof *agg_sorted * (size_t)(n ? n : 1));
	if (!agg_sorted)
		return NULL;
	n = 0;
	for (int i = 0; i < AGG_SIZE; i++)
		if (agg[i].writes)
			agg_sorted[n++] = (wf_agg_t){agg[i].key_lane & 0xFFFFFFu, (int)(agg[i].key_lane >> 24),
			                             agg[i].writes};
	qsort(agg_sorted, (size_t)n, sizeof *agg_sorted, cmp_agg);
	*count = n;
	return agg_sorted;
}

// ─── Ranges ─────────────────────────────────────────────────────────────────

void
wf_frame_range(const wf_frame_t *f, uint32_t c0, uint32_t c1, wf_range_node_fn on_node,
               wf_range_write_fn on_write, void *ctx)
{
	if (!f || c1 <= c0)
		return;
	// The innermost node is a step function: start_node from 0, then each
	// CPU event switches it. Each stretch is credited for its overlap.
	int      node = f->start_node;
	uint32_t from = 0;
	for (int i = 0; i <= f->count; i++) {
		const bool       last = i == f->count;
		const wf_event_t *e   = last ? NULL : &f->events[i];
		if (!last && wf_event_lane(e) != WF_LANE_CPU) {
			if (on_write && e->cycle >= c0 && e->cycle < c1)
				on_write(ctx, e);
			continue;
		}
		const uint32_t to = last ? (f->length > from ? f->length : from) : e->cycle;
		const uint32_t a = from > c0 ? from : c0, b = to < c1 ? to : c1;
		if (on_node && b > a)
			on_node(ctx, node, b - a);
		if (last)
			break;
		node = e->target;
		from = e->cycle;
	}
}

// ─── Export walk ────────────────────────────────────────────────────────────

typedef struct {
	int path[MAX_DEPTH]; // root first
	int depth;
} stack_t_;

// Root-first path to `node`, into `out`; returns its length.
static int
path_to(int node, wf_parent_fn parent_of, void *ctx, int *out)
{
	int tmp[MAX_DEPTH], n = 0;
	for (int x = node; x >= 0 && n < MAX_DEPTH; x = parent_of(ctx, x))
		tmp[n++] = x;
	for (int i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

// Make `s` (thread `th`) hold exactly `path`: end what diverges, begin the rest.
static void
retarget(stack_t_ *s, int th, const int *path, int n, uint32_t cycle, wf_emit_fn emit, void *ctx)
{
	int common = 0;
	while (common < s->depth && common < n && s->path[common] == path[common])
		common++;
	while (s->depth > common)
		emit(ctx, WF_OUT_END, th, cycle, s->path[--s->depth], NULL);
	while (s->depth < n) {
		s->path[s->depth] = path[s->depth];
		emit(ctx, WF_OUT_BEGIN, th, cycle, path[s->depth], NULL);
		s->depth++;
	}
}

void
wf_walk_frame(const wf_frame_t *f, wf_parent_fn parent_of, int root_irq, wf_emit_fn emit, void *ctx)
{
	static stack_t_ st[2]; // static: 2 KB each, and the walk is not re-entered
	st[0].depth = st[1].depth = 0;
	int path[MAX_DEPTH];

	// Enter the node the frame started in.
	int n = path_to(f->start_node, parent_of, ctx, path);
	retarget(&st[n && path[0] == root_irq ? 1 : 0], n && path[0] == root_irq ? 1 : 0, path, n, 0,
	         emit, ctx);

	for (int i = 0; i < f->count; i++) {
		const wf_event_t *e = &f->events[i];
		if (wf_event_lane(e) != WF_LANE_CPU) {
			emit(ctx, WF_OUT_EVENT, (e->flags & WF_F_IRQ) ? 1 : 0, e->cycle, -1, e);
			continue;
		}
		n            = path_to(e->target, parent_of, ctx, path);
		const bool q = n && path[0] == root_irq;
		if (q) {
			retarget(&st[1], 1, path, n, e->cycle, emit, ctx); // main stays open
		} else {
			retarget(&st[1], 1, path, 0, e->cycle, emit, ctx); // handlers are over
			retarget(&st[0], 0, path, n, e->cycle, emit, ctx);
		}
	}
	retarget(&st[1], 1, path, 0, f->length, emit, ctx);
	retarget(&st[0], 0, path, 0, f->length, emit, ctx);
}
