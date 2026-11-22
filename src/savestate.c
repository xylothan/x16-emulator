// Commander X16 Emulator
// Save states: the header, the order modules are saved in, the check a state
// must pass before it is loaded, named in-memory slots and state files.

#include "savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "glue.h"
#include "cartridge.h"
#include "code_map.h"
#include "debug_server.h"
#include "host_prof.h"
#include "ieee.h"
#include "perf_budget.h"
#include "perf_hist.h"
#include "sdcard.h"
#include "sprite_trace.h"
#include "timing.h"
#include "vera_bandwidth.h"
#include "version.h"
#include "waterfall.h"
#include "x16_hash.h"
#include "cpu/fake6502.h"
#include "cpu/registers.h"

extern struct regs regs;
extern uint8_t *CART;
extern bool ym2151_irq_support;
extern bool has_serial;
extern bool headless;
extern bool no_ieee_intercept;

#define SAVESTATE_FORMAT 1

// ─── Which machine wrote a state ─────────────────────────────────────────────
//
// A state's header names three things about the emulator that wrote it, and
// each is treated as what it is:
//
// * SAVESTATE_LAYOUT_VERSION: what the bytes mean. A state of another layout
//   cannot be read, so it is refused. Bump it when a change alters what a
//   state holds or how it is laid out (a chunk gains, loses or reorders a
//   field, or a value changes meaning), unless the chunk can take a new chunk
//   version whose loader still reads the old one.
// * SAVESTATE_MACHINE_VERSION: how the machine runs. Bump it whenever a change
//   alters what the emulated machine does (CPU, memory, VERA, the sound chips,
//   VIAs, SMC, any of their timing). A state from another machine version
//   still loads, since its bytes mean what they did, but it will not run on
//   as it did when it was saved, and the load says so (savestate_last_origin(),
//   the x16/loadState reply, a console line). Changes that leave the machine
//   alone (the debugger, DAP, the host window, keys, audio sent to the host)
//   keep both versions.
// * The toolchain: compiler and version, target, Debug or Release. Layout does
//   not depend on it (savestate_io.h), but floating-point arithmetic can, so a
//   state from another toolchain loads and is reported like one from another
//   machine version.
//
// testbench/test_machine_version.py, run in CI, replays fixed workloads and
// fails when their hashes change under an unchanged machine version, or their
// chunk list changes under an unchanged layout version.
//
// x16_source_id, a hash of every source file, decides nothing; it is reported
// over DAP (x16/status) so a state's writer can still be traced.

#define SAVESTATE_LAYOUT_VERSION  1
#define SAVESTATE_MACHINE_VERSION 2

extern const char x16_source_id[];

#ifndef X16_BUILD_CONFIG
#define X16_BUILD_CONFIG ""
#endif
#define SS_STR2(x) #x
#define SS_STR(x)  SS_STR2(x)
#if defined(_MSC_FULL_VER)
#define SS_COMPILER "msvc " SS_STR(_MSC_FULL_VER)
#elif defined(__clang__)
#define SS_COMPILER "clang " __clang_version__
#elif defined(__GNUC__)
#define SS_COMPILER "gcc " __VERSION__
#else
#define SS_COMPILER "cc"
#endif
#if defined(__EMSCRIPTEN__)
#define SS_ARCH "wasm"
#elif defined(_M_ARM64) || defined(__aarch64__)
#define SS_ARCH "arm64"
#elif defined(_M_X64) || defined(__x86_64__)
#define SS_ARCH "x64"
#elif defined(_M_IX86) || defined(__i386__)
#define SS_ARCH "x86"
#elif defined(__arm__)
#define SS_ARCH "arm"
#else
#define SS_ARCH "other"
#endif

#define SAVESTATE_BUILD_LEN 64

static uint32_t
toolchain_hash(void)
{
	static const char toolchain[] = SS_COMPILER " " SS_ARCH " " X16_BUILD_CONFIG;
	return (uint32_t)x16_hash_bytes(X16_HASH_INIT, toolchain, strlen(toolchain));
}

