// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#include "via.h"
#include "savestate.h"
#include "i2c.h"
#include "memory.h"
#include "serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
//XXX
#include "glue.h"
#include "joystick.h"

// Timers, after W65C22S datasheet Figures 2-3 and 2-4. A count is what the
// counter holds during a cycle; via_step() moves it across cycle ends. Writing
// T1C-H or T2C-H loads N, which the counter holds through the next cycle as
// well. After 0 comes FFFF, when the flag sets (IRQB falls N+1.5 cycles after
// the write); T1 then reloads its latch, so a free-running period is N+2, and
// T2 counts on.
typedef struct {
	unsigned timer_count[2];
	unsigned pb6_pulse_counts;
	uint8_t registers[15];
	bool timer1_m1;          // T1 holds FFFF after 0: the next cycle reloads
	bool timer_running[2];   // the timer sets its flag at the next time-out
	bool timer_loaded[2];    // written this cycle: the cycle ends without a decrement
	bool pb7_output;
	unsigned consumed;       // cycles stepped since the main loop last stepped
	// Pins, after the W65C22 datasheet (WDC): Table 2-5, Figures 4-3 to 4-7.
	// Every W65C22S pin has a bus holder (5.3), so a port pin nothing drives
	// keeps its last level.
	uint8_t pa_pins, pb_pins; // port pin levels
	bool ca1, ca2, cb1, cb2;  // control pin input levels
	uint8_t ira, irb;         // input registers, while latching holds them
	bool ca2_hs, cb2_hs;      // handshake output low until CA1 or CB1 is active
	uint8_t ca2_pulse, cb2_pulse; // cycle ends a pulse output has left low
} via_t;

static via_t via[2];

// only internal logic is handled here, see via1/2 calls for external
// operations specific to each unit

static void
via_init(via_t *via)
{
	// timer latches, timer counters and SR are not cleared
	for (int i = 0; i < 4; i++) via->registers[i] = 0;
	for (int i = 11; i < 15; i++) via->registers[i] = 0;
	via->timer_running[0] = false;
	via->timer_running[1] = false;
	via->timer_loaded[0] = false;
	via->timer_loaded[1] = false;
	via->timer1_m1 = false;
	via->consumed = 0;
	via->pb7_output = true;
	via->ca1 = via->ca2 = via->cb1 = via->cb2 = true;
	via->ca2_hs = via->cb2_hs = false;
	via->ca2_pulse = via->cb2_pulse = 0;
}

// The VIA drives the port pins DDR selects with OR.
static void
via_drive_ports(via_t *via)
{
	via->pa_pins = (via->pa_pins & ~via->registers[3]) | (via->registers[1] & via->registers[3]);
	via->pb_pins = (via->pb_pins & ~via->registers[2]) | (via->registers[0] & via->registers[2]);
}

// What IRA and IRB read for the input bits: the pins, or with latching on
// (ACR bits 0 and 1), the pins at the last active CA1 or CB1 transition.
static uint8_t
via_ira(const via_t *via)
{
	return (via->registers[11] & 0x01) ? via->ira : via->pa_pins;
}

static uint8_t
via_irb(const via_t *via)
{
	return (via->registers[11] & 0x02) ? via->irb : via->pb_pins;
}

// A CA2 or CB2 output mode (Table 2-5): 4 handshake, 5 pulse, 6 low, 7 high.
// Returns the level, or -1 for an input.
static int
via_c2_output(uint8_t mode, bool hs, uint8_t pulse)
{
	switch (mode) {
		case 4: return !hs;
		case 5: return pulse == 0;
		case 6: return 0;
		case 7: return 1;
		default: return -1;
	}
}

// An ORA or IRA access through register 1 (not 15): CA2 goes low, held until
// CA1 is active in handshake mode, for the next cycle in pulse mode
// (Figures 4-3 to 4-5). ORB writes do the same for CB2.
static void
via_ca2_handshake(via_t *via)
{
	via->ca2_hs = true;
	via->ca2_pulse = 2;
}

