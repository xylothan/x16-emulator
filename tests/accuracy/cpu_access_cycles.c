// The cycle of each CPU access to a VIA or VERA, counted from the instruction's
// opcode fetch: the offset the device is brought up to before the access.
//
// ORACLE: SingleStepTests/65x02 wdc65c02/v1 and SingleStepTests/65816 v1, the
// per-cycle bus traces in tests/pt/: the index of each access to the
// effective address in the cycle list of the opcode named below.

#include "../support/fake_devices.h"
#include "../support/harness.h"

#include "cpu/fake6502.h"
#include "cpu/registers.h"
#include "memory.h"

#include <stdint.h>

extern struct regs regs;

#define CODE 0x0800
#define ZP   0x10

typedef enum { C02, C816_EMU, C816_NATIVE } cpu_t;

typedef struct {
	const char *name;
	cpu_t       cpu;
	uint8_t     code[3];
	uint8_t     x, y;
	uint16_t    pointer;   // at ZP, or at S + operand when sp is set
	int         n;
	unsigned    offsets[5];
	uint16_t    sp;        // 0: as reset
	uint16_t    dp;
	fake_dev_t  dev;
} access_case;

static const access_case cases[] = {
	{ "65C02 LDA abs",              C02, { 0xAD, 0x04, 0x9F }, 0, 0, 0,      1, { 3 } },
	{ "65C02 STA abs",              C02, { 0x8D, 0x04, 0x9F }, 0, 0, 0,      1, { 3 } },
	{ "65C02 BIT abs",              C02, { 0x2C, 0x04, 0x9F }, 0, 0, 0,      1, { 3 } },
	{ "65C02 INC abs",              C02, { 0xEE, 0x04, 0x9F }, 0, 0, 0,      3, { 3, 4, 5 } },
	{ "65C02 ASL abs",              C02, { 0x0E, 0x04, 0x9F }, 0, 0, 0,      3, { 3, 4, 5 } },
	{ "65C02 TSB abs",              C02, { 0x0C, 0x04, 0x9F }, 0, 0, 0,      3, { 3, 4, 5 } },
	{ "65C02 LDA abs,X",            C02, { 0xBD, 0x00, 0x9F }, 4, 0, 0,      1, { 3 } },
	{ "65C02 LDA abs,X crossing",   C02, { 0xBD, 0xFF, 0x9E }, 5, 0, 0,      1, { 4 } },
	{ "65C02 STA abs,X",            C02, { 0x9D, 0x00, 0x9F }, 4, 0, 0,      1, { 4 } },
	{ "65C02 LDA (zp),Y",           C02, { 0xB1, ZP, 0x00 },   0, 4, 0x9F00, 1, { 4 } },
	{ "65C02 LDA (zp),Y crossing",  C02, { 0xB1, ZP, 0x00 },   0, 0x14, 0x9EF0, 1, { 5 } },
	{ "65C02 STA (zp),Y",           C02, { 0x91, ZP, 0x00 },   0, 4, 0x9F00, 1, { 5 } },
	{ "65C816 E=0 M=0 LDA abs",     C816_NATIVE, { 0xAD, 0x04, 0x9F }, 0, 0, 0, 2, { 3, 4 } },
	{ "65C816 E=0 M=0 STA abs",     C816_NATIVE, { 0x8D, 0x04, 0x9F }, 0, 0, 0, 2, { 3, 4 } },
	{ "65C816 E=0 M=0 INC abs",     C816_NATIVE, { 0xEE, 0x04, 0x9F }, 0, 0, 0, 5, { 3, 4, 5, 6, 7 } },
	// $03: a cycle after the offset.
	{ "65C816 E=0 M=0 LDA sr,S",    C816_NATIVE, { 0xA3, 0x04, 0x00 }, 0, 0, 0, 2, { 3, 4 }, 0x9F00 },
	// $13: a cycle after the offset and another after the pointer.
	{ "65C816 E=0 M=0 LDA (sr,S),Y", C816_NATIVE, { 0xB3, 0x03, 0x00 }, 0, 0, 0x9F04, 2, { 6, 7 }, 0x01F0 },
	// $D4: a cycle after the operand when D is not page aligned.
	{ "65C816 E=0 PEI, D=$9F01",    C816_NATIVE, { 0xD4, 0x03, 0x00 }, 0, 0, 0, 2, { 3, 4 }, 0, 0x9F01 },
	// $60: two internal cycles, the pulls, then a cycle at S.
	{ "65C816 E=0 RTS, S=$9F03",    C816_NATIVE, { 0x60, 0x00, 0x00 }, 0, 0, 0, 3, { 3, 4, 5 }, 0x9F03 },
	// The same cycles, at VERA's DATA0.
	{ "VERA 65C02 LDA abs",              C02, { 0xAD, 0x23, 0x9F }, 0, 0, 0,      1, { 3 }, 0, 0, FAKE_VERA },
	{ "VERA 65C02 STA abs",              C02, { 0x8D, 0x23, 0x9F }, 0, 0, 0,      1, { 3 }, 0, 0, FAKE_VERA },
	{ "VERA 65C02 INC abs",              C02, { 0xEE, 0x23, 0x9F }, 0, 0, 0,      3, { 3, 4, 5 }, 0, 0, FAKE_VERA },
	{ "VERA 65C02 LDA abs,X crossing",   C02, { 0xBD, 0xFF, 0x9E }, 0x24, 0, 0,   1, { 4 }, 0, 0, FAKE_VERA },
	{ "VERA 65C02 STA abs,X",            C02, { 0x9D, 0x20, 0x9F }, 3, 0, 0,      1, { 4 }, 0, 0, FAKE_VERA },
	{ "VERA 65C02 STA (zp),Y",           C02, { 0x91, ZP, 0x00 },   0, 3, 0x9F20, 1, { 5 }, 0, 0, FAKE_VERA },
	{ "VERA 65C816 E=0 M=0 STA abs",     C816_NATIVE, { 0x8D, 0x23, 0x9F }, 0, 0, 0, 2, { 3, 4 }, 0, 0, FAKE_VERA },
	{ "VERA 65C816 E=0 M=0 INC abs",     C816_NATIVE, { 0xEE, 0x23, 0x9F }, 0, 0, 0, 5, { 3, 4, 5, 6, 7 }, 0, 0, FAKE_VERA },
};

