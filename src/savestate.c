// Commander X16 Emulator
// Save states: the header, the order modules are saved in, the check a state
// must pass before it is loaded, and the named in-memory slots.

#include "savestate.h"

#include <stdio.h>
#include <string.h>

#include "glue.h"
#include "cartridge.h"
#include "code_map.h"
#include "debug_server.h"
#include "host_prof.h"
#include "ieee.h"
#include "perf_budget.h"
#include "perf_hist.h"
#include "sprite_trace.h"
#include "timing.h"
#include "vera_bandwidth.h"
#include "version.h"
#include "waterfall.h"
#include "git_rev.h"
#include "cpu/fake6502.h"
#include "cpu/registers.h"

extern struct regs regs;
extern uint8_t *CART;
extern bool ym2151_irq_support;
extern bool has_serial;
extern bool headless;

#define SAVESTATE_FORMAT 1

// Which build wrote a state. Values are stored in the writer's own layout, so a
// state only means anything to the build that wrote it.
#define SAVESTATE_BUILD_LEN 64
static const char build_id[] = "x16emu " VER " " GIT_REV " " __DATE__ " " __TIME__;

static bool loaded_any = false;

bool
savestate_any_loaded(void)
{
	return loaded_any;
}

// ─── Header ──────────────────────────────────────────────────────────────────
//
// The machine a state was taken from. Loading into a machine configured
// differently would put, say, 2 MB of banked RAM into a machine with 512 KB,
// or VIA2 state into a machine without one, so each field must match.

typedef struct {
	const char *name;
	uint32_t    value;
} shape_field;

static int
current_shape(shape_field *out)
{
	int n = 0;
	out[n++] = (shape_field){ "CPU (65C02 or 65C816)", regs.is65c816 ? 816 : 6502 };
	out[n++] = (shape_field){ "CPU clock (-mhz)", MHZ };
	out[n++] = (shape_field){ "low RAM banks", num_banks };
	out[n++] = (shape_field){ "banked RAM (-ram)", num_ram_banks };
	out[n++] = (shape_field){ "second VIA (-via2)", has_via2 };
	out[n++] = (shape_field){ "YM2151 IRQ (-ymirq)", ym2151_irq_support };
	out[n++] = (shape_field){ "serial bus (-serial)", has_serial };
	out[n++] = (shape_field){ "MIDI card (-midicard)", has_midi_card };
	out[n++] = (shape_field){ "cartridge (-cart)", CART != NULL };
	out[n++] = (shape_field){ "Gen 2 machine", is_gen2 };
	out[n++] = (shape_field){ "-testbench", headless };
	return n;
}

#define SHAPE_MAX 16

static void
header_save(ss_writer *w)
{
	ss_begin(w, "X16S", SAVESTATE_FORMAT);
	char build[SAVESTATE_BUILD_LEN];
	memset(build, 0, sizeof build);
	strncpy(build, build_id, sizeof build - 1);
	ss_put(w, build, sizeof build);
	shape_field shape[SHAPE_MAX];
	const int n = current_shape(shape);
	ss_put_u32(w, (uint32_t)n);
	for (int i = 0; i < n; i++) {
		ss_put_u32(w, shape[i].value);
	}
	ss_end(w);
}

// The header was already checked to be this machine's, so loading it only
// steps over it.
static void
header_skip(ss_reader *r)
{
	if (ss_open(r, "X16S") < 0) {
		return;
	}
	r->pos = r->chunk_end;
	ss_close(r);
}

static bool
header_check(const void *data, size_t len, char *err, size_t err_len)
{
	ss_reader r;
	ss_reader_init(&r, data, len);
	if (ss_open(&r, "X16S") != SAVESTATE_FORMAT) {
		snprintf(err, err_len, "not a save state from this emulator");
		return false;
	}
	char build[SAVESTATE_BUILD_LEN];
	ss_get(&r, build, sizeof build);
	build[sizeof build - 1] = '\0';
	if (r.failed || strcmp(build, build_id) != 0) {
		snprintf(err, err_len, "the state was saved by a different build (%s)", r.failed ? "?" : build);
		return false;
	}
	shape_field shape[SHAPE_MAX];
	const int n = current_shape(shape);
	if (ss_get_u32(&r) != (uint32_t)n) {
		snprintf(err, err_len, "the state's machine description does not match this build");
		return false;
	}
	for (int i = 0; i < n; i++) {
		const uint32_t v = ss_get_u32(&r);
		if (!r.failed && v != shape[i].value) {
			snprintf(err, err_len, "the state is from a machine with a different %s (%u, this one has %u)",
			         shape[i].name, (unsigned)v, (unsigned)shape[i].value);
			return false;
		}
	}
	ss_close(&r);
	if (r.failed) {
		snprintf(err, err_len, "the state's header is damaged");
		return false;
	}
	return true;
}

// ─── The whole machine ───────────────────────────────────────────────────────

void
savestate_write(ss_writer *w)
{
	header_save(w);
	cpu_state_save(w);
	irq_ctx_state_save(w);
	main_state_save(w);
	memory_state_save(w);
	cartridge_state_save(w);
	video_state_save(w);
	psg_state_save(w);
	pcm_state_save(w);
	audio_state_save(w);
	YM_state_save(w);
	via_state_save(w);
	joystick_state_save(w);
	i2c_state_save(w);
	smc_state_save(w);
	rtc_state_save(w);
	vera_spi_state_save(w);
	sdcard_state_save(w);
	ieee_state_save(w);
#ifdef HAS_DAP
	debug_server_state_save(w);
#endif
}

