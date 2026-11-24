// Commander X16 Emulator
// The keyframe ring. See keyframes.h.

#include "keyframes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool keyframes_armed = false;

#define THUMB_BYTES ((size_t)KEYFRAMES_THUMB_W * KEYFRAMES_THUMB_H * sizeof(uint32_t))

static keyframes_hooks_t host;

static keyframe_t entries[KEYFRAMES_MAX];
static uint32_t   head, count;
static size_t     bytes, budget;
static uint32_t   interval, since;
static uint64_t   captured, evicted, refused;

static keyframe_gap_t gaps[KEYFRAMES_MAX_GAPS];
static uint32_t       n_gaps, gaps_dropped;
static bool           in_gap, gap_stored;

void
keyframes_init(const keyframes_hooks_t *hooks)
{
	host = *hooks;
}

static void
drop_oldest(void)
{
	free((void *)entries[head].data);
	bytes -= entries[head].size + (entries[head].thumb ? THUMB_BYTES : 0);
	memset(&entries[head], 0, sizeof entries[head]);
	head = (head + 1) % KEYFRAMES_MAX;
	count--;
	evicted++;
}

void
keyframes_clear(void)
{
	while (count) {
		drop_oldest();
	}
	head = 0;
	since = 0;
	captured = evicted = refused = 0;
	n_gaps = gaps_dropped = 0;
	in_gap = gap_stored = false;
}

bool
keyframes_start(uint32_t interval_frames, size_t budget_bytes)
{
	if (!host.capture || interval_frames == 0 || budget_bytes == 0) {
		return false;
	}
	keyframes_clear();
	interval        = interval_frames;
	budget          = budget_bytes;
	keyframes_armed = true;
	return true;
}

void
keyframes_stop(void)
{
	keyframes_armed = false;
}

void
keyframes_resume(void)
{
	keyframes_armed = interval != 0 && budget != 0;
}

static void
note_gap(uint64_t cycle, const char *why)
{
	refused++;
	if (in_gap) {
		if (gap_stored) {
			gaps[n_gaps - 1].last_cycle = cycle;
			gaps[n_gaps - 1].frames++;
		}
		return;
	}
	in_gap     = true;
	gap_stored = n_gaps < KEYFRAMES_MAX_GAPS;
	if (!gap_stored) {
		gaps_dropped++;
		return;
	}
	keyframe_gap_t *g = &gaps[n_gaps++];
	g->first_cycle    = cycle;
	g->last_cycle     = cycle;
	g->frames         = 1;
	snprintf(g->reason, sizeof g->reason, "%s", why);
}

bool
keyframes_capture(uint64_t cycle, uint32_t event_index)
{
	const void *data = NULL;
	size_t      len  = 0;
	uint64_t    hash = 0;
	char        why[KEYFRAMES_REASON_LEN] = "";

	if (!host.capture(&data, &len, &hash, why, sizeof why)) {
		note_gap(cycle, why[0] ? why : "the state could not be taken");
		return false;
	}
	const size_t thumb = host.thumbnail ? THUMB_BYTES : 0;
	if (len + thumb > budget) {
		note_gap(cycle, "the state is larger than the budget");
		return false;
	}
	// The thumbnail sits after the state, on a word boundary.
	const size_t at   = (len + 3) & ~(size_t)3;
	uint8_t     *copy = malloc(at + thumb);
	if (!copy) {
		note_gap(cycle, "out of memory");
		return false;
	}
	memcpy(copy, data, len);
	if (thumb) {
		host.thumbnail((uint32_t *)(copy + at));
	}

	while (count && (bytes + len + thumb > budget || count == KEYFRAMES_MAX)) {
		drop_oldest();
	}
	keyframe_t *k  = &entries[(head + count) % KEYFRAMES_MAX];
	k->cycle       = cycle;
	k->event_index = event_index;
	k->hash        = hash;
	k->size        = len;
	k->data        = copy;
	k->thumb       = thumb ? (const uint32_t *)(copy + at) : NULL;
	count++;
	bytes += len + thumb;
	captured++;
	since      = 0;
	in_gap     = false;
	gap_stored = false;
	return true;
}

void
keyframes_frame(uint64_t cycle, uint32_t event_index)
{
	if (!keyframes_armed) {
		return;
	}
	if (since < interval) {
		since++;
	}
	if (since >= interval) {
		keyframes_capture(cycle, event_index);
	}
}

const keyframe_t *
keyframes_get(uint32_t index)
{
	return index < count ? &entries[(head + index) % KEYFRAMES_MAX] : NULL;
}

const keyframe_gap_t *
keyframes_gap(uint32_t index)
{
	return index < n_gaps ? &gaps[index] : NULL;
}

void
keyframes_get_status(keyframes_status_t *out)
{
	memset(out, 0, sizeof *out);
	out->armed          = keyframes_armed;
	out->count          = count;
	out->bytes          = bytes;
	out->budget         = budget;
	out->interval       = interval;
	out->captured       = captured;
	out->evicted        = evicted;
	out->refused_frames = refused;
	out->gaps           = n_gaps;
	out->gaps_dropped   = gaps_dropped;
}
