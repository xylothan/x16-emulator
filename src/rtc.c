// Commander X16 Emulator
// Copyright (c) 2021 Michael Steil
// All rights reserved. License: 2-clause BSD

// MCP7940N RTC, after Microchip DS20005010J: the registers of Table 5-1 with
// the access and reset values of Registers 5-1 to 5-19, the address pointer of
// Section 6.1, and the alarm flags of Section 5.4. The MFP pin, the square
// wave, trimming and the power-fail time-stamps are not connected to anything.

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include "rtc.h"
#include "savestate.h"
#include "glue.h"

bool nvram_dirty = false;
uint8_t nvram[0x40];                // SRAM, 0x20-0x5F

static uint8_t rtcc[0x20];          // RTCC registers, 0x00-0x1F, as written
static uint8_t pointer;             // address pointer
static bool    expect_address;      // the next byte written sets the pointer
static unsigned int clocks;

#define RTCSEC   0x00
#define RTCMIN   0x01
#define RTCHOUR  0x02
#define RTCWKDAY 0x03
#define RTCDATE  0x04
#define RTCMTH   0x05
#define RTCYEAR  0x06
#define CONTROL  0x07
#define ALM0     0x0a
#define ALM1     0x11

#define ST       0x80
#define H12      0x40
#define PM       0x20
#define OSCRUN   0x20
#define LPYR     0x20
#define ALMxIF   0x08
#define ALMPOL   0x80

// Writable bits. RTCWKDAY: OSCRUN is read-only and a write clears PWRFAIL.
// ALMxWKDAY: a write clears ALMxIF, and ALM1WKDAY's ALMPOL mirrors ALM0's.
// ALMxHOUR's 12/24 mirrors RTCHOUR's. 0x09, 0x10 and 0x17 are reserved.
static const uint8_t writable[0x20] = {
	0xff, 0x7f, 0x7f, 0x0f, 0x3f, 0x1f, 0xff, 0xff, 0xff, 0x00,
	0x7f, 0x7f, 0x3f, 0xf7, 0x3f, 0x1f, 0x00,
	0x7f, 0x7f, 0x3f, 0x77, 0x3f, 0x1f, 0x00,
	0x7f, 0x7f, 0x3f, 0xff, 0x7f, 0x7f, 0x3f, 0xff,
};

static const uint8_t por[0x20] = {
	[RTCWKDAY] = 0x01, [RTCDATE] = 0x01, [RTCMTH] = 0x01, [RTCYEAR] = 0x01,
	[CONTROL] = 0x80,
	[ALM0 + 3] = 0x01, [ALM0 + 4] = 0x01, [ALM0 + 5] = 0x01,
	[ALM1 + 3] = 0x01, [ALM1 + 4] = 0x01, [ALM1 + 5] = 0x01,
};

#define BCD(a) (((a) / 10) << 4 | ((a) % 10))
#define UNBCD(a) (((a) >> 4) * 10 + ((a) & 0xf))

static bool
leap_year(void)
{
	// 2000-2099, where every year divisible by 4 is a leap year
	return UNBCD(rtcc[RTCYEAR]) % 4 == 0;
}

// An hours register in 0-23, in the format RTCHOUR's 12/24 bit selects.
static int
hour24(uint8_t h)
{
	if (rtcc[RTCHOUR] & H12) {
		return UNBCD(h & 0x1f) % 12 + ((h & PM) ? 12 : 0);
	}
	return UNBCD(h & 0x3f);
}

static void
set_hour24(int v)
{
	if (rtcc[RTCHOUR] & H12) {
		const int h = v % 12 ? v % 12 : 12;
		rtcc[RTCHOUR] = H12 | (v >= 12 ? PM : 0) | BCD(h);
	} else {
		rtcc[RTCHOUR] = BCD(v);
	}
}