// The ID, NUL-padded to SAVESTATE_BUILD_LEN bytes, which is how a header holds
// it: "x16emu layout <L> machine <M> toolchain <8 hex digits>".
static const char *
build_id(void)
{
	static char id[SAVESTATE_BUILD_LEN];
	if (id[0] == '\0') {
		snprintf(id, sizeof id, "x16emu layout %d machine %d toolchain %08x", SAVESTATE_LAYOUT_VERSION,
		         SAVESTATE_MACHINE_VERSION, (unsigned)toolchain_hash());
	}
	return id;
}

// IDs from before the layout version, which are read as layout 1:
//
// * "x16emu machine <M>-<toolchain>": every such build wrote layout 1, its
//   structs laid out as the images that now write them spell out.
// * Builds identified by a hash of their sources, only those listed here. Each
//   is known to hold layout 1 and to run as the machine version given (its
//   states, loaded there and here, run on with the same hash every frame).
//   Others are refused: chunk versions were not bumped while the source hash
//   guarded every change, so one might hold a same-length chunk that means
//   something else.
//
// When SAVESTATE_LAYOUT_VERSION is bumped, empty this list and drop the
// "machine <M>-" form from parse_build().
static const struct {
	const char *id;
	int         machine;
	uint32_t    toolchain;
} legacy_builds[] = {
	{ "x16emu r50 4666e4fd3e96ba94-fafd4819", 1, 0xfafd4819 }, // main @ 0e22ecb (#134), MSVC x64 Release
};

// What a header's build ID says. False when this build cannot read the state:
// another layout, or an ID it does not know.
static bool
parse_build(const char *build, savestate_origin *o)
{
	unsigned tc     = 0;
	int      layout = -1, machine = -1, n = 0;
	if (sscanf(build, "x16emu layout %d machine %d toolchain %8x%n", &layout, &machine, &tc, &n) == 3 &&
	    build[n] == '\0') {
		// this format
	} else if ((n = 0, sscanf(build, "x16emu machine %d-%8x%n", &machine, &tc, &n) == 2) && build[n] == '\0') {
		layout = 1;
	} else {
		layout = machine = -1;
		for (size_t i = 0; i < sizeof legacy_builds / sizeof legacy_builds[0]; i++) {
			if (strcmp(build, legacy_builds[i].id) == 0) {
				layout  = 1;
				machine = legacy_builds[i].machine;
				tc      = legacy_builds[i].toolchain;
			}
		}
	}
	memset(o, 0, sizeof *o);
	o->layout  = layout;
	o->machine = machine;
	snprintf(o->toolchain, sizeof o->toolchain, "%08x", tc);
	o->machine_changed   = machine != SAVESTATE_MACHINE_VERSION;
	o->toolchain_changed = (uint32_t)tc != toolchain_hash();
	return layout == SAVESTATE_LAYOUT_VERSION;
}

const char *
savestate_build_id(void)
{
	return build_id();
}

int
savestate_layout_version(void)
{
	return SAVESTATE_LAYOUT_VERSION;
}

int
savestate_machine_version(void)
{
	return SAVESTATE_MACHINE_VERSION;
}

const char *
savestate_toolchain(void)
{
	static char tc[12];
	if (tc[0] == '\0') {
		snprintf(tc, sizeof tc, "%08x", (unsigned)toolchain_hash());
	}
	return tc;
}

const char *
savestate_source_id(void)
{
	return x16_source_id;
}

// The header's build ID sits right after its chunk header (tag, version,
// reserved, length: 12 bytes). It names who wrote the state, not what the
// machine is, so the hash steps over it: the same machine run by two builds of
// one machine version hashes the same in both.
#define BUILD_FIELD_AT 12