// A control pin's input level. CA1 and CB1 are active on the edge PCR bits 0
// and 4 select, CA2 and CB2 as inputs on the edge bits 2 and 6 select.
static void
via_control_input(via_t *via, int pin, bool level)
{
	const uint8_t pcr = via->registers[12];
	switch (pin) {
		case VIA_PIN_CA1:
			if (level != via->ca1 && level == ((pcr & 0x01) != 0)) {
				via->registers[13] |= 0x02;
				via->ca2_hs = false;
				via->ira = via->pa_pins;
			}
			via->ca1 = level;
			break;
		case VIA_PIN_CA2:
			if (level != via->ca2 && !(pcr & 0x08) && level == ((pcr & 0x04) != 0)) {
				via->registers[13] |= 0x01;
			}
			via->ca2 = level;
			break;
		case VIA_PIN_CB1:
			if (level != via->cb1 && level == ((pcr & 0x10) != 0)) {
				via->registers[13] |= 0x10;
				via->cb2_hs = false;
				via->irb = via->pb_pins;
			}
			via->cb1 = level;
			break;
		case VIA_PIN_CB2:
			if (level != via->cb2 && !(pcr & 0x80) && level == ((pcr & 0x40) != 0)) {
				via->registers[13] |= 0x08;
			}
			via->cb2 = level;
			break;
	}
}

static void
via_clear_pra_irqs(via_t *via)
{
	via->registers[13] &= ~0x02;
	if ((via->registers[12] & 0b00001010) != 0b00000010) {
		via->registers[13] &= ~0x01;
	}
}

static void
via_clear_prb_irqs(via_t *via)
{
	via->registers[13] &= ~0x10;
	if ((via->registers[12] & 0b10100000) != 0b00100000) {
		via->registers[13] &= ~0x08;
	}
}

static uint8_t
via_read(via_t *via, uint8_t reg, bool debug)
{
	uint8_t ifr;
	bool    irq;
	switch (reg) {
		case 0: // IRB: ORB on the output bits (Table 2-2)
			if (!debug) via_clear_prb_irqs(via);
			return (via->registers[0] & via->registers[2]) | (via_irb(via) & ~via->registers[2]);
		case 1: // IRA
			if (!debug) {
				via_clear_pra_irqs(via);
				via_ca2_handshake(via);
			}
			// fall through
		case 15: // IRA, no handshake
			return (via->registers[1] & via->registers[3]) | (via_ira(via) & ~via->registers[3]);
		case 4: // T1L
			if (!debug) via->registers[13] &= ~0x40;
			return (uint8_t)(via->timer_count[0] & 0xff);
		case 5: // T1H
			return (uint8_t)(via->timer_count[0] >> 8);
		case 8: // T2L
			if (!debug) via->registers[13] &= ~0x20;
			return (uint8_t)(via->timer_count[1] & 0xff);
		case 9: // T2H
			return (uint8_t)(via->timer_count[1] >> 8);
		case 10: // SR
			if (!debug) via->registers[13] &= ~0x04;
			return via->registers[10];
		case 13: // IFR
			ifr = via->registers[13];
			irq = (ifr & via->registers[14]) != 0;
			return ((uint8_t)irq << 7) | ifr;
		case 14: // IER
			return via->registers[14] | 0x80;
		default:
			return via->registers[reg];
	}
}