static void
run(const access_case *c)
{
	fake_devices_reset();
	memory_reset();
	reset6502(c->cpu != C02);
	if (c->cpu == C816_NATIVE) {
		regs.e = 0;
		regs.status &= (uint8_t) ~(FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH);
	}
	for (int i = 0; i < 3; i++) {
		write6502((uint16_t)(CODE + i), 0, c->code[i]);
	}
	const uint16_t at = c->sp ? (uint16_t)(c->sp + c->code[1]) : ZP;
	if (c->pointer) {
		write6502(at, 0, (uint8_t)c->pointer);
		write6502((uint16_t)(at + 1), 0, (uint8_t)(c->pointer >> 8));
	}
	if (c->sp) {
		regs.sp = c->sp;
	}
	regs.dp = c->dp;
	regs.pc = CODE;
	regs.xl = c->x;
	regs.yl = c->y;
	regs.a  = 0;
	regs.status &= (uint8_t) ~FLAG_DECIMAL;
	clockticks6502 = 1000;
	cpu_step_base  = clockticks6502;
	fake_dev[FAKE_VIA1].catch_ups = 0;
	fake_dev[FAKE_VERA].catch_ups = 0;
	step6502();
	const fake_dev_log_t *d = &fake_dev[c->dev];
	check_eq(d->catch_ups, c->n, c->name);
	for (int i = 0; i < c->n && i < d->catch_ups; i++) {
		check_eq((int)d->offsets[i], (int)c->offsets[i], c->name);
	}
}

// IRQ entry spends its cycles before the next instruction: an access is
// counted from when the devices were last stepped, not from its own opcode.
static void
test_offset_counts_from_the_last_step(void)
{
	fake_devices_reset();
	memory_reset();
	reset6502(false);
	write6502(CODE, 0, 0xAD);
	write6502(CODE + 1, 0, 0x04);
	write6502(CODE + 2, 0, 0x9F);
	regs.pc = CODE;
	clockticks6502 = 1007;
	cpu_step_base  = 1000;
	fake_dev[FAKE_VIA1].catch_ups = 0;
	step6502();
	check_eq((int)fake_dev[FAKE_VIA1].offsets[0], 10, "LDA abs, 7 cycles after the last step");
}

int
main(void)
{
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		run(&cases[i]);
	}
	test_offset_counts_from_the_last_step();
	return x16_test_summary("cpu_access_cycles");
}
