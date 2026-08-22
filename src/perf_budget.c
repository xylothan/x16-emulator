// Commander X16 Emulator — guest performance budget accounting.
// See perf_budget.h for what this measures and why it has to classify cycles
// rather than merely count them.

#include "perf_budget.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Rounding and absolute value, done by hand rather than with <math.h>.
//
// perf_budget.c is linked into unit tests that link nothing else -- that is the
// whole point of it taking the machine as arguments -- and it is also linked
// into memory.c's test targets. lrintf() and fabsf() are intrinsics on MSVC but
// real libm symbols on glibc, so using them meant every one of those targets
// had to grow a -lm it does not otherwise need. Both values rounded here are
// positive, so the naive form is exact and obviously correct.
static uint32_t
round_pos(float v)
{
	return (uint32_t)(v + 0.5f);
}

static float
abs_f(float v)
{
	return v < 0.0f ? -v : v;
}

bool perf_budget_enabled = false;

// ─── Configuration ──────────────────────────────────────────────────────────

static uint32_t cfg_machine_khz      = 8000;
static uint32_t cfg_cycles_per_frame = 134400; // 8 MHz, VGA
static uint16_t cfg_scanlines        = 525;
static float    cfg_target_fps       = 60.0f;

static perf_idle_mode_t cfg_idle_mode = PERF_IDLE_AUTO;

// Derived from the above by recompute_budget().
static int      budget_frames = 1;      // vsyncs the budget spans
static uint32_t budget_cycles = 134400;

// ─── The ring ───────────────────────────────────────────────────────────────

static perf_frame_t *ring     = NULL;
static uint32_t     *scratch  = NULL; // percentile workspace, sized with the ring
static int           capacity = PERF_DEFAULT_CAPACITY;
static int           head     = 0; // next slot to write
static int           count    = 0; // valid samples
static uint32_t      total_frames = 0;

// ─── The frame in progress ──────────────────────────────────────────────────

static perf_frame_t cur;
static uint32_t     cur_zone[PERF_MAX_ZONES];

// Trailing work history, for budgets that span more than one frame. Only the
// last `budget_frames` entries matter and budget_frames is small, so a tiny
// ring of its own is cheaper and simpler than reaching back into the main one
// (which may be empty, or may have been resized, exactly when this is needed).
#define PERF_PERIOD_MAX 16
static uint32_t period_hist[PERF_PERIOD_MAX];
static int      period_pos = 0;

// ─── Spin detection ─────────────────────────────────────────────────────────

static uint32_t store_count = 0;

static struct {
	uint16_t anchor;
	uint16_t lo, hi;
	uint16_t last_pc;
	uint32_t stores_at_anchor;
	uint32_t pending; // cycles in the pass currently being accumulated
	uint8_t  iters;   // clean passes completed
	bool     valid;
	bool     have_last;
} spin;

// ─── Markers ────────────────────────────────────────────────────────────────

static bool    marker_mode = false; // a marker has been seen this session
static bool    marker_idle = false; // $F1 seen; work resumes at $F0
static int8_t  marker_stack[PERF_MARKER_STACK];
static int     marker_depth = 0;

// ─── Zones ──────────────────────────────────────────────────────────────────

static perf_zone_t zones[PERF_MAX_ZONES];
static uint8_t    *zone_of = NULL; // PC -> zone index + 1; 0 means none

// ─── Overrun accumulator ────────────────────────────────────────────────────

static perf_overrun_report_t pending_overruns;

// ────────────────────────────────────────────────────────────────────────────