uint64_t
savestate_hash(const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	if (len < BUILD_FIELD_AT + SAVESTATE_BUILD_LEN) {
		return x16_hash_bytes(X16_HASH_INIT, p, len);
	}
	uint64_t h = x16_hash_bytes(X16_HASH_INIT, p, BUILD_FIELD_AT);
	return x16_hash_bytes(h, p + BUILD_FIELD_AT + SAVESTATE_BUILD_LEN, len - BUILD_FIELD_AT - SAVESTATE_BUILD_LEN);
}

// What a state does not hold but depends on: the ROM, and a cartridge's ROM
// banks. Neither can change once the machine is running, so each is hashed once.
static uint64_t
rom_hash(void)
{
	static uint64_t h = 0;
	if (h == 0) {
		h = x16_hash_bytes(X16_HASH_INIT, ROM, ROM_SIZE);
	}
	return h;
}

static uint64_t
cart_hash(void)
{
	static bool     done = false;
	static uint64_t h    = 0;
	if (!done) {
		h    = cartridge_rom_hash();
		done = true;
	}
	return h;
}

static bool loaded_any = false;

bool
savestate_any_loaded(void)
{
	return loaded_any;
}

static uint32_t boundary_ticks = 0;
static bool     boundary_seen  = false;

void
savestate_mark_frame_boundary(void)
{
	boundary_ticks = clockticks6502;
	boundary_seen  = true;
}

bool
savestate_at_frame_boundary(void)
{
	return boundary_seen && clockticks6502 == boundary_ticks;
}

// ─── Header ──────────────────────────────────────────────────────────────────
//
// The machine a state was taken from. Loading into a machine configured
// differently would put, say, 2 MB of banked RAM into a machine with 512 KB,
// or VIA2 state into a machine without one, so each field must match.
// Options that only set what the machine starts with (-prg, -nvram, -seed) or
// how the host shows and plays it (-warp, -sound, -scale) are not here.

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
	out[n++] = (shape_field){ "YM2151 IRQ (-enable-ym2151-irq)", ym2151_irq_support };
	out[n++] = (shape_field){ "serial bus (-serial)", has_serial };
	out[n++] = (shape_field){ "MIDI card (-midicard)", has_midi_card };
	out[n++] = (shape_field){ "cartridge (-cart)", CART != NULL };
	out[n++] = (shape_field){ "Gen 2 machine", is_gen2 };
	out[n++] = (shape_field){ "-testbench", headless };
	out[n++] = (shape_field){ "mid-line effects (-midline-effects)", enable_midline };
	out[n++] = (shape_field){ "host filesystem (-nohostfs)", !no_ieee_intercept };
	out[n++] = (shape_field){ "host filesystem in use", using_hostfs };
	out[n++] = (shape_field){ "SD card (-sdcard)", sdcard_attached };
	return n;
}

#define SHAPE_MAX 24