// Section 5.4, Table 5-5: a flag is set while its enabled alarm matches.
static void
alarms(void)
{
	for (int a = 0; a < 2; a++) {
		if (!(rtcc[CONTROL] & (0x10 << a))) {
			continue;
		}
		const uint8_t *al = &rtcc[a ? ALM1 : ALM0];
		const bool sec  = (al[0] & 0x7f) == (rtcc[RTCSEC] & 0x7f);
		const bool min  = (al[1] & 0x7f) == (rtcc[RTCMIN] & 0x7f);
		const bool hour = hour24(al[2]) == hour24(rtcc[RTCHOUR]);
		const bool wday = (al[3] & 7) == (rtcc[RTCWKDAY] & 7);
		const bool date = (al[4] & 0x3f) == (rtcc[RTCDATE] & 0x3f);
		const bool mth  = (al[5] & 0x1f) == (rtcc[RTCMTH] & 0x1f);
		bool match;
		switch ((al[3] >> 4) & 7) {
			case 0: match = sec; break;
			case 1: match = min; break;
			case 2: match = hour; break;
			case 3: match = wday; break;
			case 4: match = date; break;
			case 7: match = sec && min && hour && wday && date && mth; break;
			default: match = false; break;
		}
		if (match) {
			rtcc[(a ? ALM1 : ALM0) + 3] |= ALMxIF;
		}
	}
}

void
rtc_init(bool set_system_time)
{
	memcpy(rtcc, por, sizeof rtcc);
	pointer = 0;
	expect_address = false;
	clocks = 0;

	if (set_system_time) {
		time_t t = time(NULL);
		struct tm tm = *localtime(&t);
		rtcc[RTCSEC]   = ST | BCD(tm.tm_sec);
		rtcc[RTCMIN]   = BCD(tm.tm_min);
		rtcc[RTCHOUR]  = BCD(tm.tm_hour);
		rtcc[RTCWKDAY] = 0x08 | (tm.tm_wday == 0 ? 7 : tm.tm_wday);   // VBATEN
		rtcc[RTCDATE]  = BCD(tm.tm_mday);
		rtcc[RTCMTH]   = BCD(tm.tm_mon + 1);
		rtcc[RTCYEAR]  = BCD(tm.tm_year % 100);
	}
}

static const uint8_t days_per_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static void
tick(void)
{
	int s = UNBCD(rtcc[RTCSEC] & 0x7f) + 1;
	if (s < 60) {
		rtcc[RTCSEC] = ST | BCD(s);
		return;
	}
	rtcc[RTCSEC] = ST;
	int m = UNBCD(rtcc[RTCMIN]) + 1;
	if (m < 60) {
		rtcc[RTCMIN] = BCD(m);
		return;
	}
	rtcc[RTCMIN] = 0;
	int h = hour24(rtcc[RTCHOUR]) + 1;
	if (h < 24) {
		set_hour24(h);
		return;
	}
	set_hour24(0);
	const int wd = (rtcc[RTCWKDAY] & 7) + 1;
	rtcc[RTCWKDAY] = (rtcc[RTCWKDAY] & ~7) | (wd > 7 ? 1 : wd);
	int month = UNBCD(rtcc[RTCMTH] & 0x1f);
	int dpm = month >= 1 && month <= 12 ? days_per_month[month - 1] : 31;
	if (month == 2 && leap_year()) {
		dpm++;
	}
	int d = UNBCD(rtcc[RTCDATE]) + 1;
	if (d <= dpm) {
		rtcc[RTCDATE] = BCD(d);
		return;
	}
	rtcc[RTCDATE] = 1;
	if (++month <= 12) {
		rtcc[RTCMTH] = BCD(month);
		return;
	}
	rtcc[RTCMTH] = 1;
	rtcc[RTCYEAR] = BCD((UNBCD(rtcc[RTCYEAR]) + 1) % 100);
}

void
rtc_step(int c)
{
	if (!(rtcc[RTCSEC] & ST)) {
		return;
	}
	clocks += c;
	while (clocks >= (MHZ * 1000000)) {
		clocks -= (MHZ * 1000000);
		tick();
		alarms();
	}
}

static uint8_t
reg_read(uint8_t a)
{
	if (a >= 0x20) {
		return nvram[a - 0x20];
	}
	uint8_t v = rtcc[a] & (writable[a] | ALMxIF * (a == ALM0 + 3 || a == ALM1 + 3));
	switch (a) {
		case RTCWKDAY:
			// The oscillator is taken to run as soon as ST is set.
			return v | ((rtcc[RTCSEC] & ST) ? OSCRUN : 0);
		case RTCMTH:
			return v | (leap_year() ? LPYR : 0);
		case ALM0 + 2:
		case ALM1 + 2:
			return v | (rtcc[RTCHOUR] & H12);
		case ALM1 + 3:
			return v | (rtcc[ALM0 + 3] & ALMPOL);
	}
	return v;
}

