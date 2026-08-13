// Regression: watching memory must not change what the machine does.
//
// The watchpoint hook sits in write6502(), on the path every store takes. It
// runs before the write and does not return early, which is what lets a store
// still land while the debugger is told about it -- but nothing pinned that,
// and "the debugger swallowed my write" is a bug that would look like the
// emulated program being wrong.
//
// The wider claim is the one that matters: setting a watchpoint changes nothing
// except that a break is reported. These compare a whole machine signature
// either side of it rather than a list of fields, because the failures worth
// catching are the ones nobody thought to name.
//
// memory.c, debug_core.c and the CPU core are real here; only the devices
// behind the I/O page are fakes.

#include "support/fake_devices.h"
#include "support/harness.h"
#include "support/signature.h"

#include "debug_core.h"
#include "memory.h"

#include <stdint.h>

// Declared locally for the same reason the other tests do it: the only
// declarations live in headers that reach SDL.
extern uint8_t *RAM;
extern uint32_t clockticks6502;

#define SCRATCH 0x0400
#define VERA_REG 0x9F23

// fake_devices.c counts these for us.
extern int fake_break_count;

static void
setup(void)
{
	fake_devices_reset();
	debug_wp_clear_all();
	fake_break_count = 0;
}

// ---- A watched store still reaches memory ----------------------------------

static void
test_watched_store_still_writes(void)
{
	setup();
	RAM[SCRATCH] = 0x00;
	debug_wp_add_for(SCRATCH, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);

	write6502(SCRATCH, 0, 0x5A);

	check_eq(RAM[SCRATCH], 0x5A, "a watched address still receives the store");
	check(fake_break_count > 0, "and the watchpoint reported a break");
}

// ---- A watched I/O store still reaches the device --------------------------
// The one most likely to break, because the device is behind another layer.

static void
test_watched_io_store_still_reaches_device(void)
{
	setup();
	debug_wp_add_for(VERA_REG, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);

	write6502(VERA_REG, 0, 0x77);

	check_eq(fake_dev[FAKE_VERA].writes, 1, "a watched I/O store still reaches VERA");
	check_eq(fake_dev[FAKE_VERA].last_value, 0x77, "with the value the program wrote");
	check(fake_break_count > 0, "and the watchpoint reported a break");
}

// ---- Watching an address does not change the store itself ------------------

static void
test_watch_does_not_alter_the_machine(void)
{
	// Run the same store with and without a watchpoint, and compare everything
	// except the break count, which is the one thing that should differ.
	setup();
	write6502(SCRATCH, 0, 0x11);
	write6502(VERA_REG, 0, 0x22);
	machine_sig_t unwatched = machine_signature();

	setup();
	debug_wp_add_for(SCRATCH, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);
	write6502(SCRATCH, 0, 0x11);
	write6502(VERA_REG, 0, 0x22);
	machine_sig_t watched = machine_signature();

	check(machine_sig_equal(unwatched, watched),
	      "a watchpoint changes nothing about the stores themselves");
}

// ---- A watchpoint elsewhere is inert ---------------------------------------

static void
test_unrelated_watch_is_inert(void)
{
	setup();
	write6502(SCRATCH, 0, 0x33);
	machine_sig_t clean = machine_signature();

	setup();
	debug_wp_add_for(0x0500, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);
	write6502(SCRATCH, 0, 0x33);
	machine_sig_t with_other = machine_signature();

	check(machine_sig_equal(clean, with_other),
	      "a watchpoint on another address leaves the machine alone");
	check_eq(fake_break_count, 0, "and does not report a break");
}

// ---- Watching costs no cycles ----------------------------------------------
// The scan runs on every store, so if it charged anything the whole machine
// would run slower whenever a watchpoint was set.

static void
test_watching_costs_no_cycles(void)
{
	setup();
	write6502(SCRATCH, 0, 0x44);
	uint32_t plain = clockticks6502;

	setup();
	debug_wp_add_for(SCRATCH, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);
	write6502(SCRATCH, 0, 0x44);
	check_eq(clockticks6502, plain, "a watched store costs the same as any other");

	// And on a slow I/O address, where a store does cost cycles.
	setup();
	write6502(0x9FA0, 0, 0x55);
	uint32_t io_plain = clockticks6502;

	setup();
	debug_wp_add_for(0x9FA0, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);
	write6502(0x9FA0, 0, 0x55);
	check_eq(clockticks6502, io_plain, "and a watched slow-I/O store costs the same");
}

// ---- A value filter decides when to break, not whether to write ------------

static void
test_value_filter_still_writes(void)
{
	setup();
	debug_wp_add_for(SCRATCH, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);
	debug_wp_set_value(SCRATCH, DEBUG_BANK_ANY, BPCMP_EQ, 0x99);

	write6502(SCRATCH, 0, 0x01);
	check_eq(RAM[SCRATCH], 0x01, "a store that fails the filter still writes");
	check_eq(fake_break_count, 0, "and reports no break");

	write6502(SCRATCH, 0, 0x99);
	check_eq(RAM[SCRATCH], 0x99, "a store that matches the filter also writes");
	check(fake_break_count > 0, "and reports a break");
}

// ---- An inactive watchpoint is inert ---------------------------------------

static void
test_inactive_watch_is_inert(void)
{
	setup();
	debug_wp_add_for(SCRATCH, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);
	debug_wp_set_active(SCRATCH, DEBUG_BANK_ANY, false);

	write6502(SCRATCH, 0, 0x66);
	check_eq(RAM[SCRATCH], 0x66, "a disabled watchpoint still lets the store through");
	check_eq(fake_break_count, 0, "and reports no break");
}

int
main(void)
{
	test_watched_store_still_writes();
	test_watched_io_store_still_reaches_device();
	test_watch_does_not_alter_the_machine();
	test_unrelated_watch_is_inert();
	test_watching_costs_no_cycles();
	test_value_filter_still_writes();
	test_inactive_watch_is_inert();
	return x16_test_summary("watchpoint_purity");
}
