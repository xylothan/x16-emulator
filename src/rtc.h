// Commander X16 Emulator
// Copyright (c) 2021 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef _RTC_H_
#define _RTC_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void rtc_i2c_data(uint8_t v);
void rtc_init(bool set_system_time);
void rtc_set_system_time();
void rtc_step(int c);
uint8_t rtc_read();
void rtc_write();

// ─── Debugger accessor ───────────────────────────────────────────────────────
// Side-effect-free snapshot of RTC state. Safe to call every frame from the
// debugger while the emulation is running.
//
// Calendar fields are plain integers (not BCD). `nvram` is a copy of the
// 64-byte battery-backed NVRAM owned by the RTC.

typedef struct {
	bool    running;        // oscillator is running (ST bit)
	bool    h24;            // 24-hour mode (true) vs 12-hour AM/PM mode (false)
	int     seconds;        // 0-59
	int     minutes;        // 0-59
	int     hours;          // 0-23 (always stored as 24h internally)
	int     day_of_week;    // 1-7 (1=Monday per MCP7940N convention)
	int     day;            // 1-31
	int     month;          // 1-12
	int     year;           // 0-99 (years since 2000)
	uint8_t i2c_data_pos;   // write position in the internal I2C data buffer
	uint8_t nvram[0x40];    // copy of the 64-byte NVRAM (battery-backed)
	bool    nvram_dirty;    // NVRAM has been written since last save
} rtc_debug_state_t;

void rtc_debug_get_state(rtc_debug_state_t *out);

#ifdef __cplusplus
}
#endif

#endif