static void
via_write(via_t *via, uint8_t reg, uint8_t value)
{
	switch (reg) {
		case 0: // ORB
			via_clear_prb_irqs(via);
			via->cb2_hs = true;
			via->cb2_pulse = 2;
			via->registers[0] = value;
			via_drive_ports(via);
			break;
		case 1: // ORA
			via_clear_pra_irqs(via);
			via_ca2_handshake(via);
			// fall through
		case 15: // ORA, no handshake
			via->registers[1] = value;
			via_drive_ports(via);
			break;
		case 2: // DDRB
		case 3: // DDRA
			via->registers[reg] = value;
			via_drive_ports(via);
			break;
		case 4: // T1L
			via->registers[6] = value;
			break;
		case 5: // T1H
		case 7: // T1LH
			via->registers[13] &= ~0x40;
			via->registers[7] = value;
			if (reg == 5) {
				via->timer_count[0] = ((unsigned)value << 8) | via->registers[6];
				via->timer_running[0] = true;
				via->timer_loaded[0] = true;
				via->timer1_m1 = false;
				via->pb7_output = false;
			}
			break;
		case 9: // T2H
			via->registers[13] &= ~0x20;
			via->timer_count[1] = ((unsigned)value << 8) | via->registers[8];
			via->timer_running[1] = true;
			via->timer_loaded[1] = true;
			break;
		case 10: // SR
			via->registers[13] &= ~0x04;
			via->registers[10] = value;
			break;
		case 11: // ACR: latching starts from the pins as they are
			if (value & ~via->registers[11] & 0x01) via->ira = via->pa_pins;
			if (value & ~via->registers[11] & 0x02) via->irb = via->pb_pins;
			via->registers[11] = value;
			break;
		case 13: // IFR
			via->registers[13] &= ~(value & 0x7f);
			break;
		case 14: // IER
			if (value & 0x80) {
				via->registers[14] |= value & 0x7f;
			} else {
				via->registers[14] &= ~value & 0x7f;
			}
			break;
		default:
			via->registers[reg] = value;
	}
}

static void
via_step(via_t *via, unsigned clocks)
{
	const uint8_t acr = via->registers[11];
	uint8_t ifr = via->registers[13];

	// T1
	unsigned n = clocks;
	unsigned cnt = via->timer_count[0];
	if (n && via->timer_loaded[0]) {
		via->timer_loaded[0] = false;
		n--;
	}
	while (n) {
		if (via->timer1_m1) {
			const unsigned latch = ((unsigned)via->registers[7] << 8) | via->registers[6];
			via->timer1_m1 = false;
			cnt = latch;
			n--;
			// Once the flag is set or the one-shot spent, whole periods change
			// nothing but PB7's phase.
			const unsigned period = latch + 2;
			if (n >= period && (!via->timer_running[0] || (ifr & 0x40))) {
				const unsigned skip = n / period;
				if (via->timer_running[0] && (skip & 1)) {
					via->pb7_output ^= true;
				}
				n -= skip * period;
			}
			continue;
		}
		if (n <= cnt) {
			cnt -= n;
			break;
		}
		n -= cnt + 1;
		cnt = 0xffff;
		via->timer1_m1 = true;
		if (via->timer_running[0]) {
			ifr |= 0x40;
			via->pb7_output ^= true;
			if (!(acr & 0x40)) {
				via->timer_running[0] = false;
			}
		}
	}
	via->timer_count[0] = cnt;

	// T2
	cnt = via->timer_count[1];
	if (acr & 0x20) {
		n = via->pb6_pulse_counts;
	} else {
		n = clocks;
		if (n && via->timer_loaded[1]) {
			via->timer_loaded[1] = false;
			n--;
		}
	}
	via->pb6_pulse_counts = 0;
	if (n > cnt && via->timer_running[1]) {
		ifr |= 0x20;
		via->timer_running[1] = false;
	}
	via->timer_count[1] = (cnt - n) & 0xffff;

	// TODO shift register

	via->ca2_pulse = (uint8_t)(clocks >= via->ca2_pulse ? 0 : via->ca2_pulse - clocks);
	via->cb2_pulse = (uint8_t)(clocks >= via->cb2_pulse ? 0 : via->cb2_pulse - clocks);

	via->registers[13] = ifr;
}

