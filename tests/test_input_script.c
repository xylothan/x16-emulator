// Checks for scripted input (src/input_script.c).
//
// Key names resolve through a fake lookup and events land in a log, so the
// parser and the frame scheduler are checked with no SDL and no machine.

#include "input_script.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>

// A handful of SDL-like names with made-up scancodes.
static int
fake_lookup(const char *name)
{
	static const struct { const char *n; int sc; } keys[] = {
		{"Return", 40}, {"Space", 44}, {"A", 4}, {"Left Shift", 225}, {"F1", 58},
	};
	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		const char *a = name, *b = keys[i].n;
		while (*a && *b && (*a | 32) == (*b | 32)) {
			a++;
			b++;
		}
		if (!*a && !*b)
			return keys[i].sc;
	}
	return -1;
}

static char log_buf[1024];

static void
logf_(const char *s)
{
	strncat(log_buf, s, sizeof log_buf - strlen(log_buf) - 1);
}

static void
h_joy_set(int slot, uint16_t mask)
{
	char b[32];
	snprintf(b, sizeof b, "J%d=%04X ", slot, mask);
	logf_(b);
}

static void
h_joy_off(int slot)
{
	char b[16];
	snprintf(b, sizeof b, "J%d=off ", slot);
	logf_(b);
}

static void
h_key(int sc, bool down)
{
	char b[16];
	snprintf(b, sizeof b, "K%d%c ", sc, down ? 'v' : '^');
	logf_(b);
}

static const is_hooks_t hooks = {h_joy_set, h_joy_off, h_key};

// Play frames 1..n and return what was delivered on frame `want` (or all
// frames when want is 0).
static const char *
play(uint32_t n, uint32_t want)
{
	static char out[1024];
	out[0] = 0;
	for (uint32_t f = 1; f <= n; f++) {
		log_buf[0] = 0;
		input_script_frame(f, &hooks);
		if (!want || f == want)
			strncat(out, log_buf, sizeof out - strlen(out) - 1);
	}
	return out;
}

static bool
parses(const char *text)
{
	char err[128];
	return input_script_parse(text, fake_lookup, err, sizeof err);
}

static bool
fails_with(const char *text, const char *needle)
{
	char err[128];
	if (input_script_parse(text, fake_lookup, err, sizeof err))
		return false;
	return strstr(err, needle) != NULL;
}

int
main(void)
{
	// ── Buttons map to the pad's active-low bits ────────────────────────────
	{
		check(parses("1 joy1 start"), "joy1 start");
		check_eq(input_script_count(), 1, "one event");
		check_eq(input_script_at(0)->mask, (uint16_t)~(1u << 3), "START is bit 3, pulled low");
		check(parses("1 joy2 a+b+up"), "combined buttons");
		check_eq(input_script_at(0)->slot, 1, "joy2 is slot 1");
		check_eq(input_script_at(0)->mask, (uint16_t)~((1u << 0) | (1u << 8) | (1u << 4)),
		         "A bit 0, B bit 8, UP bit 4");
		check(parses("1 JOY1 Start + Select"), "case and spaces around + are allowed");
		check_eq(input_script_at(0)->mask, (uint16_t)~((1u << 3) | (1u << 2)), "START+SELECT");
		check(parses("1 joy4 none"), "none");
		check_eq(input_script_at(0)->mask, 0xFFFF, "none holds nothing");
		check(parses("1 joy1 off"), "off");
		check_eq(input_script_at(0)->kind, IS_JOY_OFF, "off releases the pad");
	}

	// ── Keys: key is down then up on the same frame ─────────────────────────
	{
		check(parses("5 key Return\n6 keydown Left Shift\n7 keyup left shift"), "keys");
		check_eq(input_script_count(), 4, "key makes two events");
		check(strcmp(play(7, 5), "K40v K40^ ") == 0, "key = down, up on its frame");
		check(parses("5 key Return\n6 keydown Left Shift\n7 keyup left shift"), "keys again");
		check(strcmp(play(7, 6), "K225v ") == 0, "multi-word key name");
		check(parses("5 key Return\n6 keydown Left Shift\n7 keyup left shift"), "keys again");
		check(strcmp(play(7, 7), "K225^ ") == 0, "name lookup ignores case");
	}

	// ── Timing: absolute, relative, same-frame order, comments, ';' ─────────
	{
		check(parses("# boot\n"
		             "10 joy1 start   # tap\n"
		             "+6 joy1 none\n"
		             "\n"
		             "+30 key Space; +0 joy1 off\n"),
		      "a small script");
		check(strcmp(play(60, 0), "J0=FFF7 J0=FFFF K44v K44^ J0=off ") == 0, "all events, in order");
		check(parses("10 joy1 start\n+6 joy1 none\n+30 key Space; +0 joy1 off"), "again");
		check(strcmp(play(16, 16), "J0=FFFF ") == 0, "+6 after 10 is frame 16");
		check(parses("10 joy1 start\n+6 joy1 none\n+30 key Space; +0 joy1 off"), "again");
		check(strcmp(play(46, 46), "K44v K44^ J0=off ") == 0, "+0 shares the frame, after");
		check(strcmp(play(100, 0), "") == 0, "nothing replays once played");
	}

	// ── A late start skips past events rather than bunching them ────────────
	{
		check(parses("2 joy1 a\n3 joy1 b\n9 joy1 none"), "three events");
		log_buf[0] = 0;
		input_script_frame(5, &hooks);
		check(strcmp(log_buf, "") == 0, "frames 2 and 3 are not replayed on 5");
		input_script_frame(9, &hooks);
		check(strcmp(log_buf, "J0=FFFF ") == 0, "frame 9 still plays");
	}

	// ── Sources merge by frame, each on its own timeline ────────────────────
	{
		char err[128];
		input_script_clear();
		check(input_script_append("1 joy1 start\n+30 joy1 none", fake_lookup, err, sizeof err),
		      "first source");
		check(input_script_append("+10 key A\n20 key F1", fake_lookup, err, sizeof err),
		      "second source: +10 counts from 0, and 20 < 31 is fine");
		check(strcmp(play(40, 0), "J0=FFF7 K4v K4^ K58v K58^ J0=FFFF ") == 0,
		      "merged in frame order");
	}

	// ── Errors name the line ────────────────────────────────────────────────
	{
		check(fails_with("1 joy1 start\n2 joy1 jump", "line 2: unknown button 'jump'"), "bad button");
		check(fails_with("1 key Nope", "unknown key 'Nope'"), "bad key");
		check(fails_with("1 joy5 a", "unknown command 'joy5'"), "only four pads");
		check(fails_with("1 mouse 3", "unknown command"), "bad command");
		check(fails_with("joy1 a", "expected a frame"), "missing frame");
		check(fails_with("0 joy1 a", "frames start at 1"), "frame 0");
		check(fails_with("+0 joy1 a", "frames start at 1"), "+0 at the start is frame 0");
		check(fails_with("20 joy1 a\n10 joy1 b", "before the previous"), "going backwards");
		check(fails_with("1 joy1", "needs buttons"), "joy with nothing");
		check(fails_with("1 key", "needs a key name"), "key with nothing");
		check(fails_with("1 joy1 a+", "unknown button ''"), "trailing +");
	}

	input_script_clear();
	check_eq(input_script_count(), 0, "clear empties it");
	return x16_test_summary("input_script");
}
