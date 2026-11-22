// Checks for the call stack unwinder (src/call_stack.c).

#include "call_stack.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void
check(bool cond, const char *what)
{
	if (!cond) {
		failures++;
		printf("FAIL: %s\n", what);
	} else {
		printf("ok  : %s\n", what);
	}
}

// Bank 0 and one other, enough to tell which bank each read used.
static uint8_t mem[2][0x10000];

static uint8_t
read_mem(uint16_t address, uint8_t bank)
{
	return mem[bank ? 1 : 0][address];
}

static void
clear(void)
{
	memset(mem, 0, sizeof mem);
}

// Puts a return address on the stack as JSR would, for a JSR at `jsr`.
static void
push_call(uint16_t at, uint16_t jsr, int code_bank)
{
	const uint16_t pushed = (uint16_t)(jsr + 2);
	mem[0][at]                 = (uint8_t)(pushed & 0xFF);
	mem[0][(uint16_t)(at + 1)] = (uint8_t)(pushed >> 8);
	mem[code_bank][jsr]        = 0x20;
}

int
main(void)
{
	uint16_t f[CALL_STACK_MAX];
	int      n;

	clear();
	n = call_stack_unwind(0x1234, 0x01FF, 0, false, read_mem, f);
	check(n == 1 && f[0] == 0x1234, "an empty stack is just the PC");

	clear();
	push_call(0x01FE, 0x0A00, 0);
	push_call(0x01FC, 0x0A10, 0);
	n = call_stack_unwind(0x0A20, 0x01FB, 0, false, read_mem, f);
	check(n == 3, "two calls give three frames");
	check(f[0] == 0x0A20, "the PC comes first");
	check(f[1] == 0x0A13, "then the innermost return address");
	check(f[2] == 0x0A03, "then the outer one");

	clear();
	push_call(0x01FE, 0x0A00, 0);
	mem[0][0x01FD] = 0x55; // a byte pushed by PHA between the two calls
	push_call(0x01FB, 0x0A10, 0);
	n = call_stack_unwind(0x0A20, 0x01FA, 0, false, read_mem, f);
	check(n == 3 && f[1] == 0x0A13 && f[2] == 0x0A03, "a pushed byte between calls is skipped");

	clear();
	push_call(0x01FE, 0x0A00, 0);
	n = call_stack_unwind(0x0A20, 0x00FD, 0, false, read_mem, f);
	check(n == 2 && f[1] == 0x0A03, "emulation mode uses only SP's low byte");

	clear();
	mem[0][0x01FF] = 0x02; // low byte of a pair that would straddle the page
	mem[0][0x0200] = 0x0A;
	mem[0][0x0A00] = 0x20;
	n = call_stack_unwind(0x0A20, 0x01FE, 0, false, read_mem, f);
	check(n == 1, "emulation mode does not read past page 1");

	clear();
	push_call(0x3FFE, 0x0A00, 0);
	n = call_stack_unwind(0x0A20, 0x3FFD, 0, true, read_mem, f);
	check(n == 2 && f[1] == 0x0A03, "native mode scans above a 16-bit SP");

	clear();
	push_call(0x3000 + 256, 0x0A00, 0);
	n = call_stack_unwind(0x0A20, 0x3000, 0, true, read_mem, f);
	check(n == 1, "native mode stops at its window");
	clear();
	push_call(0x3000 + 255, 0x0A00, 0);
	n = call_stack_unwind(0x0A20, 0x3000, 0, true, read_mem, f);
	check(n == 2, "and sees the last pair inside it");

	clear();
	push_call(0x0000, 0x0A00, 0);
	n = call_stack_unwind(0x0A20, 0xFFFF, 0, true, read_mem, f);
	check(n == 2 && f[1] == 0x0A03, "a native SP at the top wraps to address 0");

	clear();
	push_call(0x01FE, 0x0A00, 1);
	n = call_stack_unwind(0x0A20, 0x01FD, 1, false, read_mem, f);
	check(n == 2, "the opcode is read from the program bank");
	n = call_stack_unwind(0x0A20, 0x01FD, 0, false, read_mem, f);
	check(n == 1, "and not from bank 0 when that is not the program bank");

	clear();
	for (uint16_t at = 0x0101; at < 0x01FE; at = (uint16_t)(at + 2))
		push_call(at, 0x0A00, 0);
	n = call_stack_unwind(0x0A20, 0x0100, 0, false, read_mem, f);
	check(n == CALL_STACK_MAX, "a deep stack stops at the limit");
	check(f[0] == 0x0A20 && f[CALL_STACK_MAX - 1] == 0x0A03, "with the PC still first");

	check(call_stack_site(true, 0x0A20) == 0x0A20, "the current frame is at the PC");
	check(call_stack_site(false, 0x0A03) == 0x0A00, "a caller is at its JSR");
	check(call_stack_site(false, 0x0001) == 0xFFFE, "a call site wraps with the address space");

	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