static void
reg_write(uint8_t a, uint8_t v)
{
	if (a >= 0x20) {
		nvram[a - 0x20] = v;
		nvram_dirty = true;
		return;
	}
	rtcc[a] = v & writable[a];
	alarms();
}

static void
advance(void)
{
	// Each block wraps on itself: 0x1F to 0x00, 0x5F to 0x20 (Section 6.1.2).
	if (pointer < 0x20) {
		pointer = (pointer + 1) & 0x1f;
	} else {
		pointer = pointer == 0x5f ? 0x20 : pointer + 1;
	}
}

bool
rtc_i2c_start(bool read)
{
	expect_address = !read;
	return true;
}

bool
rtc_i2c_write(uint8_t v)
{
	if (expect_address) {
		// Past 0x5F neither the address nor its data is acknowledged.
		if (v >= 0x60) {
			return false;
		}
		pointer = v;
		expect_address = false;
		return true;
	}
	reg_write(pointer, v);
	advance();
	return true;
}

uint8_t
rtc_i2c_read(void)
{
	const uint8_t v = reg_read(pointer);
	advance();
	return v;
}

void
rtc_i2c_stop(void)
{
	expect_address = false;
}

// Side-effect-free snapshot of RTC state for the ImGui debugger.
void
rtc_debug_get_state(rtc_debug_state_t *out)
{
	out->running      = (rtcc[RTCSEC] & ST) != 0;
	out->h24          = !(rtcc[RTCHOUR] & H12);
	out->seconds      = UNBCD(rtcc[RTCSEC] & 0x7f);
	out->minutes      = UNBCD(rtcc[RTCMIN]);
	out->hours        = hour24(rtcc[RTCHOUR]);
	out->day_of_week  = rtcc[RTCWKDAY] & 7;
	out->day          = UNBCD(rtcc[RTCDATE]);
	out->month        = UNBCD(rtcc[RTCMTH] & 0x1f);
	out->year         = UNBCD(rtcc[RTCYEAR]);
	out->i2c_data_pos = pointer;
	out->nvram_dirty  = nvram_dirty;
	for (int i = 0; i < 0x40; i++) {
		out->nvram[i] = nvram[i];
	}
}

// ─── Save states ─────────────────────────────────────────────────────────────
//
// The registers, the sub-second cycle accumulator, the NVRAM and the address
// pointer. Version 1 held decoded time fields and no alarm or control
// registers; those load at their reset values. A loaded state never marks the
// NVRAM for writing back to the -nvram file (see savestate_any_loaded()).

void
rtc_state_save(ss_writer *w)
{
	ss_begin(w, "RTC ", 2);
	ss_put(w, nvram, sizeof nvram);
	ss_put(w, rtcc, sizeof rtcc);
	ss_put_u8(w, pointer);
	ss_put_bool(w, expect_address);
	ss_put_u32(w, clocks);
	ss_end(w);
}

void
rtc_state_load(ss_reader *r)
{
	const int version = ss_open(r, "RTC ");
	ss_get(r, nvram, sizeof nvram);
	if (version < 2) {
		const bool running = ss_get_bool(r);
		const bool vbaten  = ss_get_bool(r);
		const bool h24     = ss_get_bool(r);
		clocks             = ss_get_u32(r);
		int32_t t[7];
		uint8_t old[16];
		ss_get(r, t, sizeof t);
		ss_get(r, old, sizeof old);
		(void)ss_get_u8(r);
		memcpy(rtcc, por, sizeof rtcc);
		rtcc[RTCHOUR]  = h24 ? 0 : H12;
		rtcc[RTCSEC]   = (running ? ST : 0) | BCD(t[0] % 60);
		rtcc[RTCMIN]   = BCD(t[1] % 60);
		set_hour24(t[2] % 24);
		rtcc[RTCWKDAY] = (vbaten ? 0x08 : 0) | (t[3] & 7);
		rtcc[RTCDATE]  = BCD(t[4] % 32);
		rtcc[RTCMTH]   = BCD(t[5] % 13);
		rtcc[RTCYEAR]  = BCD(t[6] % 100);
		pointer        = old[0] < 0x60 ? old[0] : 0;
		expect_address = false;
	} else {
		ss_get(r, rtcc, sizeof rtcc);
		pointer        = ss_get_u8(r);
		expect_address = ss_get_bool(r);
		clocks         = ss_get_u32(r);
		if (pointer >= 0x60) pointer = 0;
	}
	ss_close(r);
	nvram_dirty = false;
}
