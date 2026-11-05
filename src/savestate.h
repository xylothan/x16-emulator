// Commander X16 Emulator
// Save states: capture the whole machine into memory and put it back.
//
// A state holds everything the emulated machine needs to carry on exactly as
// it would have: CPU, memory, VERA and its audio, the YM2151, both VIAs, the
// controller ports, the SMC/RTC and their I2C bus, PS/2 queues and the SD card
// interface. It does not hold host-side things: the SD card image's contents,
// open host files, the debugger, profilers or the audio output queue.

#ifndef SAVESTATE_H
#define SAVESTATE_H

#include "savestate_io.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Whole states ────────────────────────────────────────────────────────────

// Serialize the machine. Saving only reads: a machine that was saved runs on
// exactly as one that was not.
void savestate_write(ss_writer *w);

// Check a state against this machine without touching anything. On a mismatch
// returns false with a reason in err.
bool savestate_check(const void *data, size_t len, char *err, size_t err_len);

// Overwrite the machine with a state that passed savestate_check().
bool savestate_apply(const void *data, size_t len, char *err, size_t err_len);

// True once any state has been loaded into this process. The machine's
// persistent files (-nvram, cartridge NVRAM) are never written after that,
// since they would record a history that did not happen.
bool savestate_any_loaded(void);

// ─── Named in-memory slots ───────────────────────────────────────────────────

#define SAVESTATE_MAX_SLOTS     64
#define SAVESTATE_MAX_SLOT_NAME 64

// Save the machine into a slot, replacing what it held. Returns the state's
// size, or 0 with a reason in err.
size_t savestate_slot_save(const char *name, char *err, size_t err_len);

// Load a slot. False with a reason in err; the machine is untouched then.
bool savestate_slot_load(const char *name, size_t *size, char *err, size_t err_len);

// The bytes a slot holds, or NULL.
const void *savestate_slot_data(const char *name, size_t *len);

// ─── Files ───────────────────────────────────────────────────────────────────
//
// A state file holds the same bytes as a slot, so it loads only into the build
// that wrote it, with the same machine configuration and ROM.

// Save the machine to a file, replacing it. Returns the state's size, or 0
// with a reason in err.
size_t savestate_file_save(const char *path, char *err, size_t err_len);

// Load a file. False with a reason in err; the machine is untouched then.
bool savestate_file_load(const char *path, size_t *size, char *err, size_t err_len);

// ─── Per-module serializers ──────────────────────────────────────────────────
//
// Each lives in its module's own file. A loader reads exactly what its saver
// wrote, in the same order; savestate_check() has already compared every
// chunk's tag, version and length with this machine's, so loaders do not
// validate.

void cpu_state_save(ss_writer *w);
void cpu_state_load(ss_reader *r);
void irq_ctx_state_save(ss_writer *w);
void irq_ctx_state_load(ss_reader *r);
void main_state_save(ss_writer *w);
void main_state_load(ss_reader *r);
void memory_state_save(ss_writer *w);
void memory_state_load(ss_reader *r);
void cartridge_state_save(ss_writer *w);
void cartridge_state_load(ss_reader *r);
// A hash of the cartridge's bank types and ROM banks, which a state does not
// hold; 0 without a cartridge.
uint64_t cartridge_rom_hash(void);
void video_state_save(ss_writer *w);
void video_state_load(ss_reader *r);
void psg_state_save(ss_writer *w);
void psg_state_load(ss_reader *r);
void pcm_state_save(ss_writer *w);
void pcm_state_load(ss_reader *r);
void audio_state_save(ss_writer *w);
void audio_state_load(ss_reader *r);
void YM_state_save(ss_writer *w);
void YM_state_load(ss_reader *r);
void via_state_save(ss_writer *w);
void via_state_load(ss_reader *r);
void joystick_state_save(ss_writer *w);
void joystick_state_load(ss_reader *r);
void i2c_state_save(ss_writer *w);
void i2c_state_load(ss_reader *r);
void smc_state_save(ss_writer *w);
void smc_state_load(ss_reader *r);
void rtc_state_save(ss_writer *w);
void rtc_state_load(ss_reader *r);
void vera_spi_state_save(ss_writer *w);
void vera_spi_state_load(ss_reader *r);
void sdcard_state_save(ss_writer *w);
void sdcard_state_load(ss_reader *r);
void ieee_state_save(ss_writer *w);
void ieee_state_load(ss_reader *r);

// main.c: re-base host-side measurements and drop pending typed text.
void main_state_loaded(void);

#ifdef __cplusplus
}
#endif

#endif