// Bring a unit up to the cycle of an access, `offset` cycles after the main
// loop last stepped the devices.
static void
via_catch_up(via_t *via, unsigned offset)
{
	if (offset > via->consumed) {
		via_step(via, offset - via->consumed);
		via->consumed = offset;
	}
}

// The rest of the cycles since the main loop last stepped.
static void
via_step_rest(via_t *via, unsigned clocks)
{
	via_step(via, clocks > via->consumed ? clocks - via->consumed : 0);
	via->consumed = 0;
}

//
// VIA#1
//
// PA0: I2CDATA   I2C DATA
// PA1: I2CCLK    I2C CLK
// PA2: NESLATCH  NES LATCH (for all controllers)
// PA3: NESCLK    NES CLK   (for all controllers)
// PA4: NESDAT3   NES DATA  (controller 3)
// PA5: NESDAT2   NES DATA  (controller 2)
// PA6: NESDAT1   NES DATA  (controller 1)
// PA7: NESDAT0   NES DATA  (controller 0)
// PB0: -         Unused
// PB1: -         Unused
// PB2: -         Unused
// PB3: IECATTO   Serial ATN  out
// PB4: IECCLKO   Serial CLK  out
// PB5: IECDATAO  Serial DATA out
// PB6: IECCLKI   Serial CLK  in
// PB7: IECDATAI  Serial DATA in
// CA1: -         Unused
// CA2: -         Unused
// CB1: IECSRQ
// CB2: -         Unused

void
via1_init()
{
	via_init(&via[0]);
	i2c_port.clk_in = 1;
	serial_port.in.atn = 0;
	serial_port.in.clk = 0;
	serial_port.in.data = 0;
	serial_port.out.clk = 1;
	serial_port.out.data = 1;
}

// DDR=0 (input)  -> take input bit
// DDR=1 (output) -> take output bit
// For now, just assume that I2C peripherals always drive all lines and VIA
// overrides them with their input values based on current DDR bits
static uint8_t
via1_pb(void)
{
	return
		(~via[0].registers[2] & (
			(serial_port_read_clk() << 6) |
			(serial_port_read_data() << 7)
		)) |
		(via[0].registers[2] & (
			(serial_port.in.atn << 3) |
			((!serial_port.in.clk) << 4) |
			((!serial_port.in.data) << 5)
		));
}

static uint8_t
via1_pa(void)
{
	return (~via[0].registers[3] & i2c_port.data_out) |					//I2C Data: PA0=1 if DDR bit is 0 (input) and data_out is 1; usage of data_out and data_in is a bit confusing...
		(via[0].registers[3] & i2c_port.data_in) |						//I2C Data: PA0=1 if DDR bit is 1 (output) and data_in is 1
		(~via[0].registers[3] & I2C_CLK_MASK ) |						//I2C Clock: PA1=1 if DDR bit is 0 (input), simulating an input pull-up
		(via[0].registers[3] & i2c_port.clk_in) |						//I2C Clock: PA1=1 if DDR bit is 1 (output) and clk_in is 1, simulating a pin driven by the VIA
		Joystick_data;
}

uint8_t
via1_read(uint8_t reg, bool debug)
{
	// Nothing drives CA1 or CB1 (IECSRQ is not modelled), so with latching on
	// a port reads as it was when latching began.
	switch (reg) {
		case 0: // PB
			if (!debug) via_clear_prb_irqs(&via[0]);
			return (via[0].registers[11] & 2) ? via[0].irb : via1_pb();

		case 1: // PA
		case 15: // PA, no handshake
			// i2c_step() advances the bus state machine, so it has to sit behind
			// the debug check: reading $9F01 in a memory view shouldn't disturb
			// a transfer in progress. That bus talks to the SMC and the RTC.
			if (!debug) {
				i2c_step();
				if (reg == 1) {
					via_clear_pra_irqs(&via[0]);
					via_ca2_handshake(&via[0]);
				}
			}
			return (via[0].registers[11] & 1) ? via[0].ira : via1_pa();

		default:
			return via_read(&via[0], reg, debug);
	}
}

