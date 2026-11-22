// RAM and ROM banking: the registers at $0000 and $0001, and the two windows
// they steer.
//
// X16 banking is discrete logic with no published source.
//   ORACLE: C -- X16Community/x16-docs, "X16 Reference - 08 - Memory Map.md",
//   pinned at 79a8303bc33cda4ec3e85f3239f9b3d4c8c76260, for the bank
//   registers, the window sizes and the bank counts. Quoted inline. Fetch it
//   with `python tests/refs/fetch_refs.py`.
//
// ORACLE: none for an uninstalled bank: those assertions characterize this emulator.
//
// The fixture is a 512 KB machine (num_ram_banks is 64).

#include "support/fake_devices.h"
#include "support/harness.h"

#include "memory.h"

#include <stdint.h>

#define RAM_BANK_REG 0x0000
#define ROM_BANK_REG 0x0001

#define BANKED_RAM_START 0xA000
#define BANKED_RAM_END   0xBFFF
#define BANKED_ROM_START 0xC000

// Matches num_ram_banks in fake_devices.c.
#define INSTALLED_RAM_BANKS 64

static void
reset(void)
{
	fake_devices_reset();
	memory_reset();
}

static uint8_t
read(uint16_t address)
{
	return read6502(address, 0);
}

static void
write(uint16_t address, uint8_t value)
{
	write6502(address, 0, value);
}

// ─── Conformance: the documented shape ──────────────────────────────────────

// Memory Map, the banking register table:
//     |$0000    |Current RAM bank (0-255)                                      |
//     |$0001    |Current ROM/Cartridge bank (ROM is 0-31, Cartridge is 32-255) |
//
// and directly beneath it:
//     "The currently set banks can also be read back from the respective
//      memory locations. Both settings default to 0 on RESET."
static void
test_the_bank_registers_hold_and_report_a_bank(void)
{
	reset();
	check_eq(read(RAM_BANK_REG), 0, "the RAM bank defaults to 0 on reset");
	check_eq(read(ROM_BANK_REG), 0, "and so does the ROM bank");

	write(RAM_BANK_REG, 0x2A);
	check_eq(read(RAM_BANK_REG), 0x2A, "the RAM bank reads back what was written");
	check_eq(memory_get_ram_bank(), 0x2A, "and the machine is switched to it");

	write(ROM_BANK_REG, 0x1F);
	check_eq(read(ROM_BANK_REG), 0x1F, "the ROM bank reads back what was written");
	check_eq(memory_get_rom_bank(), 0x1F, "and the machine is switched to it");

	check_eq(read(RAM_BANK_REG), 0x2A, "with the two registers independent");

	// "0-255": the whole byte is the bank number, with nothing reserved.
	write(RAM_BANK_REG, 0xFF);
	check_eq(read(RAM_BANK_REG), 0xFF, "the RAM bank spans the whole byte");
	check_eq(memory_get_ram_bank(), 0xFF, "and reaches the latch unmasked");
	write(ROM_BANK_REG, 0xFF);
	check_eq(read(ROM_BANK_REG), 0xFF, "and so does the ROM bank");
	check_eq(memory_get_rom_bank(), 0xFF, "also unmasked");
}

// The other half of the same sentence: "Both settings default to 0 on RESET."
// memory.c:325 writes the byte to RAM and the latch; memory.c:220 reads it back.
static void
test_a_reset_returns_both_banks_to_zero(void)
{
	reset();
	write(RAM_BANK_REG, 0xFF);
	write(ROM_BANK_REG, 0xFF);

	reset();

	check_eq(memory_get_ram_bank(), 0, "a reset switches the machine to RAM bank 0");
	check_eq(memory_get_rom_bank(), 0, "and to ROM bank 0");

	check_divergent(read(RAM_BANK_REG) == 0,
	                "$0000 reports bank 0 after a reset",
	                "memory.c:110 clears the latch but the address reads RAM[0] "
	                "(memory.c:220), which the reset leaves alone, so the byte reports the "
	                "pre-reset bank while the machine is on bank 0");
	check_divergent(read(ROM_BANK_REG) == 0,
	                "$0001 reports bank 0 after a reset",
	                "same cause as $0000: the register's RAM shadow survives the reset");
}

