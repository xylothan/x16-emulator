// CPU dummy bus cycles as seen by a device on the I/O page.
//
// ORACLE: SingleStepTests/65x02 wdc65c02/v1 and SingleStepTests/65816 v1, the
// per-cycle bus traces in tests/pt/, for each opcode named below.
// ORACLE: Commander X16 Developer Edition schematic Rev 1.1: U30 (74ACT138)
// selects I/O from A5-A15 alone; CPU pin 39 (65C816 VDA) is not connected.

#include "../support/fake_devices.h"
#include "../support/harness.h"

#include "cpu/fake6502.h"
#include "cpu/registers.h"
#include "memory.h"

#include <stdint.h>

extern struct regs regs;

#define CODE 0x0800

typedef enum { C02, C816_EMU, C816_NATIVE } cpu_t;

typedef struct {
	const char *name;
	cpu_t       cpu;
	uint8_t     code[3];
	uint8_t     x;
	bool        decimal;
	int         reads;
	int         writes;
} bus_case;

static const bus_case cases[] = {
	// $EE, $FE, $0E, $0C: read, read, write at the operand.
	{ "65C02 INC abs",            C02, { 0xEE, 0x23, 0x9F }, 0x00, false, 2, 1 },
	{ "65C02 INC abs,X",          C02, { 0xFE, 0x22, 0x9F }, 0x01, false, 2, 1 },
	{ "65C02 ASL abs",            C02, { 0x0E, 0x23, 0x9F }, 0x00, false, 2, 1 },
	{ "65C02 TSB abs",            C02, { 0x0C, 0x23, 0x9F }, 0x00, false, 2, 1 },
	// $AD, $8D: one access.
	{ "65C02 LDA abs",            C02, { 0xAD, 0x23, 0x9F }, 0x00, false, 1, 0 },
	{ "65C02 STA abs",            C02, { 0x8D, 0x23, 0x9F }, 0x00, false, 0, 1 },
	// $BD, $9D: the indexing cycle re-reads the last operand byte.
	{ "65C02 LDA abs,X crossing", C02, { 0xBD, 0xFF, 0x9E }, 0x24, false, 1, 0 },
	{ "65C02 STA abs,X crossing", C02, { 0x9D, 0xFF, 0x9E }, 0x24, false, 0, 1 },
	// $6D, $ED: D=1 reads the operand again.
	{ "65C02 ADC abs, D=0",       C02, { 0x6D, 0x23, 0x9F }, 0x00, false, 1, 0 },
	{ "65C02 ADC abs, D=1",       C02, { 0x6D, 0x23, 0x9F }, 0x00, true,  2, 0 },
	{ "65C02 SBC abs, D=1",       C02, { 0xED, 0x23, 0x9F }, 0x00, true,  2, 0 },
	// $6C: pointer low, pointer high, pointer high.
	{ "65C02 JMP (abs)",          C02, { 0x6C, 0x23, 0x9F }, 0x00, false, 3, 0 },
	// $EE: E=1 writes the old value in the modify cycle, E=0 reads.
	{ "65C816 E=1 INC abs",       C816_EMU,    { 0xEE, 0x23, 0x9F }, 0x00, false, 1, 2 },
	{ "65C816 E=0 INC abs",       C816_NATIVE, { 0xEE, 0x23, 0x9F }, 0x00, false, 2, 1 },
	// $BD: the indexing cycle reads the uncarried address $9F23; the read is at $A023.
	{ "65C816 LDA abs,X crossing", C816_EMU,   { 0xBD, 0x80, 0x9F }, 0xA3, false, 1, 0 },
};

static void
run(const bus_case *c)
{
	fake_devices_reset();
	memory_reset();
	reset6502(c->cpu != C02);
	if (c->cpu == C816_NATIVE) {
		regs.e = 0;
		regs.status |= FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
	}
	for (int i = 0; i < 3; i++) {
		write6502((uint16_t)(CODE + i), 0, c->code[i]);
	}
	regs.pc = CODE;
	regs.xl = c->x;
	regs.a  = 0;
	regs.status &= (uint8_t) ~FLAG_DECIMAL;
	if (c->decimal) {
		regs.status |= FLAG_DECIMAL;
	}
	step6502();
	check_eq(fake_dev[FAKE_VERA].reads, c->reads, c->name);
	check_eq(fake_dev[FAKE_VERA].writes, c->writes, c->name);
}

int
main(void)
{
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		run(&cases[i]);
	}
	return x16_test_summary("cpu_dummy_cycles");
}