static void
recompute_budget(void)
{
	if (cfg_cycles_per_frame == 0)
		cfg_cycles_per_frame = 1;

	const float vsync = perf_budget_vsync_hz();
	if (cfg_target_fps <= 0.0f || vsync <= 0.0f) {
		budget_frames = 1;
		budget_cycles = cfg_cycles_per_frame;
		return;
	}

	// Vsyncs per unit of work. Above 1 the work may span several frames; below
	// 1 the caller is asking for a routine to fit inside part of one.
	const float ratio = vsync / cfg_target_fps;
	int         n     = (int)round_pos(ratio);

	if (n >= 1 && abs_f(ratio - (float)n) <= 0.05f * (float)n) {
		// Close enough to a whole number of vsyncs to be what was meant.
		if (n > PERF_PERIOD_MAX)
			n = PERF_PERIOD_MAX;
		budget_frames = n;
		budget_cycles = cfg_cycles_per_frame * (uint32_t)n;
	} else {
		// A fraction of a frame, or an awkward multiple of one. Sum over as
		// many whole frames as the ratio covers, and scale the budget exactly.
		if (n < 1)
			n = 1;
		if (n > PERF_PERIOD_MAX)
			n = PERF_PERIOD_MAX;
		budget_frames = n;
		budget_cycles = round_pos((float)cfg_cycles_per_frame * ratio);
		if (budget_cycles == 0)
			budget_cycles = 1;
	}
}

// What a period's work is actually compared against: the budget, plus the
// boundary granularity described at PERF_BOUNDARY_SLACK.
static uint32_t
overrun_threshold(void)
{
	return budget_cycles + (uint32_t)budget_frames * PERF_BOUNDARY_SLACK;
}

static void
spin_reset(uint16_t pc, bool valid)
{
	spin.anchor           = pc;
	spin.lo               = pc;
	spin.hi               = pc;
	spin.stores_at_anchor = store_count;
	spin.pending          = 0;
	spin.iters            = 0;
	spin.valid            = valid;
}

static void
clear_current(void)
{
	memset(&cur, 0, sizeof cur);
	memset(cur_zone, 0, sizeof cur_zone);
	spin_reset(0, false);
	spin.have_last = false;
}

void
perf_budget_configure(uint32_t machine_khz, uint32_t cycles_per_frame, uint16_t scanlines_per_frame)
{
	if (machine_khz)
		cfg_machine_khz = machine_khz;
	if (cycles_per_frame)
		cfg_cycles_per_frame = cycles_per_frame;
	if (scanlines_per_frame)
		cfg_scanlines = scanlines_per_frame;
	recompute_budget();
}

void
perf_budget_set_target_fps(float fps)
{
	if (fps > 0.0f)
		cfg_target_fps = fps;
	recompute_budget();
}

float
perf_budget_get_target_fps(void)
{
	return cfg_target_fps;
}

float
perf_budget_vsync_hz(void)
{
	if (cfg_cycles_per_frame == 0)
		return 0.0f;
	return (float)cfg_machine_khz * 1000.0f / (float)cfg_cycles_per_frame;
}

uint32_t
perf_budget_cycles_per_frame(void)
{
	return cfg_cycles_per_frame;
}

uint32_t
perf_budget_cycles_per_scanline(void)
{
	return cfg_scanlines ? cfg_cycles_per_frame / cfg_scanlines : 0;
}

uint32_t
perf_budget_budget_cycles(void)
{
	return budget_cycles;
}

int
perf_budget_frames_per_budget(void)
{
	return budget_frames;
}

void
perf_budget_set_idle_mode(perf_idle_mode_t mode)
{
	cfg_idle_mode = mode;
}

perf_idle_mode_t
perf_budget_get_idle_mode(void)
{
	return cfg_idle_mode;
}

// ─── Enable / capacity ──────────────────────────────────────────────────────

static bool
alloc_ring(int frames)
{
	perf_frame_t *nr = (perf_frame_t *)calloc((size_t)frames, sizeof(perf_frame_t));
	if (!nr)
		return false;
	uint32_t *ns = (uint32_t *)calloc((size_t)frames, sizeof(uint32_t));
	if (!ns) {
		free(nr);
		return false;
	}
	free(ring);
	free(scratch);
	ring     = nr;
	scratch  = ns;
	capacity = frames;
	head     = 0;
	count    = 0;
	return true;
}

