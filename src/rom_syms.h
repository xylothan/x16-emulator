// Commander X16 Emulator — ROM symbols for the profiler.
//
// The x16-rom release ships one VICE label file per ROM bank next to rom.bin
// (kernal.sym, basic.sym, ...), lines like
//
//     al 00FFD2 .bsout
//
// Loaded, they name ROM time by routine instead of by page. Which file is
// which bank is fixed by the ROM's build (x16-rom Makefile, rom_labels.h
// target): kernal 0, keymap 1, dos 2, fat32 3, basic 4, monitor 5,
// charset 6, diag 7, graph 8, demo 9, audio 10, util 11, bannex 12.
//
// Kept: labels in that bank's ROM window ($C000-$FFFF), and from kernal.sym
// also the KERNAL's code in low RAM ($0200-$03FF: the IRQ entry, jsrfar's RAM
// half). Dropped: ld65's segment bookkeeping (__FOO_RUN__, __FOO_LOAD__ ...),
// and zero-page and other data addresses, which name variables, not code.
//
// Parsing is separate from file access and unit-tested (tests/test_rom_syms.c).
#ifndef ROM_SYMS_H
#define ROM_SYMS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROM_SYMS_KRAM_START 0x0200u // KERNAL code copied to low RAM
#define ROM_SYMS_KRAM_END   0x0400u // exclusive

typedef struct {
	const char *name;
	uint16_t    addr;
	int16_t     bank; // ROM bank for $C000-$FFFF; -1 for KERNAL low-RAM code
} rom_sym_t;

// The ROM bank a release .sym file describes, by base name ("basic.sym"), or
// -1 if it is not one of them.
int rom_syms_bank_of_file(const char *basename);

// Add the labels from one .sym file's text, as ROM bank `bank`. Returns how
// many were kept.
int rom_syms_add_text(const char *text, int bank);

// Load every known .sym file found in `dir` (the folder holding rom.bin).
// Returns the number of labels loaded; 0 if there were none.
int rom_syms_load_dir(const char *dir);

void             rom_syms_clear(void);
int              rom_syms_count(void);
const rom_sym_t *rom_syms_at(int i);

#ifdef __cplusplus
}
#endif

#endif
