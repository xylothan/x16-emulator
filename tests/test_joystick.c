// The SNES controller ports: the latch/clock shift register, and the virtual
// joystick that lets automation drive a port with no gamepad attached.
//
// The properties worth pinning are the ones a script driving the machine over
// DAP depends on: that a button it holds actually reaches the data line, on the
// right bit for the right port; that the shift register clocks out LSB-first
// with the upper nibble forced high the way the hardware does; and -- the one
// that is easy to break -- that a port nothing is driving still reads as
// released, forever, rather than draining a shift register to zero and
// reporting every button pressed.
//
// joystick.c reaches SDL's headers and its controller-enumeration functions, so
// this links SDL, but never calls joystick_init(): the virtual path deliberately
// needs no SDL controller, and the test is here to prove that.

#include "support/harness.h"

#include "../src/joystick.h"

#include <string.h>

// Reading a port means latching, then clocking bits out one at a time. The
// latch emits bit 0 itself, so the loop starts at 1. Joystick_data is cleared
// first because the latch path ORs into it without clearing, exactly as it does
// when the VIA drives the line.
static uint16_t
read_slot(int slot, int bits)
{
	const uint8_t slot_bit = 0x80 >> slot;
	uint16_t      out      = 0;

	Joystick_data = 0;
	joystick_set_latch(true);
	if (Joystick_data & slot_bit) {
		out |= 1;
	}
	joystick_set_latch(false);

	for (int i = 1; i < bits; ++i) {
		joystick_set_clock(false);
		joystick_set_clock(true);
		if (Joystick_data & slot_bit) {
			out |= (uint16_t)(1u << i);
		}
	}

	return out;
}

static void
reset_joysticks(void)
{
	for (int i = 0; i < NUM_JOYSTICKS; ++i) {
		joystick_clear_virtual(i);
		Joystick_slots_enabled[i] = false;
	}
}

static void
test_virtual_button_reaches_the_data_line(void)
{
	reset_joysticks();

	// Active low: clearing a bit presses the button.
	joystick_set_virtual(0, (uint16_t)~JOY_BIT_A);
	check_eq(read_slot(0, 16), (uint16_t)(~JOY_BIT_A | 0xF000), "a pressed A clocks out on port 1");

	joystick_set_virtual(0, (uint16_t)~(JOY_BIT_DPAD_LEFT | JOY_BIT_B));
	check_eq(read_slot(0, 16), (uint16_t)(~(JOY_BIT_DPAD_LEFT | JOY_BIT_B) | 0xF000),
	         "two buttons held at once both clock out");
}

static void
test_upper_nibble_is_forced_high(void)
{
	reset_joysticks();

	// Every defined button pressed. Bits 12-15 have no button behind them and
	// the latch forces them high, so the port cannot report them as pressed.
	joystick_set_virtual(1, 0x0000);
	check_eq(read_slot(1, 16), 0xF000, "bits 12-15 read high even with every button held");
}

static void
test_each_port_is_independent(void)
{
	reset_joysticks();

	joystick_set_virtual(0, (uint16_t)~JOY_BIT_A);
	joystick_set_virtual(1, (uint16_t)~JOY_BIT_B);
	joystick_set_virtual(2, (uint16_t)~JOY_BIT_START);
	joystick_set_virtual(3, (uint16_t)~JOY_BIT_DPAD_UP);

	check_eq(read_slot(0, 16), (uint16_t)(~JOY_BIT_A | 0xF000), "port 1 reports only its own buttons");
	check_eq(read_slot(1, 16), (uint16_t)(~JOY_BIT_B | 0xF000), "port 2 reports only its own buttons");
	check_eq(read_slot(2, 16), (uint16_t)(~JOY_BIT_START | 0xF000), "port 3 reports only its own buttons");
	check_eq(read_slot(3, 16), (uint16_t)(~JOY_BIT_DPAD_UP | 0xF000), "port 4 reports only its own buttons");
}

static void
test_a_port_with_no_source_reads_released(void)
{
	reset_joysticks();

	// The compatibility guard, and the reason presence detection works at all.
	// A port nothing drives must keep reporting 1s past the sixteenth clock,
	// because that is how the KERNAL tells an empty port from an occupied one:
	// a real controller's shift register runs out and reads 0, while a floating
	// port stays high. Let this fall through to the shift register and every
	// port would report a controller plugged into it.
	check_eq(read_slot(0, 16), 0xFFFF, "an undriven port reads as nothing pressed");

	Joystick_data = 0;
	joystick_set_latch(true);
	joystick_set_latch(false);
	for (int i = 0; i < 40; ++i) {
		joystick_set_clock(false);
		joystick_set_clock(true);
		if (!(Joystick_data & 0x80)) {
			check(false, "an undriven port stays high past the sixteenth clock");
			return;
		}
	}
	check(true, "an undriven port stays high past the sixteenth clock");
}

