// Commander X16 Emulator — call stack unwinding. See call_stack.h.

#include "call_stack.h"

#define OPCODE_JSR 0x20

int call_stack_unwind(uint16_t pc, uint16_t sp, uint8_t program_bank, bool native,
                      call_stack_read_t read, uint16_t *frames)
{
	uint16_t slots[CALL_STACK_MAX];
	return call_stack_unwind_slots(pc, sp, program_bank, native, read, frames, slots);
}

int call_stack_unwind_slots(uint16_t pc, uint16_t sp, uint8_t program_bank, bool native,
                            call_stack_read_t read, uint16_t *frames, uint16_t *slots)
{
	int count = 0;
	slots[count]    = 0;
	frames[count++] = pc;

	// Emulation mode: the rest of page 1. Native: a bounded window above SP.
	const uint32_t start = native ? (uint32_t)sp + 1 : 0x100u + (sp & 0xFFu) + 1;
	const uint32_t end   = native ? (uint32_t)sp + 1 + 256 : 0x200u;

	for (uint32_t a = start; a + 1 < end && count < CALL_STACK_MAX;) {
		const uint8_t  lo     = read((uint16_t)a, 0);
		const uint8_t  hi     = read((uint16_t)(a + 1), 0);
		const uint16_t pushed = (uint16_t)(lo | (hi << 8));
		// JSR pushes the address of its own last byte, so the opcode is two
		// below that and execution resumes one above it.
		if (read((uint16_t)(pushed - 2), program_bank) == OPCODE_JSR) {
			slots[count]    = (uint16_t)a;
			frames[count++] = (uint16_t)(pushed + 1);
			a += 2;
		} else {
			a += 1;
		}
	}
	return count;
}