bool
perf_budget_set_enabled(bool enabled)
{
	if (!enabled) {
		perf_budget_enabled = false;
		free(ring);
		free(scratch);
		ring    = NULL;
		scratch = NULL;
		head = count = 0;
		clear_current();
		return true;
	}

	if (!ring && !alloc_ring(capacity))
		return false;

	clear_current();
	memset(period_hist, 0, sizeof period_hist);
	period_pos = 0;
	memset(&pending_overruns, 0, sizeof pending_overruns);
	perf_budget_enabled = true;
	return true;
}

bool
perf_budget_is_enabled(void)
{
	return perf_budget_enabled;
}

static bool arm_want[PERF_OWNER_COUNT];

bool
perf_budget_arm(perf_owner_t owner, bool want)
{
	if ((int)owner < 0 || (int)owner >= PERF_OWNER_COUNT)
		return false;
	arm_want[owner] = want;

	bool any = false;
	for (int i = 0; i < PERF_OWNER_COUNT; i++)
		any = any || arm_want[i];

	// Only act on a real change: perf_budget_set_enabled(true) restarts the
	// accumulator, so calling it every frame from a panel that is merely still
	// open would keep resetting the frame in progress.
	if (any == perf_budget_enabled)
		return true;
	return perf_budget_set_enabled(any);
}

bool
perf_budget_owner_wants(perf_owner_t owner)
{
	if ((int)owner < 0 || (int)owner >= PERF_OWNER_COUNT)
		return false;
	return arm_want[owner];
}

bool
perf_budget_set_capacity(int frames)
{
	if (frames < 2)
		frames = 2;
	if (frames == capacity && ring)
		return true;
	if (!ring) { // not allocated yet; remember it for when profiling starts
		capacity = frames;
		return true;
	}
	return alloc_ring(frames);
}

int
perf_budget_capacity(void)
{
	return capacity;
}

void
perf_budget_reset(void)
{
	head = count = 0;
	total_frames = 0;
	clear_current();
	memset(period_hist, 0, sizeof period_hist);
	period_pos = 0;
	memset(&pending_overruns, 0, sizeof pending_overruns);
	store_count = 0;

	// Marker state goes too. Marker mode suppresses the heuristic entirely, so
	// leaving it set would mean a program that used markers once could never
	// get automatic detection back -- not by resetting, not by reloading, not
	// by anything short of restarting the emulator. Since a guest that is still
	// driving markers re-asserts the mode on its very next write, clearing it
	// costs that guest nothing and gives every other one its detector back.
	marker_mode  = false;
	marker_idle  = false;
	marker_depth = 0;
}

// ─── The hot path ───────────────────────────────────────────────────────────

void
perf_budget_note_store_(void)
{
	store_count++;
}

