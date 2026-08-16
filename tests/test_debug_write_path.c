// The write half of the debugOn contract.
//
// tests/test_debugon_contract.c pins the read half: a debug read reports what
// is there, moves no device state and spends no cycles. This is the other
// half, which until recently did not exist -- write6502() took no flag, so
// nothing downstream could tell a debugger's edit from a store the program
// executed, and every consequence of a store followed.
//
// The rule, decided rather than discovered:
//
//     a debugger's write updates the machine and does nothing else. The byte
//     reads as though it had always been there.
//
// Two consequences follow, and each is asserted against the real-write case
// beside it, because either alone reads as an unremarkable fact about slow I/O
// or about watchpoints:
//
//   * No cycles. A debugger that spent them would change program timing by
//     being used, which is the same reason a debug read spends none.
//   * No watchpoint. A watchpoint answers "when does the program touch this?",
//     and the program did not.
//
// What a debug write does do is write. The device is updated, its side effects
// happen, and the value is there afterwards -- that is the point of the edit,
// and the half that must not be suppressed.
//
// ORACLE: none, and none is possible. Whether a debugger's write should be
// distinguishable from the program's is a design question about this emulator,
// not a question about hardware: the machine cannot tell that a debugger
// exists. These assertions pin a decision, not a measurement.
//
// Devices are recording fakes, so what is pinned is how memory.c routes and
// charges, not how any real device behaves.

#include "support/fake_devices.h"
#include "support/harness.h"

#include "debug_core.h"
#include "memory.h"

#include <stdint.h>

extern uint32_t clockticks6502;

// Defined in fake_devices.c, which does not declare it in its header; the
// existing watchpoint test externs it the same way.
extern int fake_break_count;

// $9F40-$9F5F is the slow IO2 range: three cycles on a real access.
#define ADDR_SLOW_IO 0x9F41
#define ADDR_VERA    0x9F23
#define ADDR_RAM     0x0200

static void
reset(void)
{
	fake_devices_reset();
	debug_wp_clear_all();
	fake_break_count = 0;
}

static void
test_a_debug_write_spends_no_cycles(void)
{
	reset();
	write6502(ADDR_SLOW_IO, 0, 0x42);
	check_eq(clockticks6502, 3, "the program's write to slow I/O spends three cycles");

	reset();
	debug_write6502(ADDR_SLOW_IO, 0, 0x42);
	check_eq(clockticks6502, 0, "the debugger's write to the same address spends none");

	// The other slow range, which charges from a different branch.
	reset();
	write6502(0x9FA0, 0, 0x42);
	check_eq(clockticks6502, 3, "and the same holds in the slow IO5-7 range");

	reset();
	debug_write6502(0x9FA0, 0, 0x42);
	check_eq(clockticks6502, 0, "where the debugger's write is also free");

	// Plain RAM costs nothing either way, which keeps the above honest.
	reset();
	write6502(ADDR_RAM, 0, 0x42);
	check_eq(clockticks6502, 0, "a write to plain RAM spends none either way");
}

static void
test_a_debug_write_reports_no_watchpoint(void)
{
	reset();
	debug_wp_add_for(ADDR_RAM, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);

	write6502(ADDR_RAM, 0, 0x11);
	check_eq(fake_break_count, 1, "the program's store on a watched address breaks");

	reset();
	debug_wp_add_for(ADDR_RAM, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);

	debug_write6502(ADDR_RAM, 0, 0x22);
	check_eq(fake_break_count, 0, "the debugger's edit of the same address does not");
	check_eq(read6502(ADDR_RAM, 0), 0x22, "and the byte is written regardless");
}

// The half that must survive: an edit is only useful if it lands.
static void
test_a_debug_write_still_updates_the_machine(void)
{
	reset();
	debug_write6502(ADDR_RAM, 0, 0x5A);
	check_eq(read6502(ADDR_RAM, 0), 0x5A, "a debug write to RAM is there afterwards");

	reset();
	debug_write6502(ADDR_VERA, 0, 0x42);
	check_eq(fake_dev[FAKE_VERA].writes, 1, "a debug write reaches the device");
	check_eq(fake_dev[FAKE_VERA].last_value, 0x42, "carrying its value");
	check_eq(fake_dev[FAKE_VERA].state, 1, "and moves it, because writing the port is the point");
}

// Everything the machine can see afterwards is the same. Only the things that
// exist for the debugger's benefit -- the clock and the watchpoint -- differ,
// and they are what "does nothing else" means.
static void
test_the_two_writes_leave_the_same_machine(void)
{
	reset();
	write6502(ADDR_VERA, 0, 0x42);
	const uint32_t by_program = fake_devices_state_sum();
	const int      writes     = fake_dev[FAKE_VERA].writes;
	const uint8_t  value      = fake_dev[FAKE_VERA].last_value;

	reset();
	debug_write6502(ADDR_VERA, 0, 0x42);

	check_eq(fake_devices_state_sum(), by_program, "both writes leave identical device state");
	check_eq(fake_dev[FAKE_VERA].writes, writes, "both are counted as a write");
	check_eq(fake_dev[FAKE_VERA].last_value, value, "and both delivered the same byte");
}

int
main(void)
{
	test_a_debug_write_spends_no_cycles();
	test_a_debug_write_reports_no_watchpoint();
	test_a_debug_write_still_updates_the_machine();
	test_the_two_writes_leave_the_same_machine();
	return x16_test_summary("debug_write_path");
}