static void
savestate_read(ss_reader *r)
{
	header_skip(r);
	cpu_state_load(r);
	irq_ctx_state_load(r);
	main_state_load(r);
	memory_state_load(r);
	cartridge_state_load(r);
	video_state_load(r);
	psg_state_load(r);
	pcm_state_load(r);
	audio_state_load(r);
	YM_state_load(r);
	via_state_load(r);
	joystick_state_load(r);
	i2c_state_load(r);
	smc_state_load(r);
	rtc_state_load(r);
	vera_spi_state_load(r);
	sdcard_state_load(r);
	ieee_state_load(r);
#ifdef HAS_DAP
	debug_server_state_load(r);
#endif
}

bool
savestate_check(const void *data, size_t len, char *err, size_t err_len)
{
	if (!header_check(data, len, err, err_len)) {
		return false;
	}

	// What a state of this machine looks like, chunk by chunk. Counting writes
	// nothing, and saving only reads, so this leaves the machine as it was.
	ss_layout expect, got;
	ss_writer counter;
	ss_writer_init(&counter, true, &expect);
	savestate_write(&counter);
	if (counter.failed) {
		snprintf(err, err_len, "this machine's state could not be described");
		return false;
	}
	if (!ss_read_layout(data, len, &got)) {
		snprintf(err, err_len, "the state is damaged: its sections do not add up");
		return false;
	}
	const int i = ss_layout_diff(&expect, &got);
	if (i >= 0) {
		if (i < got.count && i < expect.count) {
			snprintf(err, err_len, "the state's '%.4s' section does not match this machine's", expect.chunks[i].tag);
		} else {
			snprintf(err, err_len, "the state has %d sections, this machine has %d", got.count, expect.count);
		}
		return false;
	}
	return true;
}

bool
savestate_apply(const void *data, size_t len, char *err, size_t err_len)
{
	if (!savestate_check(data, len, err, err_len)) {
		return false;
	}

	ss_reader r;
	ss_reader_init(&r, data, len);
	savestate_read(&r);
	loaded_any = true;
	if (r.failed || r.pos != len) {
		// Unreachable after a passing check, short of a loader that reads
		// differently from its saver. The machine is part-overwritten now.
		snprintf(err, err_len, "the state did not load cleanly; reset the machine");
		return false;
	}

	// Host-side things that followed the replaced machine start afresh. Pacing
	// re-bases on the restored clock, so the machine neither sprints nor waits.
	main_state_loaded();
	timing_init();
	code_map_reset();
	perf_budget_reset();
	perf_hist_reset();
	vera_bandwidth_reset();
	host_prof_reset();
	sprite_trace_reset();
	wf_reset();
	return true;
}

// ─── Slots ───────────────────────────────────────────────────────────────────

typedef struct {
	char       name[SAVESTATE_MAX_SLOT_NAME + 1];
	ss_writer  w;
	bool       used;
} slot_t;

static slot_t slots[SAVESTATE_MAX_SLOTS];

static slot_t *
slot_find(const char *name)
{
	for (int i = 0; i < SAVESTATE_MAX_SLOTS; i++) {
		if (slots[i].used && strcmp(slots[i].name, name) == 0) {
			return &slots[i];
		}
	}
	return NULL;
}

size_t
savestate_slot_save(const char *name, char *err, size_t err_len)
{
	if (name == NULL || name[0] == '\0' || strlen(name) > SAVESTATE_MAX_SLOT_NAME) {
		snprintf(err, err_len, "a slot name is 1 to %d characters", SAVESTATE_MAX_SLOT_NAME);
		return 0;
	}
	// Not covered by states yet: refusing is better than a state that would
	// come back missing a device.
	if (has_serial || has_midi_card) {
		snprintf(err, err_len, "a machine with %s cannot be saved", has_serial ? "-serial" : "-midicard");
		return 0;
	}
	char why[128];
	if (ieee_state_busy(why, sizeof why)) {
		snprintf(err, err_len, "cannot save while %s", why);
		return 0;
	}
	slot_t *s = slot_find(name);
	if (s == NULL) {
		for (int i = 0; i < SAVESTATE_MAX_SLOTS && s == NULL; i++) {
			if (!slots[i].used) {
				s = &slots[i];
			}
		}
		if (s == NULL) {
			snprintf(err, err_len, "all %d slots are in use", SAVESTATE_MAX_SLOTS);
			return 0;
		}
		ss_writer_init(&s->w, false, NULL);
		strcpy(s->name, name);
	}
	// A slot keeps its buffer, so saving over it again does not reallocate.
	ss_writer_rewind(&s->w);
	savestate_write(&s->w);
	if (s->w.failed) {
		ss_writer_free(&s->w);
		s->used = false;
		snprintf(err, err_len, "out of memory");
		return 0;
	}
	s->used = true;
	return s->w.len;
}

bool
savestate_slot_load(const char *name, size_t *size, char *err, size_t err_len)
{
	slot_t *s = name ? slot_find(name) : NULL;
	if (s == NULL) {
		snprintf(err, err_len, "no state saved in slot '%s'", name ? name : "");
		return false;
	}
	if (size) {
		*size = s->w.len;
	}
	return savestate_apply(s->w.data, s->w.len, err, err_len);
}

const void *
savestate_slot_data(const char *name, size_t *len)
{
	slot_t *s = name ? slot_find(name) : NULL;
	if (s == NULL) {
		return NULL;
	}
	if (len) {
		*len = s->w.len;
	}
	return s->w.data;
}