// Track a candidate spin loop, and credit whole passes of a confirmed one to
// idle.
//
// A loop is identified by the BACKWARD JUMP that closes it, not by watching the
// PC drift within some window. That distinction matters: keying on drift means
// whatever address the walk happens to start at becomes the anchor, so a
// compact routine whose whole body fits inside the window is treated as one
// enormous "loop" that stores -- and the tight, store-free poll nested inside
// it is never seen at all. Anchoring on the branch target instead means each
// loop is recognised as itself, however tightly the code around it is packed.
//
// Classification happens at pass boundaries rather than per instruction so that
// a loop is never credited before it has proved itself; the cost is that the
// first confirmed pass is not counted, which is a handful of cycles against a
// frame of six figures.
static void
spin_track(uint32_t clocks, uint16_t pc, int irq_depth)
{
	// A handler is work by definition, and it also perturbs the loop it
	// interrupted, so re-anchor rather than trying to resume the pass.
	if (irq_depth > 0) {
		spin_reset(pc, false);
		spin.last_pc   = pc;
		spin.have_last = true;
		return;
	}

	if (!spin.valid)
		spin_reset(pc, true);

	const bool backward = spin.have_last && pc < spin.last_pc;

	if (backward) {
		if (pc == spin.anchor) {
			// The loop being watched has closed another pass.
			const bool clean = (spin.stores_at_anchor == store_count) &&
			                   ((uint16_t)(spin.hi - spin.lo) <= PERF_SPIN_SPAN);
			if (clean) {
				if (spin.iters >= 1)
					cur.idle += spin.pending;
				if (spin.iters < 255)
					spin.iters++;
			} else {
				spin.iters = 0;
			}
			spin.pending          = 0;
			spin.lo               = pc;
			spin.hi               = pc;
			spin.stores_at_anchor = store_count;
		} else {
			// A different backward jump: that target is the loop head now.
			spin_reset(pc, true);
		}
	} else {
		if (pc < spin.lo)
			spin.lo = pc;
		if (pc > spin.hi)
			spin.hi = pc;
	}

	spin.pending += clocks;
	spin.last_pc   = pc;
	spin.have_last = true;
}

void
perf_budget_step_(uint32_t clocks, uint16_t pc, uint8_t pbank, bool waiting, int irq_depth)
{
	cur.total += clocks;
	cur.instructions++;
	if (irq_depth > 0)
		cur.irq += clocks;

	// Attribution. An open marker region wins over an address zone: the guest
	// saying "this is the sprite update" is better evidence than a range
	// somebody drew around what they thought the sprite update was.
	int  zone   = -1;
	bool z_idle = false;
	if (marker_depth > 0) {
		zone = marker_stack[marker_depth - 1];
	} else if (zone_of) {
		const uint8_t t = zone_of[pc];
		if (t) {
			const perf_zone_t *z = &zones[t - 1];
			if (z->bank < 0 || z->bank == (int16_t)pbank)
				zone = t - 1;
		}
	}
	if (zone >= 0 && zone < PERF_MAX_ZONES) {
		cur_zone[zone] += clocks;
		z_idle = zones[zone].is_idle;
	}

	// Idle classification, highest precedence first.
	if (marker_mode) {
		// The guest is telling us. Do not also guess: an explicit statement
		// that beats the heuristic is the whole point of the marker layer.
		if (marker_idle || z_idle)
			cur.idle += clocks;
		spin_reset(pc, false);
	} else if (z_idle) {
		cur.idle += clocks;
		spin_reset(pc, false);
	} else if (waiting && cfg_idle_mode != PERF_IDLE_NONE) {
		cur.idle += clocks; // WAI: unambiguous, and free to detect
		spin_reset(pc, false);
	} else if (cfg_idle_mode == PERF_IDLE_AUTO) {
		spin_track(clocks, pc, irq_depth);
	} else {
		spin_reset(pc, false);
	}

	// A headless run has no video_step() to raise a frame, so nothing would
	// ever close the accumulator and the module would report one ever-growing
	// frame. Close it synthetically once it has run well past a frame's worth.
	// The threshold is deliberately generous: a machine that does produce
	// frames must never reach it, however badly it is overrunning its budget.
	if (cur.total >= cfg_cycles_per_frame * 4u)
		perf_budget_frame_end(0);
}

