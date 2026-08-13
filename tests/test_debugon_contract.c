// Regression: the debugOn contract.
//
// real_read6502() carries a debugOn flag down to every device behind the I/O
// page. The rule it encodes, nowhere written down and nowhere enforced, has two
// halves:
//
//   * a debug read reports what is there and disturbs nothing -- no device
//     state moved, no cycles spent;
//   * a real read does both, because that is what the hardware does.
//
// It matters because the debugger reads memory constantly: a memory view
// refreshing over the I/O page would otherwise step every device it touches.
// That is not hypothetical -- a debug read of VIA port A was advancing the I2C
// bus until recently, which on this machine talks to the SMC and the RTC.
//
// Devices here are recording fakes (tests/support/fake_devices.c), so these
// assertions are about how memory.c routes and guards, not about how any real
// device behaves. memory.c, debug_core.c and the CPU core are the real thing.

#include "support/fake_devices.h"
#include "support/harness.h"

#include "memory.h"
#include "cpu/fake6502.h"

#include <stdio.h>

extern struct regs regs;
extern uint32_t clockticks6502;

// Addresses that reach a device, one per fake we can address by name.
#define ADDR_VIA1  0x9F01
#define ADDR_VIA2  0x9F11
#define ADDR_VERA  0x9F23
#define ADDR_MIDI  0x9F61

static uint8_t
debug_read(uint16_t addr)
{
	return debug_read6502(addr, 0, USE_CURRENT_X16_BANK);
}

// ---- A debug read reaches the right device, and says it is a debug read ----

static void
test_debug_read_routing(void)
{
	const struct {
		uint16_t   addr;
		fake_dev_t dev;
		uint8_t    reg;
	} cases[] = {
		{ ADDR_VIA1, FAKE_VIA1, 0x1 },
		{ ADDR_VIA2, FAKE_VIA2, 0x1 },
		{ ADDR_VERA, FAKE_VERA, 0x3 },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		char label[96];
		fake_devices_reset();
		debug_read(cases[i].addr);

		const fake_dev_log_t *d = &fake_dev[cases[i].dev];
		snprintf(label, sizeof label, "$%04X reaches %s",
		         cases[i].addr, fake_dev_name(cases[i].dev));
		check_eq(d->reads, 1, label);

		snprintf(label, sizeof label, "$%04X arrives at %s as a debug read",
		         cases[i].addr, fake_dev_name(cases[i].dev));
		check_eq(d->debug_reads, 1, label);

		snprintf(label, sizeof label, "$%04X selects register %X",
		         cases[i].addr, cases[i].reg);
		check_eq(d->last_reg, cases[i].reg, label);
	}
}

// ---- A real read is not flagged as a debug read ----------------------------

static void
test_real_read_is_not_flagged(void)
{
	fake_devices_reset();
	read6502(ADDR_VERA, 0);
	check_eq(fake_dev[FAKE_VERA].reads, 1, "a real read reaches the device");
	check_eq(fake_dev[FAKE_VERA].debug_reads, 0, "and is not flagged as a debug read");
	check_eq(fake_dev[FAKE_VERA].state, 1, "and is allowed to move device state");
}

// ---- A debug read moves no device state ------------------------------------

static void
test_debug_read_moves_no_state(void)
{
	fake_devices_reset();
	debug_read(ADDR_VERA);
	check_eq(fake_dev[FAKE_VERA].state, 0, "a debug read leaves VERA's state alone");

	fake_devices_reset();
	debug_read(ADDR_VIA1);
	check_eq(fake_dev[FAKE_VIA1].state, 0, "a debug read leaves VIA1's state alone");

	// Repeating it must stay free: a memory view refreshes constantly.
	fake_devices_reset();
	for (int i = 0; i < 16; i++) {
		debug_read(ADDR_VERA);
	}
	check_eq(fake_dev[FAKE_VERA].reads, 16, "sixteen debug reads all arrived");
	check_eq(fake_dev[FAKE_VERA].state, 0, "and none of them moved anything");
}

// ---- A debug read costs no cycles ------------------------------------------
// The slow I/O ranges charge three cycles on a real access. A debugger that
// spent those would change program timing just by being open.

static void
test_debug_read_costs_nothing(void)
{
	// $9F40-$9F5F, the slow IO2 range.
	fake_devices_reset();
	debug_read(0x9F41);
	check_eq(clockticks6502, 0, "a debug read of $9F41 costs no cycles");

	fake_devices_reset();
	read6502(0x9F41, 0);
	check_eq(clockticks6502, 3, "and a real read of $9F41 costs three");

	// $9FA0-$9FFF, the slow IO5-7 range.
	fake_devices_reset();
	debug_read(0x9FA0);
	check_eq(clockticks6502, 0, "a debug read of $9FA0 costs no cycles");

	fake_devices_reset();
	read6502(0x9FA0, 0);
	check_eq(clockticks6502, 3, "and a real read of $9FA0 costs three");

	// Plain RAM is free either way, which keeps the above honest.
	fake_devices_reset();
	read6502(0x0200, 0);
	check_eq(clockticks6502, 0, "a real read of RAM costs nothing");
}

// ---- Sweeping the whole I/O page disturbs nothing ---------------------------
// One test, no per-device maintenance, and it covers devices added later. The
// directed cases above are what stop it passing vacuously.

static void
test_io_page_sweep_is_pure(void)
{
	fake_devices_reset();

	for (uint16_t addr = 0x9F00; addr < 0xA000; addr++) {
		debug_read(addr);
	}

	check(fake_devices_total_reads() > 0, "the sweep reached at least one device");
	check_eq(clockticks6502, 0, "sweeping the I/O page costs no cycles");

	// The YM is the one exception, and it is a signature problem rather than a
	// routing one: YM_read_status() takes no debug flag, so memory.c has no way
	// to tell it this read is for the debugger.
	uint32_t moved = fake_devices_state_sum() - fake_dev[FAKE_YM].state;
	check_eq(moved, 0, "sweeping the I/O page moves no device state");
	check(fake_dev[FAKE_YM].state > 0,
	      "except the YM, whose read has no debug parameter to honour");
}

int
main(void)
{
	test_debug_read_routing();
	test_real_read_is_not_flagged();
	test_debug_read_moves_no_state();
	test_debug_read_costs_nothing();
	test_io_page_sweep_is_pure();
	return x16_test_summary("debugon_contract");
}
