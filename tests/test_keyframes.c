// Checks for the keyframe ring (src/keyframes.c), on fake states.

#include "keyframes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void
check(bool cond, const char *what)
{
	if (!cond) {
		failures++;
		printf("FAIL: %s\n", what);
	} else {
		printf("ok  : %s\n", what);
	}
}

static uint8_t fake_state[256];
static size_t  fake_len = 100;
static uint8_t fake_byte;
static bool    refuse;

static bool
fake_capture(const void **data, size_t *len, uint64_t *hash, char *why, size_t why_len)
{
	if (refuse) {
		snprintf(why, why_len, "busy");
		return false;
	}
	memset(fake_state, fake_byte, sizeof fake_state);
	*data = fake_state;
	*len  = fake_len;
	*hash = fake_byte;
	return true;
}

static keyframes_status_t
status(void)
{
	keyframes_status_t s;
	keyframes_get_status(&s);
	return s;
}

static void
frames(int n, uint64_t *cycle)
{
	for (int i = 0; i < n; i++) {
		*cycle += 10;
		fake_byte = (uint8_t)*cycle;
		keyframes_frame(*cycle, (uint32_t)(*cycle / 10));
	}
}

static void
test_interval(void)
{
	uint64_t c = 0;
	check(keyframes_start(3, 1000), "starts");
	frames(2, &c);
	check(status().count == 0, "nothing before the interval");
	frames(1, &c);
	check(status().count == 1, "one at the interval");
	const keyframe_t *k = keyframes_get(0);
	check(k && k->cycle == 30 && k->event_index == 3, "stamped with position");
	check(k && k->hash == 30 && k->size == 100, "hash and size kept");
	check(k && ((const uint8_t *)k->data)[99] == 30, "holds a copy");
	memset(fake_state, 99, sizeof fake_state);
	check(k && ((const uint8_t *)k->data)[0] == 30, "copy outlives the capture buffer");
	frames(3, &c);
	check(status().count == 2 && keyframes_get(1)->cycle == 60, "next at the next interval");
}

static void
test_eviction(void)
{
	uint64_t c = 0;
	check(keyframes_start(1, 350), "starts");
	frames(5, &c);
	keyframes_status_t s = status();
	check(s.count == 3 && s.bytes == 300, "held to the budget");
	check(s.captured == 5 && s.evicted == 2, "oldest evicted");
	check(keyframes_get(0)->cycle == 30 && keyframes_get(2)->cycle == 50, "keeps the newest, in order");
	check(keyframes_get(3) == NULL, "nothing past the newest");
}

static void
test_gaps(void)
{
	uint64_t c = 0;
	check(keyframes_start(2, 1000), "starts");
	frames(2, &c);
	refuse = true;
	frames(5, &c);
	keyframes_status_t s = status();
	check(s.count == 1 && s.gaps == 1 && s.refused_frames == 4, "refused frames form one gap");
	const keyframe_gap_t *g = keyframes_gap(0);
	check(g && g->first_cycle == 40 && g->last_cycle == 70 && g->frames == 4, "gap span");
	check(g && !strcmp(g->reason, "busy"), "gap reason");
	refuse = false;
	frames(1, &c);
	s = status();
	check(s.count == 2 && keyframes_get(1)->cycle == 80, "captures on the first frame it can");
	refuse = true;
	frames(2, &c);
	refuse = false;
	frames(1, &c);
	check(status().gaps == 2, "a later refusal is a new gap");
}

static void
test_gap_limit(void)
{
	uint64_t c = 0;
	check(keyframes_start(1, 1000), "starts");
	for (uint32_t i = 0; i < KEYFRAMES_MAX_GAPS + 3; i++) {
		refuse = true;
		frames(1, &c);
		refuse = false;
		frames(1, &c);
	}
	keyframes_status_t s = status();
	check(s.gaps == KEYFRAMES_MAX_GAPS && s.gaps_dropped == 3, "gaps past the limit are counted");
}

static void
test_too_big(void)
{
	uint64_t c = 0;
	check(keyframes_start(1, 50), "starts");
	frames(1, &c);
	keyframes_status_t s = status();
	check(s.count == 0 && s.gaps == 1, "a state over the budget is a gap");
	check(!strcmp(keyframes_gap(0)->reason, "the state is larger than the budget"), "reason");
}

static void
test_stop_clear(void)
{
	uint64_t c = 0;
	check(keyframes_start(1, 1000), "starts");
	frames(2, &c);
	keyframes_stop();
	frames(2, &c);
	keyframes_status_t s = status();
	check(!s.armed && s.count == 2, "stop keeps states and captures no more");
	check(keyframes_start(1, 1000) && status().count == 0 && status().bytes == 0, "start empties");
	frames(1, &c);
	keyframes_clear();
	s = status();
	check(s.count == 0 && s.bytes == 0 && s.evicted == 0, "clear empties");
	check(!keyframes_start(0, 1000) && !keyframes_start(1, 0), "bad arguments refused");
	keyframes_stop();
}

static void
test_resume(void)
{
	uint64_t c = 100000;
	check(keyframes_start(1, 1000), "resume: starts");
	frames(2, &c);
	keyframes_stop();
	check(!status().armed, "resume: stopped");
	keyframes_resume();
	keyframes_status_t s = status();
	check(s.armed && s.count == 2, "resume: arms and keeps states");
	frames(1, &c);
	check(status().count == 3, "resume: captures again");
	keyframes_stop();
}

int
main(void)
{
	static const keyframes_hooks_t hooks = {fake_capture};
	keyframes_init(&hooks);
	test_interval();
	test_eviction();
	test_gaps();
	test_gap_limit();
	test_too_big();
	test_stop_clear();
	test_resume();
	keyframes_clear();
	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
