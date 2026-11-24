// Checks for the input log (src/input_log.c), on a fake clock and machine hash.

#include "input_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

static uint32_t      fake_clock;
static uint64_t      fake_hash;
static bool          hash_works = true;
static input_event_t seen[16];
static int           seen_n, ends;

static void
fake_apply(const input_event_t *e)
{
	if (seen_n < 16) {
		seen[seen_n++] = *e;
	}
	check(input_log_applying && !input_log_host_blocked(), "apply runs unblocked");
}

static bool
fake_hash_fn(uint64_t *out)
{
	*out = fake_hash;
	return hash_works;
}

static void
fake_end(void)
{
	ends++;
}

static input_log_status_t
status(void)
{
	input_log_status_t s;
	input_log_get_status(&s);
	return s;
}

static void
at(uint32_t clock, input_site_t site)
{
	fake_clock = clock;
	input_log_replay(site);
}

static void
begin_replay(uint32_t clock)
{
	fake_clock = clock;
	fake_hash  = 1;
	seen_n     = 0;
	ends       = 0;
	check(input_log_replay_start(), "replay starts at the anchor");
}

static void
record_three(void)
{
	fake_clock = 100;
	fake_hash  = 1;
	check(input_log_start(), "recording starts");
	fake_clock = 110;
	input_log_set_site(INPUT_SITE_FRAME);
	input_log_note(INPUT_EV_KBD, 0x41, 0, 0);
	input_log_note_at(INPUT_SITE_POST, INPUT_EV_PASTE, 0x42, 0, 0);
	input_log_set_site(INPUT_SITE_TOP);
	fake_clock = 120;
	input_log_note(INPUT_EV_MOUSE_MOVE, 0, -3, 7);
	fake_clock = 130;
	fake_hash  = 2;
	input_log_end("stopped");
}

static void
test_order_and_verify(void)
{
	record_three();
	input_log_status_t s = status();
	check(s.mode == INPUT_LOG_IDLE && s.events == 3 && s.end_cycle == 30, "recording holds three events");

	begin_replay(5000);
	check(input_log_host_blocked(), "host input is blocked during replay");
	at(5009, INPUT_SITE_TOP);
	check(seen_n == 0, "nothing applies early");
	at(5010, INPUT_SITE_FRAME);
	check(seen_n == 1 && seen[0].a == 0x41, "frame event applies at its site");
	at(5010, INPUT_SITE_POST);
	check(seen_n == 2 && seen[1].a == 0x42, "post event applies at its site");
	at(5010, INPUT_SITE_TOP);
	at(5020, INPUT_SITE_TOP);
	check(seen_n == 3 && seen[2].x == -3 && seen[2].y == 7, "top event carries its values");
	check(status().mode == INPUT_LOG_REPLAYING, "replay runs on to the recorded end");
	fake_hash = 2;
	at(5030, INPUT_SITE_TOP);
	s = status();
	check(s.result == INPUT_REPLAY_VERIFIED && s.mode == INPUT_LOG_IDLE && ends == 1, "end hash verifies");
}

static void
test_mismatch(void)
{
	begin_replay(0);
	at(10, INPUT_SITE_FRAME);
	at(10, INPUT_SITE_POST);
	at(20, INPUT_SITE_TOP);
	fake_hash = 3;
	at(30, INPUT_SITE_TOP);
	check(status().result == INPUT_REPLAY_MISMATCH, "another end hash is a mismatch");
}

static void
test_diverged_late(void)
{
	begin_replay(0);
	at(11, INPUT_SITE_TOP);
	input_log_status_t s = status();
	check(s.result == INPUT_REPLAY_DIVERGED && s.diverged_event_cycle == 10 && s.diverged_at_cycle == 11,
	      "a passed event diverges");
	check(seen_n == 0, "a passed event is not applied late");
	check(!input_log_host_blocked() && ends == 1, "divergence releases the inputs");
}

static void
test_diverged_site(void)
{
	begin_replay(0);
	at(10, INPUT_SITE_POST);
	check(status().result == INPUT_REPLAY_DIVERGED, "a site passed at the same cycle diverges");
}

