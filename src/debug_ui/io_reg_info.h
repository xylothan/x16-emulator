// Commander X16 Emulator — names and purposes for the I/O page registers.
//
// The Activity log and the VIA tab both show raw addresses, and a raw address
// is not something anyone should have to look up in a manual mid-debug. This
// maps $9F00-$9FFF to a short column-width name and a tooltip-length
// description of what the register is for.
//
// Sourced from the emulator's own decode -- video.c, via.c, memory.c and
// ymglue.h -- rather than from the reference manual, so what is described is
// what this emulator actually does. Where the two differ, the code wins,
// because the code is what the user is looking at.
#ifndef DEBUG_UI_IO_REG_INFO_H
#define DEBUG_UI_IO_REG_INFO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Short identifier for an I/O address, e.g. "DATA0", "T1C_L", "SPI_CTRL".
// Never NULL -- unmapped addresses report "-".
//
// A trailing '*' means the register is banked and its identity depends on
// machine state the log cannot recover after the fact: the four VERA registers
// at $9F29-$9F2C are selected by DCSEL, so a historical access cannot be
// attributed to one meaning with certainty.
const char *io_reg_name(uint16_t addr);

// One or two sentences on what the register does, for a tooltip. Never NULL.
const char *io_reg_purpose(uint16_t addr);

// What a VIA port pin is physically wired to on the X16, for `which` 0 (VIA1)
// or 1 (VIA2), port 'A' or 'B', bit 0-7. Never NULL.
const char *io_via_pin_name(int which, char port, int bit);

// Name of a 65C22 IFR/IER bit, 0-7.
const char *io_via_irq_bit_name(int bit);

// Name of an SMC register offset, and what it does. Both never NULL.
const char *io_smc_reg_name(uint8_t reg);
const char *io_smc_reg_purpose(uint8_t reg);

// Name of an RTC register offset. Never NULL.
const char *io_rtc_reg_name(uint8_t reg);

#ifdef __cplusplus
}
#endif

#endif
