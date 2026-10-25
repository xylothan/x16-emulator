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

static int        frame_cap;
static wf_frame_t cur;                // the frame being recorded, frame_cap events
static uint64_t   cur_start;
static wf_frame_t *frames;            // every frame ended, oldest first
static int        frame_n, frame_alloc_n;
static uint32_t   frames_seen, dropped;

// Detail is kept for frame i when i % stride == 0, or when it is one of the
// protected extremes. The stride doubles whenever the budget would be passed.
static size_t     budget, detail_bytes;
static int        detail_n, stride;

// The WF_PROTECT highest and lowest frames by each measure, best-placed
// first: [metric][0 = highest, 1 = lowest].
static int        ext[2][2][WF_PROTECT];
static int        ext_n[2][2];

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

static size_t
detail_size(const wf_frame_t *f)
{
	return sizeof *f->events * (size_t)(f->count ? f->count : 1);
}

static void
drop_detail(int i)
{
	wf_frame_t *f = &frames[i];
	if (!f->events)
		return;
	detail_bytes -= detail_size(f);
	detail_n--;
	free(f->events);
	f->events = NULL;
}

static bool
is_protected(int i)
{
	for (int m = 0; m < 2; m++)
		for (int hl = 0; hl < 2; hl++)
			for (int k = 0; k < ext_n[m][hl]; k++)
				if (ext[m][hl][k] == i)
					return true;
	return false;
}

static bool
wanted(int i)
{
	return i % stride == 0 || is_protected(i);
}

// Frame i has just been summarised: place it among the extremes. A frame
// pushed out loses its detail unless something else still wants it.
static void
place_extremes(int i)
{
	for (int m = 0; m < 2; m++) {
		const uint64_t v = wf_metric_of(&frames[i], m);
		for (int hl = 0; hl < 2; hl++) {
			int *list = ext[m][hl], *n = &ext_n[m][hl];
			// Ties go to the earlier frame, so a later one must beat strictly.
			int at = *n;
			while (at > 0) {
				const uint64_t o = wf_metric_of(&frames[list[at - 1]], m);
				if (hl == 0 ? v > o : v < o)
					at--;
				else
					break;
			}
			if (at >= WF_PROTECT)
				continue;
			int out = -1;
			if (*n == WF_PROTECT)
				out = list[WF_PROTECT - 1];
			else
				(*n)++;
			memmove(&list[at + 1], &list[at], sizeof *list * (size_t)(*n - 1 - at));
			list[at] = i;
			if (out >= 0 && !wanted(out))
				drop_detail(out);
		}
	}
}