void
perf_budget_frame_end(uint32_t host_us)
{
	if (!perf_budget_enabled || !ring)
		return;

	// A region left open by the guest is closed here rather than carried, so a
	// missing leave costs one frame instead of every frame after it.
	marker_depth = 0;

	const uint32_t work = cur.total - cur.idle;

	period_hist[period_pos] = work;
	period_pos              = (period_pos + 1) % PERF_PERIOD_MAX;

	uint32_t period = 0;
	for (int i = 0; i < budget_frames; i++) {
		const int idx = (period_pos - 1 - i + 2 * PERF_PERIOD_MAX) % PERF_PERIOD_MAX;
		period += period_hist[idx];
	}

	cur.frame       = total_frames;
	cur.host_us     = host_us;
	cur.period_work = period;
	for (int i = 0; i < PERF_RING_ZONES; i++)
		cur.zone[i] = cur_zone[i];

	ring[head] = cur;
	head       = (head + 1) % capacity;
	if (count < capacity)
		count++;
	total_frames++;

	if (period > overrun_threshold()) {
		const uint32_t over = period - budget_cycles;
		pending_overruns.overruns++;
		if (over > pending_overruns.worst_overrun) {
			pending_overruns.worst_overrun = over;
			pending_overruns.worst_frame   = cur.frame;
			pending_overruns.worst_utilization =
			    budget_cycles ? 100.0f * (float)period / (float)budget_cycles : 0.0f;
		}
	}
	pending_overruns.frames++;

	clear_current();
}

// ─── Markers ────────────────────────────────────────────────────────────────