static void
test_a_driven_port_runs_out(void)
{
	reset_joysticks();

	// The other half of that contract: a port something *is* driving has a
	// sixteen-bit shift register and nothing behind it, so the seventeenth
	// clock onwards reads 0. That difference is the presence signal, so a
	// virtual joystick has to reproduce it or nothing that checks whether a
	// controller is plugged in would see one.
	joystick_set_virtual(0, 0xFFFF);

	Joystick_data = 0;
	joystick_set_latch(true);
	joystick_set_latch(false);
	for (int i = 1; i < 16; ++i) {
		joystick_set_clock(false);
		joystick_set_clock(true);
	}

	joystick_set_clock(false);
	joystick_set_clock(true);
	check(!(Joystick_data & 0x80), "a driven port reads 0 once its sixteen bits run out");
}

static void
test_releasing_hands_the_port_back(void)
{
	reset_joysticks();

	joystick_set_virtual(2, (uint16_t)~JOY_BIT_Y);
	check_eq(read_slot(2, 16), (uint16_t)(~JOY_BIT_Y | 0xF000), "the held button reads back");

	joystick_clear_virtual(2);
	check(!joystick_virtual_active(2), "clearing deactivates the virtual joystick");
	check_eq(read_slot(2, 16), 0xFFFF, "a released port reads as nothing pressed again");
}

static void
test_holding_persists_across_reads(void)
{
	reset_joysticks();

	// A held direction has to survive being read every frame, or nothing that
	// polls the port more than once could be driven at all.
	joystick_set_virtual(0, (uint16_t)~JOY_BIT_DPAD_RIGHT);
	for (int frame = 0; frame < 3; ++frame) {
		check_eq(read_slot(0, 16), (uint16_t)(~JOY_BIT_DPAD_RIGHT | 0xF000),
		         "a held direction survives repeated reads");
	}
}

static void
test_driving_a_port_enables_it(void)
{
	reset_joysticks();

	check(!Joystick_slots_enabled[3], "the port starts disabled");
	joystick_set_virtual(3, (uint16_t)~JOY_BIT_SELECT);
	check(Joystick_slots_enabled[3], "driving a port enables it, so no -joy4 flag is needed");

	// Releasing leaves the port enabled: that bit is the user's configuration,
	// and a physical pad may bind to it now the virtual one has let go.
	joystick_clear_virtual(3);
	check(Joystick_slots_enabled[3], "releasing leaves the port enabled");
}

static void
test_debug_snapshot_reports_the_virtual_state(void)
{
	reset_joysticks();
	joystick_set_virtual(1, (uint16_t)~JOY_BIT_X);

	joystick_debug_state_t js;
	memset(&js, 0, sizeof(js));
	joystick_debug_get_state(&js);

	check(js.slots[1].virtual_active, "the snapshot reports the virtual joystick");
	check(!js.slots[1].controller_bound, "no controller is bound to a virtual port");
	check_eq(js.slots[1].button_mask, (uint16_t)~JOY_BIT_X, "the snapshot reports the held buttons");
	check(!js.slots[0].virtual_active, "an undriven port is not reported as virtual");
	check_eq(js.slots[0].button_mask, 0xFFFF, "an undriven port reports nothing pressed");
	check_eq(js.slots[0].shift_mask, 0xFFFF, "an undriven port reports the 1s it actually clocks out");
}

static void
test_an_out_of_range_port_is_ignored(void)
{
	reset_joysticks();

	joystick_set_virtual(-1, 0x0000);
	joystick_set_virtual(NUM_JOYSTICKS, 0x0000);
	joystick_clear_virtual(-1);
	joystick_clear_virtual(NUM_JOYSTICKS);

	check(!joystick_virtual_active(-1), "a negative port is not active");
	check(!joystick_virtual_active(NUM_JOYSTICKS), "a port past the last is not active");
	check_eq(read_slot(0, 16), 0xFFFF, "an out-of-range write does not spill onto a real port");
}

int
main(void)
{
	test_virtual_button_reaches_the_data_line();
	test_upper_nibble_is_forced_high();
	test_each_port_is_independent();
	test_a_port_with_no_source_reads_released();
	test_a_driven_port_runs_out();
	test_releasing_hands_the_port_back();
	test_holding_persists_across_reads();
	test_driving_a_port_enables_it();
	test_debug_snapshot_reports_the_virtual_state();
	test_an_out_of_range_port_is_ignored();

	return x16_test_summary("joystick");
}