static void
header_save(ss_writer *w)
{
	ss_begin(w, "X16S", SAVESTATE_FORMAT);
	// build_id() is a zero-filled buffer of exactly this size.
	ss_put(w, build_id(), SAVESTATE_BUILD_LEN);
	shape_field shape[SHAPE_MAX];
	const int n = current_shape(shape);
	ss_put_u32(w, (uint32_t)n);
	for (int i = 0; i < n; i++) {
		ss_put_u32(w, shape[i].value);
	}
	ss_put_u64(w, rom_hash());
	ss_put_u64(w, cart_hash());
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

// Who wrote the state header_check() last looked at (whether or not it was
// then loaded), and the last one loaded.
static savestate_origin checked_origin, loaded_origin;
static bool             checked_valid;

const savestate_origin *
savestate_last_origin(void)
{
	return loaded_any ? &loaded_origin : NULL;
}

const savestate_origin *
savestate_checked_origin(void)
{
	return checked_valid ? &checked_origin : NULL;
}

// Only the header is read, so this works on the first bytes of a file too.
static bool
header_check(const void *data, size_t len, char *err, size_t err_len)
{
	checked_valid = false;
	ss_reader r;
	ss_reader_init(&r, data, len);
	if (ss_open(&r, "X16S") != SAVESTATE_FORMAT) {
		snprintf(err, err_len, "not a save state from this emulator");
		return false;
	}
	char build[SAVESTATE_BUILD_LEN];
	ss_get(&r, build, sizeof build);
	build[sizeof build - 1] = '\0';
	if (r.failed) {
		snprintf(err, err_len, "the state's header is damaged");
		return false;
	}
	const bool readable = parse_build(build, &checked_origin);
	checked_valid       = checked_origin.layout >= 0;
	if (!readable) {
		if (checked_origin.layout < 0) {
			snprintf(err, err_len, "the state was saved by an emulator whose states this one cannot read (%s; this is %s)",
			         build, build_id());
		} else {
			snprintf(err, err_len, "the state has save-state layout %d and this emulator reads layout %d",
			         checked_origin.layout, SAVESTATE_LAYOUT_VERSION);
		}
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
	const uint64_t rom  = ss_get_u64(&r);
	const uint64_t cart = ss_get_u64(&r);
	if (!r.failed && rom != rom_hash()) {
		snprintf(err, err_len, "the state was saved with a different ROM (-rom)");
		return false;
	}
	if (!r.failed && cart != cart_hash()) {
		snprintf(err, err_len, "the state was saved with a different cartridge (-cart)");
		return false;
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

// How many bytes a state of this machine is. Counting writes nothing, and
// saving only reads, so this leaves the machine as it was.
static size_t
expected_size(ss_layout *layout, bool *ok)
{
	ss_writer counter;
	ss_writer_init(&counter, true, layout);
	savestate_write(&counter);
	*ok = !counter.failed;
	return counter.len;
}

bool
savestate_check(const void *data, size_t len, char *err, size_t err_len)
{
	if (!header_check(data, len, err, err_len)) {
		return false;
	}

	// What a state of this machine looks like, chunk by chunk.
	ss_layout expect, got;
	bool ok;
	expected_size(&expect, &ok);
	if (!ok) {
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

// A state from another machine version or toolchain loads, but will not, or
// may not, run on as it did when it was saved. Said once for each such writer,
// since a client may load the same state thousands of times.
static void
note_origin(const savestate_origin *o)
{
	if (!o->machine_changed && !o->toolchain_changed) {
		return;
	}
	static char said[8][32];
	static int  n_said;
	char        key[32];
	snprintf(key, sizeof key, "%d-%s", o->machine, o->toolchain);
	for (int i = 0; i < n_said; i++) {
		if (strcmp(said[i], key) == 0) {
			return;
		}
	}
	if (n_said < 8) {
		snprintf(said[n_said++], sizeof said[0], "%s", key);
	}
	if (o->machine_changed) {
		printf("Save state: saved by machine version %d, and this is %d. It loaded, but will not run on exactly as it did when saved.\n",
		       o->machine, SAVESTATE_MACHINE_VERSION);
	} else {
		printf("Save state: saved by another toolchain (%s, this is %s). It loaded, but may not run on exactly as it did when saved.\n",
		       o->toolchain, savestate_toolchain());
	}
}

bool
savestate_apply(const void *data, size_t len, bool allow_behaviour_change, char *err, size_t err_len)
{
	if (!savestate_check(data, len, err, err_len)) {
		return false;
	}
	// Readable, but saved by a machine that ran differently: from here it would
	// not do what it did when saved, which a caller replaying recordings must
	// know before it trusts the state. So the caller has to ask.
	if (checked_origin.machine_changed && !allow_behaviour_change) {
		snprintf(err, err_len,
		         "the state was saved by machine version %d and this is %d, so the machine would not run on "
		         "from it as it did when saved; loading it anyway has to be asked for "
		         "(x16/loadState allowBehaviourChange)",
		         checked_origin.machine, SAVESTATE_MACHINE_VERSION);
		return false;
	}

	ss_reader r;
	ss_reader_init(&r, data, len);
	savestate_read(&r);
	loaded_any    = true;
	loaded_origin = checked_origin;
	note_origin(&loaded_origin);
	savestate_mark_frame_boundary(); // every state is taken on one
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

// Whether the machine can be saved now: states do not cover everything yet,
// and refusing is better than a state that would come back missing a device.
static bool
can_save(char *err, size_t err_len)
{
	if (has_serial || has_midi_card) {
		snprintf(err, err_len, "a machine with %s cannot be saved", has_serial ? "-serial" : "-midicard");
		return false;
	}
	char why[128];
	if (ieee_state_busy(why, sizeof why)) {
		snprintf(err, err_len, "cannot save while %s", why);
		return false;
	}
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
	if (!can_save(err, err_len)) {
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
	// Saved by this process, so by this machine: there is no behaviour to change.
	return savestate_apply(s->w.data, s->w.len, false, err, err_len);
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

// ─── Files ───────────────────────────────────────────────────────────────────
//
// A file holds exactly the bytes a slot would. SDL opens it, so a UTF-8 path
// works on Windows too.

size_t
savestate_file_save(const char *path, char *err, size_t err_len)
{
	if (path == NULL || path[0] == '\0') {
		snprintf(err, err_len, "'path' must name a file");
		return 0;
	}
	if (!can_save(err, err_len)) {
		return 0;
	}
	// Kept between saves, so saving again does not reallocate.
	static ss_writer w;
	static bool      w_ready = false;
	if (!w_ready) {
		ss_writer_init(&w, false, NULL);
		w_ready = true;
	}
	ss_writer_rewind(&w);
	savestate_write(&w);
	if (w.failed) {
		snprintf(err, err_len, "out of memory");
		return 0;
	}
	SDL_RWops *f = SDL_RWFromFile(path, "wb");
	if (f == NULL) {
		snprintf(err, err_len, "cannot write %s", path);
		return 0;
	}
	const size_t written = SDL_RWwrite(f, w.data, 1, w.len);
	const int    closed  = SDL_RWclose(f);
	if (written != w.len || closed != 0) {
		snprintf(err, err_len, "writing %s failed part-way; the file is incomplete", path);
		return 0;
	}
	return w.len;
}

bool
savestate_file_load(const char *path, size_t *size, bool allow_behaviour_change, char *err, size_t err_len)
{
	if (path == NULL || path[0] == '\0') {
		snprintf(err, err_len, "'path' must name a file");
		return false;
	}
	SDL_RWops *f = SDL_RWFromFile(path, "rb");
	if (f == NULL) {
		snprintf(err, err_len, "cannot open %s", path);
		return false;
	}
	const Sint64 file_len = SDL_RWsize(f);
	bool ok;
	const size_t expect = expected_size(NULL, &ok);
	if (file_len < 0 || !ok) {
		SDL_RWclose(f);
		snprintf(err, err_len, "cannot read %s", path);
		return false;
	}
	// A file of any size but this machine's is not a state of it. Its header,
	// if it has one, usually says why better than its size does.
	const size_t want = (size_t)file_len == expect ? expect
	                  : (size_t)file_len < 4096 ? (size_t)file_len : 4096;
	uint8_t *data = (uint8_t *)malloc(want ? want : 1);
	if (data == NULL) {
		SDL_RWclose(f);
		snprintf(err, err_len, "out of memory");
		return false;
	}
	const size_t got = SDL_RWread(f, data, 1, want);
	SDL_RWclose(f);
	bool loaded = false;
	if (got != want) {
		snprintf(err, err_len, "cannot read %s", path);
	} else if ((size_t)file_len != expect) {
		if (header_check(data, got, err, err_len)) {
			snprintf(err, err_len, "%s is %lld bytes; a state of this machine is %llu",
			         path, (long long)file_len, (unsigned long long)expect);
		}
	} else {
		loaded = savestate_apply(data, got, allow_behaviour_change, err, err_len);
		if (loaded && size) {
			*size = got;
		}
	}
	free(data);
	return loaded;
}
