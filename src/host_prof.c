// Commander X16 Emulator — host-side time accounting. See host_prof.h.

#include "host_prof.h"

#include <stdlib.h>
#include <string.h>

static host_prof_clock_fn clock_fn;
static uint64_t           clock_freq = 1;

// The open sections, innermost last. Time between two marks is charged to
// whatever is on top, or to HP_EMU when nothing is.
static hp_section_t stack[HP_STACK_DEPTH];
static int          depth;
static uint64_t     mark;

static uint64_t frame_ticks[HP_SECTIONS]; // the frame in progress
static uint64_t total_ticks[HP_SECTIONS]; // since reset
static uint64_t total_cycles;
static uint32_t frames, ui_frames;

static uint32_t last_us[HP_SECTIONS];
static uint32_t last_wall_us;

static host_prof_frame_t ring[HP_RING_FRAMES]; // recent frames, oldest overwritten
static int               ring_count, ring_head;

// The worst frames since reset, by busy time, worst first.
static host_prof_frame_t worst[HP_WORST_FRAMES];
static int               nworst;

static uint32_t spike_us;                     // 0: not counting
static uint32_t spikes;
static uint32_t spikes_by[HP_SECTIONS];

static uint32_t ui_frames_at_frame_start;

typedef struct {
	const char *name;
	uint32_t    calls;
	uint64_t    ticks;
	uint64_t    max_ticks;
	double      recent_us;
} panel_t;

static panel_t panels[HP_MAX_PANELS];
static int     npanels;

static const char *const names[HP_SECTIONS] = {
	"emu", "events", "present", "ui_build", "ui_draw", "sleep", "dap",
};

static uint64_t
now(void)
{
	return clock_fn ? clock_fn() : 0;
}