// Thin evenly until `need` more bytes fit: double the stride and drop what it
// no longer reaches.
static void
make_room(size_t need)
{
	while (detail_bytes + need > budget && stride < frame_n) {
		stride *= 2;
		for (int i = 0; i < frame_n; i++)
			if (frames[i].events && !wanted(i))
				drop_detail(i);
	}
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
wf_init(int cap, size_t budget_bytes)
{
	wf_free();
	frame_cap  = cap > 16 ? cap : 16;
	budget     = budget_bytes;
	agg        = calloc(AGG_SIZE, sizeof *agg);
	cur.events = malloc(sizeof *cur.events * (size_t)frame_cap);
	if (!agg || !cur.events) {
		wf_free();
		return false;
	}
	wf_reset();
	return true;
}

static void
free_frames(void)
{
	for (int i = 0; i < frame_n; i++)
		free(frames[i].events);
	frame_n = detail_n = 0;
	detail_bytes       = 0;
}

void
wf_free(void)
{
	free_frames();
	free(frames);
	frames        = NULL;
	frame_alloc_n = 0;
	free(cur.events);
	free(agg);
	free(agg_sorted);
	agg        = NULL;
	agg_sorted = NULL;
	memset(&cur, 0, sizeof cur);
	wf_pending = 0;
}

void
wf_reset(void)
{
	if (!agg)
		return;
	memset(agg, 0, sizeof *agg * AGG_SIZE);
	free_frames();
	memset(ext_n, 0, sizeof ext_n);
	stride         = 1;
	cur.count      = 0;
	cur.writes     = 0;
	cur.truncated  = false;
	cur.number     = 0;
	cur.start      = 0;
	cur.video_mode = 1; // VGA until told otherwise
	cur.start_node = 0;
	cur_start      = 0;
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
		cur.writes++;
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
wf_frame_end(uint64_t t, uint64_t work, uint16_t node)
{
	if (!agg)
		return;
	// A write still queued belongs to an instruction that never got stamped
	// (the capture is ending mid-step): count it, do not misattribute it.
	dropped += (uint32_t)wf_pending;
	wf_pending = 0;

	cur.length = (uint32_t)(t - cur_start);
	cur.work   = work;

	// The summary, always (unless memory has run out entirely).
	if (frame_n == frame_alloc_n) {
		const int   want = frame_alloc_n ? frame_alloc_n * 2 : 1024;
		wf_frame_t *grow = realloc(frames, sizeof *frames * (size_t)want);
		if (grow) {
			frames        = grow;
			frame_alloc_n = want;
		}
	}
	if (frame_n < frame_alloc_n) {
		const int i  = frame_n++;
		frames[i]    = cur;
		frames[i].events = NULL;
		place_extremes(i);
		// The detail, if this frame is wanted and it fits.
		if (wanted(i)) {
			const size_t need = detail_size(&frames[i]);
			make_room(need);
			if (wanted(i) && detail_bytes + need <= budget) {
				frames[i].events = malloc(need);
				if (frames[i].events) {
					memcpy(frames[i].events, cur.events, sizeof *cur.events * (size_t)cur.count);
					detail_bytes += need;
					detail_n++;
				}
			}
		}
	}

	frames_seen++;
	cur.number     = frames_seen;
	cur.start      = t;
	cur.count      = 0;
	cur.writes     = 0;
	cur.truncated  = false;
	cur.start_node = node;
	cur_start      = t;
}

int
wf_frame_count(void)
{
	return frame_n;
}

const wf_frame_t *
wf_frame(int i)
{
	return (i >= 0 && i < frame_n) ? &frames[i] : NULL;
}

uint32_t wf_frames_seen(void)  { return frames_seen; }
uint32_t wf_dropped(void)      { return dropped; }
int      wf_detail_count(void) { return detail_n; }
size_t   wf_detail_bytes(void) { return detail_bytes; }

// ─── Picking ────────────────────────────────────────────────────────────────

uint64_t
wf_metric_of(const wf_frame_t *f, int metric)
{
	return metric == WF_BY_WRITES ? f->writes : f->work;
}

static int sort_metric;

static int
cmp_by_metric(const void *a, const void *b)
{
	const int      x = *(const int *)a, y = *(const int *)b;
	const uint64_t u = wf_metric_of(&frames[x], sort_metric), v = wf_metric_of(&frames[y], sort_metric);
	if (u != v)
		return u < v ? -1 : 1;
	return x - y;
}

int
wf_pick(int pick, int metric, int *out, int max)
{
	if (max <= 0 || frame_n == 0)
		return 0;
	metric = metric == WF_BY_WRITES ? WF_BY_WRITES : WF_BY_WORK;
	switch (pick) {
	case WF_PICK_ALL: {
		const int n = frame_n < max ? frame_n : max;
		for (int i = 0; i < n; i++)
			out[i] = i;
		return n;
	}
	case WF_PICK_WORST:
	case WF_PICK_BEST: {
		const int hl = pick == WF_PICK_WORST ? 0 : 1;
		const int n  = ext_n[metric][hl] < max ? ext_n[metric][hl] : max;
		memcpy(out, ext[metric][hl], sizeof *out * (size_t)n);
		return n;
	}
	case WF_PICK_MEDIAN:
	case WF_PICK_P90: {
		int *idx = malloc(sizeof *idx * (size_t)frame_n);
		if (!idx)
			return 0;
		for (int i = 0; i < frame_n; i++)
			idx[i] = i;
		sort_metric = metric;
		qsort(idx, (size_t)frame_n, sizeof *idx, cmp_by_metric);
		// Nearest rank: the median is the middle (lower of two), the 90th
		// percentile the smallest frame at or above 90% of the others.
		const int rank = pick == WF_PICK_MEDIAN ? (frame_n - 1) / 2 : (9 * frame_n + 9) / 10 - 1;
		out[0]         = idx[rank];
		free(idx);
		return 1;
	}
	case WF_PICK_AVERAGE: {
		double sum = 0;
		for (int i = 0; i < frame_n; i++)
			sum += (double)wf_metric_of(&frames[i], metric);
		const double mean = sum / frame_n;
		int          best = 0;
		double       gap  = -1;
		for (int i = 0; i < frame_n; i++) {
			const double d = (double)wf_metric_of(&frames[i], metric) - mean;
			const double g = d < 0 ? -d : d;
			if (gap < 0 || g < gap) {
				gap  = g;
				best = i;
			}
		}
		out[0] = best;
		return 1;
	}
	}
	return 0;
}

int
wf_nearest_detail(int i)
{
	for (int d = 0; d < frame_n; d++) {
		if (i - d >= 0 && i - d < frame_n && frames[i - d].events)
			return i - d;
		if (d && i + d >= 0 && i + d < frame_n && frames[i + d].events)
			return i + d;
	}
	return -1;
}

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
