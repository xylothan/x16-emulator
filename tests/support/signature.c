#include "signature.h"
#include "fake_devices.h"

#include "memory.h"
#include "cpu/registers.h"

#include <stdint.h>

extern struct regs regs;
extern uint32_t clockticks6502;
extern uint8_t *RAM;

// FNV-1a. Chosen because it is short enough to read and verify at a glance,
// which matters more here than distribution: this is change detection, not
// cryptography.
static void
mix(uint64_t *h, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		*h ^= p[i];
		*h *= 0x100000001b3ULL;
	}
}

machine_sig_t
machine_signature(void)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	// CPU: registers and what they have cost so far.
	mix(&h, &regs, sizeof regs);
	mix(&h, &clockticks6502, sizeof clockticks6502);

	// Memory: the low 64K, which is everything these fixtures touch.
	if (RAM != NULL) {
		mix(&h, RAM, 0x10000);
	}

	// Which banks are mapped, since a read through a window depends on it.
	uint8_t ram_bank = memory_get_ram_bank();
	uint8_t rom_bank = memory_get_rom_bank();
	mix(&h, &ram_bank, sizeof ram_bank);
	mix(&h, &rom_bank, sizeof rom_bank);

	// Devices: how often each was touched, and whether it moved.
	mix(&h, fake_dev, sizeof(fake_dev_log_t) * FAKE_DEV_COUNT);

	machine_sig_t sig = { h };
	return sig;
}

int
machine_sig_equal(machine_sig_t a, machine_sig_t b)
{
	return a.value == b.value;
}
