// Commander X16 Emulator
// The input log. Holds no knowledge of the devices: events are applied through
// a hook, and the clock and the machine hash come from the host.

#include "input_log.h"

#include <stdlib.h>
#include <string.h>

#define INPUT_LOG_MAX_EVENTS 1000000u

uint8_t input_log_mode     = INPUT_LOG_IDLE;
bool    input_log_applying = false;

static const uint32_t   *clock_now;
static input_log_hooks_t host;
static uint8_t           cur_site = INPUT_SITE_TOP;

static input_event_t *events;
static uint32_t       capacity, count, cursor;
static uint64_t       acc;
static uint32_t       seen;

static bool                  has_recording;
static uint64_t              anchor_hash, end_hash;
static uint64_t              end_cycle;
static uint8_t               end_site;
static bool                  end_hash_valid;
static const char           *end_reason;
static input_replay_result_t result;
static uint64_t              diverged_event, diverged_at;

static uint64_t
now(void)
{
	const uint32_t t = *clock_now;
	acc += (uint32_t)(t - seen);
	seen = t;
	return acc;
}

void
input_log_init(const uint32_t *clock, const input_log_hooks_t *hooks)
{
	clock_now = clock;
	host      = *hooks;
}

void
input_log_set_site(input_site_t site)
{
	cur_site = (uint8_t)site;
}

void
input_log_tick(void)
{
	if (input_log_mode != INPUT_LOG_IDLE) {
		now();
	}
}

static void
finish_recording(const char *reason, bool verify)
{
	end_cycle      = now();
	end_site       = cur_site;
	end_hash_valid = verify && end_site == INPUT_SITE_TOP && host.hash && host.hash(&end_hash);
	end_reason     = reason;
	has_recording  = true;
	input_log_mode = INPUT_LOG_IDLE;
}

static void
finish_replay(input_replay_result_t r, const char *reason)
{
	result         = r;
	input_log_mode = INPUT_LOG_IDLE;
	if (reason) {
		end_reason = reason;
	}
	if (host.replay_end) {
		host.replay_end();
	}
}

void
input_log_note_at(input_site_t site, input_event_kind_t kind, uint8_t a, int32_t x, int32_t y)
{
	if (input_log_mode != INPUT_LOG_RECORDING) {
		return;
	}
	if (count == capacity) {
		const uint32_t want = capacity ? capacity * 2 : 1024;
		const uint32_t next = want > INPUT_LOG_MAX_EVENTS ? INPUT_LOG_MAX_EVENTS : want;
		input_event_t *grown =
		    next > capacity ? (input_event_t *)realloc(events, next * sizeof *events) : NULL;
		if (!grown) {
			finish_recording("the log is full", false);
			return;
		}
		events   = grown;
		capacity = next;
	}
	input_event_t *e = &events[count++];
	memset(e, 0, sizeof *e);
	e->cycle = now();
	e->x     = x;
	e->y     = y;
	e->kind  = (uint8_t)kind;
	e->site  = (uint8_t)site;
	e->a     = a;
}

void
input_log_note(input_event_kind_t kind, uint8_t a, int32_t x, int32_t y)
{
	input_log_note_at((input_site_t)cur_site, kind, a, x, y);
}

bool
input_log_start(void)
{
	if (input_log_mode != INPUT_LOG_IDLE || !clock_now || !host.hash || !host.hash(&anchor_hash)) {
		return false;
	}
	count          = 0;
	cursor         = 0;
	acc            = 0;
	seen           = *clock_now;
	has_recording  = false;
	end_hash_valid = false;
	end_reason     = NULL;
	result         = INPUT_REPLAY_NONE;
	input_log_mode = INPUT_LOG_RECORDING;
	return true;
}

void
input_log_end(const char *reason)
{
	if (input_log_mode == INPUT_LOG_RECORDING) {
		finish_recording(reason, true);
	} else if (input_log_mode == INPUT_LOG_REPLAYING) {
		finish_replay(INPUT_REPLAY_CANCELLED, reason);
	}
}

void
input_log_end_unverified(const char *reason)
{
	if (input_log_mode == INPUT_LOG_RECORDING) {
		finish_recording(reason, false);
	} else if (input_log_mode == INPUT_LOG_REPLAYING) {
		finish_replay(INPUT_REPLAY_CANCELLED, reason);
	}
}

bool
input_log_replay_start(void)
{
	uint64_t h;
	if (input_log_mode != INPUT_LOG_IDLE || !has_recording || !host.hash || !host.hash(&h) ||
	    h != anchor_hash) {
		return false;
	}
	cursor         = 0;
	acc            = 0;
	seen           = *clock_now;
	diverged_event = diverged_at = 0;
	result         = INPUT_REPLAY_RUNNING;
	input_log_mode = INPUT_LOG_REPLAYING;
	return true;
}

void
input_log_replay(input_site_t site)
{
	const uint64_t t = now();
	while (cursor < count) {
		const input_event_t *e = &events[cursor];
		if (e->cycle < t || (e->cycle == t && e->site < (uint8_t)site)) {
			diverged_event = e->cycle;
			diverged_at    = t;
			finish_replay(INPUT_REPLAY_DIVERGED, NULL);
			return;
		}
		if (e->cycle > t || e->site != (uint8_t)site) {
			return;
		}
		input_log_applying = true;
		if (host.apply) {
			host.apply(e);
		}
		input_log_applying = false;
		cursor++;
	}
	if (site != INPUT_SITE_TOP || t < end_cycle) {
		return;
	}
	if (!end_hash_valid || t != end_cycle) {
		finish_replay(INPUT_REPLAY_UNVERIFIED, NULL);
		return;
	}
	uint64_t h;
	const bool hashed = host.hash && host.hash(&h);
	finish_replay(hashed && h == end_hash ? INPUT_REPLAY_VERIFIED : INPUT_REPLAY_MISMATCH, NULL);
}

void
input_log_get_status(input_log_status_t *out)
{
	memset(out, 0, sizeof *out);
	out->mode                 = (input_log_mode_t)input_log_mode;
	out->has_recording        = has_recording;
	out->events               = count;
	out->applied              = cursor;
	out->cycle                = input_log_mode != INPUT_LOG_IDLE ? now() : 0;
	out->end_cycle            = end_cycle;
	out->end_hash_valid       = end_hash_valid;
	out->result               = result;
	out->diverged_event_cycle = diverged_event;
	out->diverged_at_cycle    = diverged_at;
	out->end_reason           = end_reason;
}
