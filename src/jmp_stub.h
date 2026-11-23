// Commander X16 Emulator — following a call through JMP stubs.

#ifndef _JMP_STUB_H_
#define _JMP_STUB_H_

#include <stdint.h>

// A JSR whose target is a JMP stub -- a jump table entry, or the KERNAL's
// jmpfr, through which every jsrfar from every ROM bank reaches its target --
// really calls where the JMP goes. Read at the call, which is exact: the JMP
// executes next, with the same banks and the same pointer. A few hops at most,
// in case stubs chain. `read` must not touch the machine.
static inline uint16_t
jmp_stub_target(uint16_t target, uint8_t (*read)(uint16_t))
{
	for (int hop = 0; hop < 4; hop++) {
		const uint8_t op = read(target);
		if (op == 0x4C) { // JMP abs
			const uint16_t at = (uint16_t)(target + 1);
			target = (uint16_t)(read(at) | (read((uint16_t)(at + 1)) << 8));
		} else if (op == 0x6C) { // JMP (abs); the 65C02 has no page-wrap bug
			const uint16_t at  = (uint16_t)(target + 1);
			const uint16_t ptr = (uint16_t)(read(at) | (read((uint16_t)(at + 1)) << 8));
			target = (uint16_t)(read(ptr) | (read((uint16_t)(ptr + 1)) << 8));
		} else {
			break;
		}
	}
	return target;
}

#endif