void
via1_write(uint8_t reg, uint8_t value)
{
	const uint8_t starts = reg == 11 ? value & ~via[0].registers[11] : 0;
	via_write(&via[0], reg, value);
	// Latching starts from the ports as they are.
	if (starts & 0x01) via[0].ira = via1_pa();
	if (starts & 0x02) via[0].irb = via1_pb();
	if (reg == 0 || reg == 2) {
		// PB
		const uint8_t pb = via[0].registers[0] | ~via[0].registers[2];
		serial_port.in.atn = (pb & SERIAL_ATNIN_MASK) != 0;
		serial_port.in.clk = (pb & SERIAL_CLOCKIN_MASK) == 0;
		serial_port.in.data = (pb & SERIAL_DATAIN_MASK) == 0;

	} else if (reg == 1 || reg == 3 || reg == 15) {
		i2c_step();
		// PA
		const uint8_t pa = via[0].registers[1] | ~via[0].registers[3];
		i2c_port.data_in = pa & I2C_DATA_MASK;										//Sets data_in = 1 if the corresponding DDR bit is 0 (input), simulates a pull-up
		i2c_port.clk_in = (pa & I2C_CLK_MASK) >> 1;									//Sets clk_in = 1 if pin is an input, simulates a pull-up
		joystick_set_latch(via[0].registers[1] & JOY_LATCH_MASK);
		joystick_set_clock(via[0].registers[1] & JOY_CLK_MASK);
	}
}

void
via1_step(unsigned clocks)
{
	via_step_rest(&via[0], clocks);
}

void
via1_catch_up(unsigned offset)
{
	via_catch_up(&via[0], offset);
}

bool
via1_irq()
{
	return (via[0].registers[13] & via[0].registers[14]) != 0;
}

//
// VIA#2
//
// PA/PB: user port
// for now, just assume that all user ports are not connected
// and reads return output register (open bus behavior)

void
via2_init()
{
	via_init(&via[1]);
}

uint8_t
via2_read(uint8_t reg, bool debug)
{
	return via_read(&via[1], reg, debug);
}

void
via2_write(uint8_t reg, uint8_t value)
{
	via_write(&via[1], reg, value);
}

void
via2_step(unsigned clocks)
{
	via_step_rest(&via[1], clocks);
}

void
via2_catch_up(unsigned offset)
{
	via_catch_up(&via[1], offset);
}

bool
via2_irq()
{
	return (via[1].registers[13] & via[1].registers[14]) != 0;
}

void
via2_drive(int pin, uint8_t level)
{
	via_t *v = &via[1];
	switch (pin) {
		case VIA_PIN_PA: v->pa_pins = level; break;
		case VIA_PIN_PB: v->pb_pins = level; break;
		default: via_control_input(v, pin, level != 0); break;
	}
}

void
via2_pins(via_pins_t *out)
{
	const via_t *v   = &via[1];
	const uint8_t acr = v->registers[11];
	const uint8_t pcr = v->registers[12];
	out->ca2   = (int8_t)via_c2_output((pcr >> 1) & 7, v->ca2_hs, v->ca2_pulse);
	out->cb1   = -1;
	out->cb2   = (int8_t)via_c2_output((pcr >> 5) & 7, v->cb2_hs, v->cb2_pulse);
	out->pa    = v->registers[1];
	out->pa_oe = v->registers[3];
	out->pb    = v->registers[0];
	out->pb_oe = v->registers[2];
	if ((acr & 0x80) && (out->pb_oe & 0x80)) {
		out->pb = (uint8_t)((out->pb & 0x7f) | (v->pb7_output << 7));
	}
}

