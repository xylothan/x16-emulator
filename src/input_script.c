// Commander X16 Emulator — scripted input. See input_script.h.

#include "input_script.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static is_event_t *events;
static int         count, cap;
static int         next_event;     // playback cursor
static uint32_t    last_frame;     // previous command's frame, this text only

// Pad button names and bits, matching JOY_BIT_* in joystick.h (kept local so
// this file needs no SDL): A X SELECT START UP DOWN LEFT RIGHT B Y L R.
static const struct {
	const char *name;
	uint16_t    bit;
} buttons[] = {
	{"a", 1 << 0},     {"x", 1 << 1},     {"select", 1 << 2}, {"start", 1 << 3},
	{"up", 1 << 4},    {"down", 1 << 5},  {"left", 1 << 6},   {"right", 1 << 7},
	{"b", 1 << 8},     {"y", 1 << 9},     {"l", 1 << 10},     {"r", 1 << 11},
};

static bool
push(const is_event_t *e)
{
	if (count == cap) {
		const int   ncap = cap ? cap * 2 : 64;
		is_event_t *grown = realloc(events, sizeof *events * (size_t)ncap);
		if (!grown)
			return false;
		events = grown;
		cap    = ncap;
	}
	// Insert keeping frame order, after any event already on the same frame.
	int at = count;
	while (at > 0 && events[at - 1].frame > e->frame)
		at--;
	memmove(&events[at + 1], &events[at], sizeof *events * (size_t)(count - at));
	events[at] = *e;
	count++;
	return true;
}

static bool
ieq(const char *a, const char *b)
{
	while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
		a++;
		b++;
	}
	return !*a && !*b;
}

static char *
trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

// Parse one command line (comment already stripped, not empty).
static bool
parse_line(char *s, int lineno, is_key_lookup_fn lookup, char *err, int errsz)
{
	// <frame>
	char *p = s;
	bool  rel = false;
	if (*p == '+') {
		rel = true;
		p++;
	}
	if (!isdigit((unsigned char)*p)) {
		snprintf(err, (size_t)errsz, "line %d: expected a frame number or +N", lineno);
		return false;
	}
	char         *end;
	unsigned long n = strtoul(p, &end, 10);
	const uint32_t frame = rel ? last_frame + (uint32_t)n : (uint32_t)n;
	if (frame == 0) {
		snprintf(err, (size_t)errsz, "line %d: frames start at 1", lineno);
		return false;
	}
	if (!rel && frame < last_frame) {
		snprintf(err, (size_t)errsz, "line %d: frame %u is before the previous command's %u",
		         lineno, (unsigned)frame, (unsigned)last_frame);
		return false;
	}

	// <command>
	p = trim(end);
	char *cmd = p;
	while (*p && !isspace((unsigned char)*p))
		p++;
	if (*p)
		*p++ = 0;
	char *arg = trim(p);

	is_event_t e;
	memset(&e, 0, sizeof e);
	e.frame = frame;
	e.line  = lineno;

	if (strlen(cmd) == 4 && tolower((unsigned char)cmd[0]) == 'j' &&
	    tolower((unsigned char)cmd[1]) == 'o' && tolower((unsigned char)cmd[2]) == 'y' &&
	    cmd[3] >= '1' && cmd[3] <= '4') {
		e.slot = (uint8_t)(cmd[3] - '1');
		if (!*arg) {
			snprintf(err, (size_t)errsz, "line %d: %s needs buttons, none or off", lineno, cmd);
			return false;
		}
		if (ieq(arg, "off")) {
			e.kind = IS_JOY_OFF;
		} else {
			e.kind = IS_JOY_SET;
			e.mask = 0xFFFF; // nothing held
			if (!ieq(arg, "none")) {
				char *tok = arg;
				for (;;) {
					char *plus = strchr(tok, '+');
					if (plus)
						*plus = 0;
					char    *name = trim(tok);
					uint16_t bit  = 0;
					for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
						if (ieq(name, buttons[i].name))
							bit = buttons[i].bit;
					if (!bit) {
						snprintf(err, (size_t)errsz, "line %d: unknown button '%s'", lineno, name);
						return false;
					}
					e.mask &= (uint16_t)~bit;
					if (!plus)
						break;
					tok = plus + 1;
				}
			}
		}
		if (!push(&e))
			goto oom;
	} else if (ieq(cmd, "key") || ieq(cmd, "keydown") || ieq(cmd, "keyup")) {
		if (!*arg) {
			snprintf(err, (size_t)errsz, "line %d: %s needs a key name", lineno, cmd);
			return false;
		}
		const int sc = lookup ? lookup(arg) : -1;
		if (sc < 0) {
			snprintf(err, (size_t)errsz, "line %d: unknown key '%s'", lineno, arg);
			return false;
		}
		e.scancode = sc;
		if (!ieq(cmd, "keyup")) {
			e.kind = IS_KEY_DOWN;
			if (!push(&e))
				goto oom;
		}
		if (!ieq(cmd, "keydown")) {
			e.kind = IS_KEY_UP;
			if (!push(&e))
				goto oom;
		}
	} else {
		snprintf(err, (size_t)errsz, "line %d: unknown command '%s' (joy1-joy4, key, keydown, keyup)",
		         lineno, cmd);
		return false;
	}
	last_frame = frame;
	return true;
oom:
	snprintf(err, (size_t)errsz, "line %d: out of memory", lineno);
	return false;
}

bool
input_script_append(const char *text, is_key_lookup_fn lookup, char *err, int errsz)
{
	if (err && errsz > 0)
		err[0] = 0;
	last_frame = 0;
	char  buf[512];
	int   lineno = 0;
	const char *p = text ? text : "";
	while (*p) {
		// One line, or one ';'-separated command.
		size_t len = strcspn(p, "\n;");
		lineno++;
		if (len >= sizeof buf) {
			snprintf(err, (size_t)errsz, "line %d: too long", lineno);
			return false;
		}
		memcpy(buf, p, len);
		buf[len] = 0;
		p += len;
		if (*p)
			p++;
		char *hash = strchr(buf, '#');
		if (hash)
			*hash = 0;
		char *s = trim(buf);
		if (!*s)
			continue;
		if (!parse_line(s, lineno, lookup, err, errsz))
			return false;
	}
	return true;
}

bool
input_script_parse(const char *text, is_key_lookup_fn lookup, char *err, int errsz)
{
	input_script_clear();
	return input_script_append(text, lookup, err, errsz);
}

void
input_script_clear(void)
{
	free(events);
	events     = NULL;
	count      = 0;
	cap        = 0;
	next_event = 0;
	last_frame = 0;
}

int
input_script_count(void)
{
	return count;
}

const is_event_t *
input_script_at(int i)
{
	return (i >= 0 && i < count) ? &events[i] : NULL;
}

void
input_script_frame(uint32_t frame, const is_hooks_t *hooks)
{
	// Skip anything for frames already gone (a script started late).
	while (next_event < count && events[next_event].frame < frame)
		next_event++;
	while (next_event < count && events[next_event].frame == frame) {
		const is_event_t *e = &events[next_event++];
		if (!hooks)
			continue;
		switch (e->kind) {
			case IS_JOY_SET:  if (hooks->joy_set) hooks->joy_set(e->slot, e->mask); break;
			case IS_JOY_OFF:  if (hooks->joy_off) hooks->joy_off(e->slot); break;
			case IS_KEY_DOWN: if (hooks->key) hooks->key(e->scancode, true); break;
			case IS_KEY_UP:   if (hooks->key) hooks->key(e->scancode, false); break;
		}
	}
}