// Memory Map, the address table:
//     |\$A000-\$BFFF|Banked RAM (8 KB window into one of 256 banks for a total of 2 MB)|
//
// One window, many banks: the same address in two banks is two different
// bytes, and switching back finds the first one again.
static void
test_the_ram_window_shows_one_bank_at_a_time(void)
{
	reset();

	write(RAM_BANK_REG, 0);
	write(BANKED_RAM_START, 0x11);
	write(BANKED_RAM_END, 0x22);

	write(RAM_BANK_REG, 1);
	write(BANKED_RAM_START, 0x33);
	write(BANKED_RAM_END, 0x44);

	check_eq(read(BANKED_RAM_START), 0x33, "bank 1 holds its own byte at the window start");
	check_eq(read(BANKED_RAM_END), 0x44, "and at the window end");

	write(RAM_BANK_REG, 0);
	check_eq(read(BANKED_RAM_START), 0x11, "and bank 0 still holds what it was given");
	check_eq(read(BANKED_RAM_END), 0x22, "at both ends of the window");

	// 8 KB: the byte below is fixed RAM (Memory Map: "$0000-$9EFF").
	write(0x9EFF, 0x55);
	write(RAM_BANK_REG, 1);
	check_eq(read(0x9EFF), 0x55, "the last byte of fixed RAM is not banked");
	check_eq(BANKED_RAM_END - BANKED_RAM_START + 1, 8192, "and the window is 8 KB");
}

// Memory Map:
//     |\$C000-\$FFFF|Banked System ROM and Cartridge ROM/RAM (16 KB window into one of 256 banks, see below)|
//     |$0001    |Current ROM/Cartridge bank (ROM is 0-31, Cartridge is 32-255) |
static void
test_the_rom_window_is_sixteen_kilobytes(void)
{
	reset();
	check_eq(0xFFFF - BANKED_ROM_START + 1, 16384, "the ROM window is 16 KB");

	// Banks 0-31 are the system ROM. With no cartridge attached the emulator
	// leaves 32-255 unmapped, which is the characterization group below.
	write(ROM_BANK_REG, 31);
	check_eq(memory_get_rom_bank(), 31, "the last system ROM bank can be selected");
}

// ─── Characterization: undocumented, pinned so a change is deliberate ───────
// ORACLE: none. The Memory Map says only:
//   "(On systems with only 512 KB RAM, banks 64-255 are unavailable.)"
// memory.c:262 returns the high byte of the address as an open bus read.

static void
test_an_uninstalled_ram_bank_reads_as_the_address_high_byte(void)
{
	reset();
	check_eq(INSTALLED_RAM_BANKS, 64, "the fixture is a 512 KB machine");

	write(RAM_BANK_REG, INSTALLED_RAM_BANKS - 1);
	write(BANKED_RAM_START, 0x77);
	check_eq(read(BANKED_RAM_START), 0x77, "the last installed bank is real memory");

	write(RAM_BANK_REG, INSTALLED_RAM_BANKS);
	check_eq(read(BANKED_RAM_START), 0xA0, "the first uninstalled bank reads the address high byte");
	check_eq(read(BANKED_RAM_END), 0xBF, "which tracks the address across the window");

	write(RAM_BANK_REG, 0xFF);
	check_eq(read(BANKED_RAM_START), 0xA0, "and the highest bank reads the same");
}

static void
test_a_write_to_an_uninstalled_ram_bank_is_dropped(void)
{
	reset();

	write(RAM_BANK_REG, 0);
	write(BANKED_RAM_START, 0x5A);

	write(RAM_BANK_REG, INSTALLED_RAM_BANKS);
	write(BANKED_RAM_START, 0xA5);

	write(RAM_BANK_REG, 0);
	check_eq(read(BANKED_RAM_START), 0x5A, "a write to an uninstalled bank reaches no memory");
}

// memory.c:359-362 forwards writes to banks 32+ to the cartridge, ignores the rest.
static void
test_writes_to_system_rom_are_ignored(void)
{
	reset();
	write(ROM_BANK_REG, 0);

	const uint8_t before = read(BANKED_ROM_START);
	write(BANKED_ROM_START, (uint8_t)(before ^ 0xFF));
	check_eq(read(BANKED_ROM_START), before, "a write to system ROM changes nothing");
}

static void
test_a_rom_bank_above_the_system_rom_reads_as_open_bus(void)
{
	reset();

	// No cartridge is attached: fake_devices.c leaves CART null.
	write(ROM_BANK_REG, 32);
	check_eq(read(BANKED_ROM_START), 0xC0, "the first cartridge bank reads the address high byte");
	check_eq(read(0xFFFF), 0xFF, "which tracks the address across the window");
}

int
main(void)
{
	test_the_bank_registers_hold_and_report_a_bank();
	test_a_reset_returns_both_banks_to_zero();
	test_the_ram_window_shows_one_bank_at_a_time();
	test_the_rom_window_is_sixteen_kilobytes();

	test_an_uninstalled_ram_bank_reads_as_the_address_high_byte();
	test_a_write_to_an_uninstalled_ram_bank_is_dropped();
	test_writes_to_system_rom_are_ignored();
	test_a_rom_bank_above_the_system_rom_reads_as_open_bus();

	return x16_test_summary("memory_banking");
}
