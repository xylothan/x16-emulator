// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef VIA_H
#define VIA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void via1_init();
uint8_t via1_read(uint8_t reg, bool debug);
void via1_write(uint8_t reg, uint8_t value);
void via1_step(unsigned clocks);
void via1_catch_up(unsigned offset);
bool via1_irq();

void via2_init();
uint8_t via2_read(uint8_t reg, bool debug);
void via2_write(uint8_t reg, uint8_t value);
void via2_step(unsigned clocks);
void via2_catch_up(unsigned offset);
bool via2_irq();

// VIA2's pins, for tests: nothing on the machine drives them.
enum { VIA_PIN_CA1, VIA_PIN_CA2, VIA_PIN_CB1, VIA_PIN_CB2, VIA_PIN_PA, VIA_PIN_PB };
// Drive an input pin to LEVEL (a port, all 8 bits) from now on.
void via2_drive(int pin, uint8_t level);
typedef struct {
	int8_t  ca2, cb1, cb2;   // 0, 1, or -1 when not driven
	uint8_t pa, pa_oe;       // port value, and the bits that drive
	uint8_t pb, pb_oe;
} via_pins_t;
void via2_pins(via_pins_t *out);

// ─── Debugger accessor ───────────────────────────────────────────────────────
// Side-effect-free snapshot of one VIA's state. Safe to call every frame from
// the debugger while the emulation is running.
//
// Register index map (mirrors the MOS 6522 address decoding):
//   0=ORB/IRB  1=ORA/IRA  2=DDRB  3=DDRA
//   4=T1L      5=T1H      6=T1LL  7=T1LH
//   8=T2L      9=T2H(not stored, see timer_count)
//   10=SR      11=ACR     12=PCR  13=IFR  14=IER
//
// `which`: 0 = VIA1, 1 = VIA2.

typedef struct {
	// Whether VIA2 is fitted is a machine-configuration fact (has_via2 in
	// glue.h), not something this module knows -- and reaching for it here
	// would give via.c an external symbol it does not otherwise need, which
	// the standalone VIA tests link without. The caller decides.
	uint8_t  regs[15];         // raw register bytes (indices 0-14, see map above)
	uint16_t timer_count[2];   // live timer counter values (T1=0, T2=1)
	uint16_t timer_latch[2];   // T1 latch = (regs[7]<<8)|regs[6]; T2 latch low = regs[8]
	bool     timer_running[2]; // timer is actively counting
	bool     timer1_m1;        // T1 is in the special -1 state between underflow and reload
	bool     pb7_output;       // PB7 toggle output (used with T1 in one-shot/square-wave mode)
} via_debug_state_t;

void via_debug_get_state(int which, via_debug_state_t *out);

#ifdef __cplusplus
}
#endif

#endif