// Find (or claim) the zone slot standing for marker region `id`.
static int
marker_zone(uint8_t id)
{
	int free_slot = -1;
	for (int i = 0; i < PERF_MAX_ZONES; i++) {
		if (zones[i].used && zones[i].is_marker && zones[i].marker_id == id)
			return i;
		if (!zones[i].used && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return -1; // table full: the region still counts as work, just unnamed

	perf_zone_t *z = &zones[free_slot];
	memset(z, 0, sizeof *z);
	snprintf(z->name, sizeof z->name, "region %u", (unsigned)id);
	z->bank      = -1;
	z->is_marker = true;
	z->marker_id = id;
	z->used      = true;
	return free_slot;
}

void
perf_budget_marker_write(uint8_t value)
{
	marker_mode = true;

	if (value == 0xFE) {
		perf_budget_reset();
		marker_mode = true; // the reset cleared it; the guest is still driving
		marker_idle = false;
		return;
	}
	if (value == 0xF0) {
		marker_idle  = false;
		marker_depth = 0;
		return;
	}
	if (value == 0xF1) {
		marker_idle  = true;
		marker_depth = 0;
		return;
	}
	if (value == 0x00) {
		if (marker_depth > 0)
			marker_depth--;
		return;
	}
	if (value & 0x80) {
		const uint8_t id = value & 0x3F;
		// Unwind to just below the named region. Leaving a region that is not
		// open is ignored rather than treated as an error.
		for (int d = marker_depth - 1; d >= 0; d--) {
			const int zi = marker_stack[d];
			if (zi >= 0 && zones[zi].is_marker && zones[zi].marker_id == id) {
				marker_depth = d;
				return;
			}
		}
		return;
	}
	if (value <= 0x3F) {
		if (marker_depth < PERF_MARKER_STACK)
			marker_stack[marker_depth++] = (int8_t)marker_zone(value);
		return;
	}
}

uint8_t
perf_budget_marker_read(void)
{
	return PERF_MARKER_PROTOCOL;
}

// ─── Zones ──────────────────────────────────────────────────────────────────

static void
rebuild_zone_map(void)
{
	bool any = false;
	for (int i = 0; i < PERF_MAX_ZONES; i++) {
		if (zones[i].used && !zones[i].is_marker) {
			any = true;
			break;
		}
	}
	if (!any) {
		free(zone_of);
		zone_of = NULL;
		return;
	}
	if (!zone_of) {
		zone_of = (uint8_t *)calloc(65536, 1);
		if (!zone_of)
			return;
	}
	memset(zone_of, 0, 65536);

	// Later zones win where two overlap, which makes "draw a tighter zone
	// inside a looser one" work the way anybody would expect.
	for (int i = 0; i < PERF_MAX_ZONES; i++) {
		const perf_zone_t *z = &zones[i];
		if (!z->used || z->is_marker || z->end <= z->start)
			continue;
		for (uint32_t a = z->start; a < z->end; a++)
			zone_of[a] = (uint8_t)(i + 1);
	}
}

int
perf_budget_zone_add(const char *name, uint16_t start, uint16_t end, int16_t bank, bool is_idle)
{
	if (end <= start)
		return -1;
	for (int i = 0; i < PERF_MAX_ZONES; i++) {
		if (zones[i].used)
			continue;
		perf_zone_t *z = &zones[i];
		memset(z, 0, sizeof *z);
		snprintf(z->name, sizeof z->name, "%s", name ? name : "zone");
		z->start   = start;
		z->end     = end;
		z->bank    = bank;
		z->is_idle = is_idle;
		z->used    = true;
		rebuild_zone_map();
		return i;
	}
	return -1;
}

bool
perf_budget_zone_remove(int index)
{
	if (index < 0 || index >= PERF_MAX_ZONES || !zones[index].used)
		return false;
	memset(&zones[index], 0, sizeof zones[index]);
	// Anything still pointing at the slot would attribute cycles to a zone that
	// no longer exists.
	for (int d = 0; d < marker_depth; d++)
		if (marker_stack[d] == index)
			marker_stack[d] = -1;
	rebuild_zone_map();
	return true;
}

void
perf_budget_zone_clear(void)
{
	memset(zones, 0, sizeof zones);
	memset(cur_zone, 0, sizeof cur_zone);
	marker_depth = 0;
	rebuild_zone_map();
}

int
perf_budget_zone_count(void)
{
	int n = 0;
	for (int i = 0; i < PERF_MAX_ZONES; i++)
		if (zones[i].used)
			n = i + 1;
	return n;
}

bool
perf_budget_zone_get(int index, perf_zone_t *out)
{
	if (index < 0 || index >= PERF_MAX_ZONES || !zones[index].used)
		return false;
	if (out)
		*out = zones[index];
	return true;
}

// ─── Queries ────────────────────────────────────────────────────────────────

static int
cmp_u32(const void *a, const void *b)
{
	const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

// Nearest-rank percentile over an ascending array.
static uint32_t
percentile(const uint32_t *v, int n, int p)
{
	if (n <= 0)
		return 0;
	int idx = (int)(((int64_t)p * n + 99) / 100) - 1;
	if (idx < 0)
		idx = 0;
	if (idx >= n)
		idx = n - 1;
	return v[idx];
}

// Consumes `scratch[0..n-1]` (unsorted on entry, sorted on exit).
static void
stat_from_scratch(int n, perf_stat_t *out)
{
	memset(out, 0, sizeof *out);
	if (n <= 0)
		return;

	uint64_t sum = 0;
	for (int i = 0; i < n; i++)
		sum += scratch[i];

	qsort(scratch, (size_t)n, sizeof(uint32_t), cmp_u32);

	out->min  = scratch[0];
	out->max  = scratch[n - 1];
	out->mean = (uint32_t)(sum / (uint64_t)n);
	out->p50  = percentile(scratch, n, 50);
	out->p95  = percentile(scratch, n, 95);
	out->p99  = percentile(scratch, n, 99);
}

// How many of the newest samples fall in `seconds`.
static int
window_frames(float seconds)
{
	if (count <= 0)
		return 0;
	if (seconds <= 0.0f)
		return count;
	const float hz = perf_budget_vsync_hz();
	if (hz <= 0.0f)
		return count;
	int n = (int)(seconds * hz + 0.5f);
	if (n < 1)
		n = 1;
	if (n > count)
		n = count;
	return n;
}

// Index of the i'th newest sample (i == 0 is the newest).
static int
newest(int i)
{
	return (head - 1 - i + 2 * capacity) % capacity;
}

bool
perf_budget_last_frame(perf_frame_t *out)
{
	if (!ring || count <= 0)
		return false;
	if (out)
		*out = ring[newest(0)];
	return true;
}

bool
perf_budget_current_frame(perf_frame_t *out)
{
	if (!out)
		return false;
	*out = cur;
	out->frame = total_frames;
	for (int i = 0; i < PERF_RING_ZONES; i++)
		out->zone[i] = cur_zone[i];
	out->period_work = cur.total - cur.idle;
	return true;
}

int
perf_budget_frame_count(void)
{
	return count;
}

bool
perf_budget_frame_at(int newest_index, perf_frame_t *out)
{
	if (!ring || newest_index < 0 || newest_index >= count)
		return false;
	if (out)
		*out = ring[newest(newest_index)];
	return true;
}

uint32_t
perf_budget_total_frames(void)
{
	return total_frames;
}

bool
perf_budget_window(float seconds, perf_window_t *out)
{
	if (!out || !ring)
		return false;
	const int n = window_frames(seconds);
	if (n <= 0)
		return false;

	memset(out, 0, sizeof *out);
	out->seconds       = seconds;
	out->frames        = n;
	out->budget_cycles = budget_cycles;

	for (int i = 0; i < n; i++)
		scratch[i] = perf_frame_work(&ring[newest(i)]);
	stat_from_scratch(n, &out->work);

	for (int i = 0; i < n; i++)
		scratch[i] = ring[newest(i)].total;
	stat_from_scratch(n, &out->total);

	for (int i = 0; i < n; i++)
		scratch[i] = ring[newest(i)].irq;
	stat_from_scratch(n, &out->irq);

	for (int i = 0; i < n; i++)
		scratch[i] = ring[newest(i)].host_us;
	stat_from_scratch(n, &out->host_us);

	// Budget-period work carries the overrun answer, so the scan for the worst
	// frame rides along with the copy rather than making a second pass.
	int overruns = 0;
	const uint32_t threshold = overrun_threshold();
	for (int i = 0; i < n; i++) {
		const perf_frame_t *f = &ring[newest(i)];
		scratch[i]            = f->period_work;
		if (f->period_work > threshold) {
			overruns++;
			const uint32_t over = f->period_work - budget_cycles;
			if (over > out->worst_overrun) {
				out->worst_overrun = over;
				out->worst_frame   = f->frame;
			}
		}
	}
	stat_from_scratch(n, &out->budget_work);

	out->overruns    = overruns;
	out->overrun_pct = 100.0f * (float)overruns / (float)n;

	if (budget_cycles) {
		const float b        = (float)budget_cycles;
		out->mean_utilization = 100.0f * (float)out->budget_work.mean / b;
		out->p95_utilization  = 100.0f * (float)out->budget_work.p95 / b;
		out->max_utilization  = 100.0f * (float)out->budget_work.max / b;
	}
	return true;
}

bool
perf_budget_zone_window(int zone, float seconds, perf_stat_t *out)
{
	if (!out || zone < 0 || zone >= PERF_MAX_ZONES || !zones[zone].used)
		return false;

	memset(out, 0, sizeof *out);

	// Zones past the ring's per-frame slots have no history to take
	// percentiles over. Report the live frame flat rather than nothing, so the
	// caller still sees a number and can tell it apart by its equal fields.
	if (zone >= PERF_RING_ZONES) {
		const uint32_t v = cur_zone[zone];
		out->min = out->max = out->mean = out->p50 = out->p95 = out->p99 = v;
		return true;
	}
	if (!ring)
		return false;

	const int n = window_frames(seconds);
	if (n <= 0)
		return false;
	for (int i = 0; i < n; i++)
		scratch[i] = ring[newest(i)].zone[zone];
	stat_from_scratch(n, out);
	return true;
}

bool
perf_budget_drain_overruns(perf_overrun_report_t *out)
{
	if (pending_overruns.overruns == 0) {
		pending_overruns.frames = 0;
		return false;
	}
	if (out)
		*out = pending_overruns;
	memset(&pending_overruns, 0, sizeof pending_overruns);
	return true;
}