static void
test_wrong_anchor(void)
{
	fake_clock = 0;
	fake_hash  = 99;
	check(!input_log_replay_start(), "replay is refused away from the anchor");
	check(status().mode == INPUT_LOG_IDLE, "a refused replay stays idle");
}

static void
test_clock_wrap(void)
{
	fake_clock = 0xFFFFFFF0u;
	fake_hash  = 1;
	check(input_log_start(), "recording starts near the wrap");
	fake_clock = 0x10;
	input_log_note(INPUT_EV_KBD, 1, 0, 0);
	input_log_end("stopped");
	begin_replay(7);
	at(7 + 0x20, INPUT_SITE_TOP);
	check(seen_n == 1, "the stamp survives the 32-bit wrap");
}

static void
test_unverified(void)
{
	fake_clock = 0;
	fake_hash  = 1;
	check(input_log_start(), "recording starts");
	fake_clock = 10;
	input_log_end_unverified("hostfs");
	check(!status().end_hash_valid, "an unverified end holds no hash");
	begin_replay(0);
	at(10, INPUT_SITE_TOP);
	check(status().result == INPUT_REPLAY_UNVERIFIED, "replay reports the end unverified");
}

static void
test_cancel_and_idle_note(void)
{
	record_three();
	begin_replay(0);
	input_log_end("cancelled");
	input_log_status_t s = status();
	check(s.result == INPUT_REPLAY_CANCELLED && ends == 1, "ending a replay cancels it");
	input_log_note(INPUT_EV_KBD, 9, 0, 0);
	check(status().events == 3, "an idle log records nothing");
}

static void
test_refused_start_without_hash(void)
{
	hash_works = false;
	check(!input_log_start(), "recording needs a machine hash");
	hash_works = true;
}

static void
test_replay_from(void)
{
	fake_clock = 100;
	fake_hash  = 1;
	check(input_log_start(), "recording starts");
	input_log_note(INPUT_EV_JOY_PHYS, 0, 0xFFFE, 1);
	fake_clock = 110;
	input_log_note(INPUT_EV_KBD, 0x41, 0, 0);
	fake_clock = 120;
	uint64_t pos;
	uint32_t idx;
	input_log_position(&pos, &idx);
	check(pos == 20 && idx == 2, "position is cycle and events so far");
	input_log_note(INPUT_EV_KBD, 0x42, 0, 0);
	fake_clock = 130;
	fake_hash  = 2;
	input_log_end("stopped");

	fake_clock = 9000;
	fake_hash  = 7;
	check(!input_log_replay_from(idx, pos, 8), "replay from another state is refused");
	check(!input_log_replay_from(4, pos, 7), "an index past the log is refused");
	check(!input_log_replay_from(idx, 31, 7), "a cycle past the end is refused");
	check(status().mode == INPUT_LOG_IDLE, "a refused replay stays idle");

	seen_n = 0;
	ends   = 0;
	check(input_log_replay_from(idx, pos, 7), "replay starts partway");
	check(seen_n == 1 && seen[0].kind == INPUT_EV_JOY_PHYS, "earlier controller state applied, nothing else");
	at(9000, INPUT_SITE_TOP);
	check(seen_n == 2 && seen[1].a == 0x42, "the next event applies at its time");
	fake_hash = 2;
	at(9010, INPUT_SITE_TOP);
	check(status().result == INPUT_REPLAY_VERIFIED, "the end verifies");
}

static void
test_generation(void)
{
	const uint32_t g = input_log_generation();
	fake_hash        = 1;
	check(input_log_start(), "recording starts");
	input_log_end("stopped");
	check(input_log_generation() == g + 1, "each recording has its own generation");
}

int
main(void)
{
	static const input_log_hooks_t hooks = {fake_apply, fake_hash_fn, fake_end};
	input_log_init(&fake_clock, &hooks);
	test_order_and_verify();
	test_mismatch();
	test_diverged_late();
	test_diverged_site();
	test_wrong_anchor();
	test_clock_wrap();
	test_unverified();
	test_cancel_and_idle_note();
	test_refused_start_without_hash();
	test_replay_from();
	test_generation();
	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
