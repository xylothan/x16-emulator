#include "fake_devices.h"

#include <stddef.h>
#include <string.h>

#include "glue.h"
#include "cartridge.h"
#include "wav_recorder.h"
#include "memory.h"

extern uint32_t clockticks6502;

fake_dev_log_t fake_dev[FAKE_DEV_COUNT];

const char *
fake_dev_name(fake_dev_t dev)
{
	switch (dev) {
		case FAKE_VIA1: return "VIA1";
		case FAKE_VIA2: return "VIA2";
		case FAKE_VERA: return "VERA";
		case FAKE_YM:   return "YM";
		case FAKE_MIDI: return "MIDI";
		case FAKE_CART: return "cartridge";
		default:        return "?";
	}
}

static uint8_t
note_read(fake_dev_t dev, uint8_t reg, bool debug)
{
	fake_dev_log_t *d = &fake_dev[dev];
	d->reads++;
	d->last_reg = reg;
	if (debug) {
		d->debug_reads++;
	} else {
		// What a debug read is not allowed to cause.
		d->state++;
	}
	return (uint8_t)(0xD0 | (reg & 0x0F));
}

static void
note_write(fake_dev_t dev, uint8_t reg, uint8_t value)
{
	fake_dev_log_t *d = &fake_dev[dev];
	d->writes++;
	d->last_reg = reg;
	d->last_value = value;
	d->state++;
}

void
fake_devices_reset(void)
{
	// memory.c allocates RAM and BRAM lazily, so the first reset has to bring
	// the memory system up before anything reads through it.
	static bool memory_ready = false;
	if (!memory_ready) {
		memory_init();
		memory_ready = true;
	}

	memset(fake_dev, 0, sizeof fake_dev);
	clockticks6502 = 0;
}

uint32_t
fake_devices_state_sum(void)
{
	uint32_t sum = 0;
	for (int i = 0; i < FAKE_DEV_COUNT; i++) {
		sum += fake_dev[i].state;
	}
	return sum;
}

int
fake_devices_total_reads(void)
{
	int n = 0;
	for (int i = 0; i < FAKE_DEV_COUNT; i++) {
		n += fake_dev[i].reads;
	}
	return n;
}

int
fake_devices_total_writes(void)
{
	int n = 0;
	for (int i = 0; i < FAKE_DEV_COUNT; i++) {
		n += fake_dev[i].writes;
	}
	return n;
}

// ---- The devices memory.c routes to --------------------------------------

uint8_t via1_read(uint8_t reg, bool debug)  { return note_read(FAKE_VIA1, reg, debug); }
uint8_t via2_read(uint8_t reg, bool debug)  { return note_read(FAKE_VIA2, reg, debug); }
uint8_t video_read(uint8_t reg, bool debug) { return note_read(FAKE_VERA, reg, debug); }
uint8_t midi_serial_read(uint8_t reg, bool debug) { return note_read(FAKE_MIDI, reg, debug); }

void via1_write(uint8_t reg, uint8_t value)  { note_write(FAKE_VIA1, reg, value); }
void via2_write(uint8_t reg, uint8_t value)  { note_write(FAKE_VIA2, reg, value); }
void video_write(uint8_t reg, uint8_t value) { note_write(FAKE_VERA, reg, value); }
void midi_serial_write(uint8_t reg, uint8_t value) { note_write(FAKE_MIDI, reg, value); }
void YM_write_reg(uint8_t reg, uint8_t value) { note_write(FAKE_YM, reg, value); }

// YM_read_status() has no debug parameter, so this cannot behave differently
// for a debug read. Recorded as a real read, which is the honest reading of a
// signature that carries no way to say otherwise.
uint8_t
YM_read_status(void)
{
	return note_read(FAKE_YM, 0, false);
}

uint8_t
cartridge_read(uint16_t address, uint8_t bank)
{
	(void)bank;
	return note_read(FAKE_CART, (uint8_t)(address & 0x0F), false);
}

void
cartridge_write(uint16_t address, uint8_t bank, uint8_t value)
{
	(void)bank;
	note_write(FAKE_CART, (uint8_t)(address & 0x0F), value);
}

// ---- Everything else memory.c reaches for --------------------------------
// Inert on purpose: none of it is what these tests are about, and a stub that
// does nothing cannot quietly become part of the result.

void audio_render(void) {}
void print_iso8859_15_char(char c) { (void)c; }
void dbg_load_note_debugger(bool on) { (void)on; }
void DEBUGBreakOnWatchpoint(void) {}
uint8_t wav_recorder_get_state(void) { return 0; }
void wav_recorder_set(wav_recorder_command_t command) { (void)command; }

// The CPU core signals a halt (STP) through this. memory.c already provides
// vp6502(), the vector-pull callback.
void stop6502(uint16_t address, uint8_t bank) { (void)address; (void)bank; }

// SDL is reachable from memory.c only through memory_save() and the usage-count
// dump, neither of which these tests call. Defining them here keeps the test
// free of an SDL runtime.
size_t SDL_RWwrite(SDL_RWops *c, const void *p, size_t s, size_t n)
{
	(void)c; (void)p; (void)s;
	return n;
}
SDL_RWops *SDL_RWFromFile(const char *f, const char *m) { (void)f; (void)m; return NULL; }
int SDL_RWclose(SDL_RWops *c) { (void)c; return 0; }

// ---- Machine configuration -----------------------------------------------
// A Gen1 machine with both VIAs and no MIDI card, which is what the X16 is.
//
// Every one of these is initialised deliberately. An uninitialised global is a
// tentative definition, which some linkers quietly merge with a real one
// elsewhere and others reject; initialising them means a clash with a linked
// module is a hard error everywhere rather than only on GCC.

uint8_t *CART = NULL;
uint8_t  keymap = 0;
bool     is_gen2 = false;
bool     has_via2 = true;
bool     has_midi_card = false;
uint16_t midi_card_addr = 0x9f60;
uint16_t num_banks = 1;
uint16_t num_ram_banks = 64;
// opcode_addr belongs to the CPU core, which is linked here for real.

bool debugger_enabled = false;
bool imgui_debugger_enabled = false;
bool disable_emu_cmd_keys = false;
bool log_keyboard = false;
bool log_video = false;
bool save_on_exit = false;

echo_mode_t echo_mode = ECHO_MODE_NONE;
gif_recorder_state_t record_gif = RECORD_GIF_PAUSE;