// Side-effect-free snapshot of one VIA's state for the ImGui debugger.
// `which` is 0 for VIA1, 1 for VIA2.
void
via_debug_get_state(int which, via_debug_state_t *out)
{
	if (which < 0 || which > 1) {
		return;
	}
	const via_t *v = &via[which];
	for (int i = 0; i < 15; i++) {
		out->regs[i] = v->registers[i];
	}
	out->timer_count[0]   = (uint16_t)v->timer_count[0];
	out->timer_count[1]   = (uint16_t)v->timer_count[1];
	out->timer_latch[0]   = ((uint16_t)v->registers[7] << 8) | v->registers[6];
	out->timer_latch[1]   = v->registers[8]; // T2 only has latch low byte
	out->timer_running[0] = v->timer_running[0];
	out->timer_running[1] = v->timer_running[1];
	out->timer1_m1        = v->timer1_m1;
	out->pb7_output       = v->pb7_output;
}

// ─── Save states ─────────────────────────────────────────────────────────────
//
// Both units, saved whether or not VIA2 is fitted: the layout stays the same
// and an unfitted unit's state is simply never used.

static void
via_unit_io(via_t *v, ss_writer *w, ss_reader *r, int version)
{
	for (int i = 0; i < 2; i++) {
		if (w) ss_put_u32(w, v->timer_count[i]); else v->timer_count[i] = ss_get_u32(r);
	}
	if (w) ss_put_u32(w, v->pb6_pulse_counts); else v->pb6_pulse_counts = ss_get_u32(r);
	if (w) ss_put(w, v->registers, sizeof v->registers); else ss_get(r, v->registers, sizeof v->registers);
	if (w) ss_put_bool(w, v->timer1_m1); else v->timer1_m1 = ss_get_bool(r);
	for (int i = 0; i < 2; i++) {
		if (w) ss_put_bool(w, v->timer_running[i]); else v->timer_running[i] = ss_get_bool(r);
	}
	if (w) ss_put_bool(w, v->pb7_output); else v->pb7_output = ss_get_bool(r);
	// Version 1 has no loaded flags: a state is saved between instructions,
	// where a CPU write's cycle has always ended. A debugger write may not.
	for (int i = 0; i < 2; i++) {
		if (w) ss_put_bool(w, v->timer_loaded[i]); else v->timer_loaded[i] = version >= 2 && ss_get_bool(r);
	}
	// Version 3 adds the pins. Before it, the ports read as their output
	// registers and nothing moved a control pin.
	if (w || version >= 3) {
		uint8_t *bytes[] = { &v->pa_pins, &v->pb_pins, &v->ira, &v->irb, &v->ca2_pulse, &v->cb2_pulse };
		bool    *flags[] = { &v->ca1, &v->ca2, &v->cb1, &v->cb2, &v->ca2_hs, &v->cb2_hs };
		for (size_t i = 0; i < sizeof bytes / sizeof bytes[0]; i++) {
			if (w) ss_put_u8(w, *bytes[i]); else *bytes[i] = ss_get_u8(r);
		}
		for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
			if (w) ss_put_bool(w, *flags[i]); else *flags[i] = ss_get_bool(r);
		}
	} else {
		v->pa_pins = v->registers[1];
		v->pb_pins = v->registers[0];
		v->ira = v->irb = 0;
		v->ca2_pulse = v->cb2_pulse = 0;
		v->ca1 = v->ca2 = v->cb1 = v->cb2 = true;
		v->ca2_hs = v->cb2_hs = false;
	}
	if (r) v->consumed = 0;
}

void
via_state_save(ss_writer *w)
{
	ss_begin(w, "VIA ", 3);
	via_unit_io(&via[0], w, NULL, 3);
	via_unit_io(&via[1], w, NULL, 3);
	ss_end(w);
}

void
via_state_load(ss_reader *r)
{
	const int version = ss_open(r, "VIA ");
	via_unit_io(&via[0], NULL, r, version);
	via_unit_io(&via[1], NULL, r, version);
	ss_close(r);
}