static uint32_t
to_us(uint64_t ticks)
{
	const uint64_t us = ticks * 1000000u / clock_freq;
	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

// Charge everything since the last mark to the section on top of the stack.
static void
charge(uint64_t t)
{
	const hp_section_t top = depth > 0 ? stack[depth - 1] : HP_EMU;
	frame_ticks[top] += t - mark;
	mark = t;
}

void
host_prof_init(host_prof_clock_fn fn, uint64_t freq)
{
	clock_fn   = fn;
	clock_freq = freq ? freq : 1;
	depth      = 0;
	npanels    = 0;
	host_prof_reset();
}

uint64_t
host_prof_now(void)
{
	return now();
}

void
host_prof_reset(void)
{
	memset(frame_ticks, 0, sizeof frame_ticks);
	memset(total_ticks, 0, sizeof total_ticks);
	memset(last_us, 0, sizeof last_us);
	last_wall_us = 0;
	total_cycles = 0;
	frames = ui_frames = 0;
	ring_count = ring_head = 0;
	nworst = 0;
	spikes = 0;
	memset(spikes_by, 0, sizeof spikes_by);
	ui_frames_at_frame_start = 0;
	for (int i = 0; i < npanels; i++) {
		panels[i].calls     = 0;
		panels[i].ticks     = 0;
		panels[i].max_ticks = 0;
	}
	mark = now();
}

void
host_prof_begin(hp_section_t s)
{
	if (s <= HP_EMU || s >= HP_SECTIONS)
		return;
	charge(now());
	if (depth < HP_STACK_DEPTH)
		stack[depth] = s;
	// Counted even past the limit, so the matching end still balances.
	depth++;
}

void
host_prof_end(hp_section_t s)
{
	(void)s;
	if (depth <= 0)
		return;
	if (depth <= HP_STACK_DEPTH)
		charge(now());
	depth--;
}

void
host_prof_panel(const char *name, uint64_t ticks)
{
	panel_t *p = NULL;
	// Panel names are string literals from the registry, so the pointer is a
	// stable identity; strcmp covers a caller that builds its own.
	for (int i = 0; i < npanels; i++) {
		if (panels[i].name == name || strcmp(panels[i].name, name) == 0) {
			p = &panels[i];
			break;
		}
	}
	if (!p) {
		if (npanels >= HP_MAX_PANELS)
			return;
		p = &panels[npanels++];
		memset(p, 0, sizeof *p);
		p->name = name;
	}
	p->calls++;
	p->ticks += ticks;
	if (ticks > p->max_ticks)
		p->max_ticks = ticks;
	const double us = (double)ticks * 1e6 / (double)clock_freq;
	p->recent_us    = p->recent_us == 0.0 ? us : p->recent_us * 0.9 + us * 0.1;
}

void
host_prof_ui_frame(void)
{
	ui_frames++;
}

void
host_prof_frame_end(uint32_t cycles)
{
	charge(now());

	uint64_t wall = 0;
	for (int i = 0; i < HP_SECTIONS; i++) {
		wall += frame_ticks[i];
		total_ticks[i] += frame_ticks[i];
		last_us[i] = to_us(frame_ticks[i]);
	}
	last_wall_us = to_us(wall);

	host_prof_frame_t f;
	f.frame     = frames;
	f.wall_us   = last_wall_us;
	f.busy_us   = to_us(wall - frame_ticks[HP_SLEEP]);
	f.cycles    = cycles;
	f.ui_frames = ui_frames - ui_frames_at_frame_start;
	memcpy(f.us, last_us, sizeof f.us);

	ring[ring_head] = f;
	ring_head       = (ring_head + 1) % HP_RING_FRAMES;
	if (ring_count < HP_RING_FRAMES)
		ring_count++;

	// Insert into the worst-frames list, kept sorted worst first. Ties keep
	// the earlier frame, so a run of equal frames reports where it began.
	if (nworst < HP_WORST_FRAMES || f.busy_us > worst[nworst - 1].busy_us) {
		int at = nworst < HP_WORST_FRAMES ? nworst++ : HP_WORST_FRAMES - 1;
		while (at > 0 && worst[at - 1].busy_us < f.busy_us) {
			worst[at] = worst[at - 1];
			at--;
		}
		worst[at] = f;
	}

	if (spike_us && f.busy_us > spike_us) {
		spikes++;
		spikes_by[host_prof_dominant(&f)]++;
	}

	memset(frame_ticks, 0, sizeof frame_ticks);
	total_cycles += cycles;
	frames++;
	ui_frames_at_frame_start = ui_frames;
}

bool
host_prof_recent(int newest_index, host_prof_frame_t *out)
{
	if (newest_index < 0 || newest_index >= ring_count || !out)
		return false;
	const int i = (ring_head - 1 - newest_index + HP_RING_FRAMES * 2) % HP_RING_FRAMES;
	*out = ring[i];
	return true;
}

int
host_prof_worst(host_prof_frame_t *out, int max)
{
	const int n = max < nworst ? max : nworst;
	for (int i = 0; out && i < n; i++)
		out[i] = worst[i];
	return n;
}

hp_section_t
host_prof_dominant(const host_prof_frame_t *f)
{
	hp_section_t best = HP_EMU;
	for (int i = 0; i < HP_SECTIONS; i++) {
		if (i == HP_SLEEP)
			continue;
		if (f->us[i] > f->us[best])
			best = (hp_section_t)i;
	}
	return best;
}

void
host_prof_set_spike_us(uint32_t us)
{
	spike_us = us;
}

uint32_t
host_prof_spike_us(void)
{
	return spike_us;
}

uint32_t
host_prof_spikes(void)
{
	return spikes;
}

uint32_t
host_prof_spikes_by(hp_section_t s)
{
	return ((int)s >= 0 && (int)s < HP_SECTIONS) ? spikes_by[s] : 0;
}

const char *
host_prof_section_name(hp_section_t s)
{
	return ((int)s >= 0 && (int)s < HP_SECTIONS) ? names[s] : "?";
}

static int
cmp_u32(const void *a, const void *b)
{
	const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

void
host_prof_summary(host_prof_summary_t *out)
{
	memset(out, 0, sizeof *out);
	uint64_t wall = 0;
	for (int i = 0; i < HP_SECTIONS; i++)
		wall += total_ticks[i];

	out->frames    = frames;
	out->ui_frames = ui_frames;
	out->wall_s    = (double)wall / (double)clock_freq;
	if (out->wall_s > 0) {
		out->emu_mhz = (double)total_cycles / out->wall_s / 1e6;
		out->fps     = frames / out->wall_s;
		out->ui_hz   = ui_frames / out->wall_s;
	}
	for (int i = 0; i < HP_SECTIONS; i++) {
		const double s = (double)total_ticks[i] / (double)clock_freq;
		if (out->wall_s > 0)
			out->ms_per_s[i] = s * 1000.0 / out->wall_s;
		if (frames)
			out->us_per_frame[i] = s * 1e6 / frames;
	}

	// Called rarely (a readout tooltip, DAP, the end of a benchmark), so a
	// sort of at most HP_RING_FRAMES values is fine here.
	static uint32_t scratch[HP_RING_FRAMES];
	const int       n = ring_count;
	if (n > 0) {
		for (int i = 0; i < n; i++)
			scratch[i] = ring[i].busy_us;
		qsort(scratch, (size_t)n, sizeof scratch[0], cmp_u32);
		out->busy_p50_us  = scratch[(n - 1) * 50 / 100];
		out->busy_p95_us  = scratch[(n - 1) * 95 / 100];
		out->busy_max_us  = scratch[n - 1];
		out->busy_samples = n;
	}
}

void
host_prof_last_frame(uint32_t out_us[HP_SECTIONS], uint32_t *wall_us)
{
	if (out_us)
		memcpy(out_us, last_us, sizeof last_us);
	if (wall_us)
		*wall_us = last_wall_us;
}

int
host_prof_panel_count(void)
{
	return npanels;
}

bool
host_prof_panel_get(int i, host_prof_panel_t *out)
{
	if (i < 0 || i >= npanels || !out)
		return false;
	const panel_t *p = &panels[i];
	uint64_t       wall = 0;
	for (int s = 0; s < HP_SECTIONS; s++)
		wall += total_ticks[s];

	out->name      = p->name;
	out->calls     = p->calls;
	out->avg_us    = p->calls ? (double)p->ticks * 1e6 / (double)clock_freq / p->calls : 0.0;
	out->recent_us = p->recent_us;
	out->max_us    = to_us(p->max_ticks);
	out->ms_per_s  = wall ? (double)p->ticks * 1000.0 / (double)wall : 0.0;
	return true;
}
