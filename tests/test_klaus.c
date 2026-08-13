// Klaus Dormann's 6502/65C02 functional tests, run against the CPU core.
//
// Everything else in the CPU tests asserts cases somebody chose, which means it
// can only find mistakes somebody thought of. This runs a program that walks
// every documented opcode and addressing mode and checks its own results,
// millions of instructions at a time. It is the closest thing available to an
// independent opinion.
//
// The mechanics are simple: the .bin is a full 64K image, execution starts at
// $0400, and the program traps by branching to itself. Success is a trap at one
// specific address, taken from the listing that ships with the binary; a trap
// anywhere else means a check failed there, and the listing says which.
//
// The binaries are fetched by tests/fetch_klaus.py rather than committed --
// they are GPL and 64K each. Without them this reports skipped and passes,
// because a missing optional fixture is not a failure.

#include "support/cpu_fixture.h"
#include "support/harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Where a passing run comes to rest, per bin_files/*.lst in Klaus's repo.
#define FUNCTIONAL_SUCCESS 0x3469
#define EXTENDED_SUCCESS   0x24F1

#define START_PC 0x0400

// Generous: a full functional run is tens of millions of instructions, and the
// point of the ceiling is only to stop a broken core spinning forever.
#define MAX_INSTRUCTIONS 200000000u

static const char *
find_binary(const char *name, char *buf, size_t buflen)
{
	// Run from the build directory or the source root, so try both.
	const char *prefixes[] = { "tests/klaus/", "../tests/klaus/", "klaus/" };
	for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
		snprintf(buf, buflen, "%s%s", prefixes[i], name);
		FILE *f = fopen(buf, "rb");
		if (f) {
			fclose(f);
			return buf;
		}
	}
	return NULL;
}

// Run one test image. Returns the address it trapped at, or 0 if it never did.
static uint16_t
run_image(const char *path, bool c816)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return 0;
	}

	cpu_reset_to(c816 ? CPU_816_EMU : CPU_65C02, START_PC);
	size_t got = fread(cpu_mem, 1, sizeof cpu_mem, f);
	fclose(f);
	if (got != sizeof cpu_mem) {
		printf("     image is %zu bytes, expected 65536\n", got);
		return 0;
	}

	// The image includes its own vectors, but execution starts at a fixed
	// address rather than through reset.
	regs.pc = START_PC;

	uint16_t prev_pc = regs.pc;
	for (uint32_t i = 0; i < MAX_INSTRUCTIONS; i++) {
		cpu_steps(1);
		if (regs.pc == prev_pc) {
			return regs.pc; // branched to itself: a trap, pass or fail
		}
		prev_pc = regs.pc;
	}
	return 0;
}

// Whether a missing image is a failure rather than a skip. CI sets this, so a
// broken or deleted fetch step shows up as a red test instead of a green run
// that quietly checked nothing. Locally it is unset, and the images stay
// optional.
static bool
images_required(void)
{
	const char *v = getenv("X16_KLAUS_REQUIRED");
	return v != NULL && v[0] != '\0' && v[0] != '0';
}

static void
run_case(const char *name, uint16_t expected_success, const char *divergence)
{
	char path[256];
	char label[192];

	if (!find_binary(name, path, sizeof path)) {
		if (images_required()) {
			snprintf(label, sizeof label,
			         "%s is present (X16_KLAUS_REQUIRED is set)", name);
			check(false, label);
			printf("     run tests/fetch_klaus.py, or unset X16_KLAUS_REQUIRED\n");
		} else {
			printf("skip: %s not present (run tests/fetch_klaus.py)\n", name);
		}
		return;
	}

	printf("     running %s\n", path);
	uint16_t trap = run_image(path, false);
	bool passed = (trap == expected_success);

	snprintf(label, sizeof label, "%s traps at $%04X, expected $%04X",
	         name, trap, expected_success);

	if (divergence != NULL) {
		check_divergent(passed, label, divergence);
	} else {
		check(passed, label);
	}

	if (!passed) {
		printf("     look up $%04X in bin_files/%.*s.lst to see which check stopped it\n",
		       trap, (int)(strlen(name) - 4), name);
	}
}

int
main(void)
{
	// Documented opcodes and addressing modes. This is the one that gates.
	run_case("6502_functional_test.bin", FUNCTIONAL_SUCCESS, NULL);

	// The extended test also exercises undefined opcodes, and the emulator
	// fills every unused slot with a one-byte NOP rather than modelling the
	// byte and cycle counts real undefined opcodes have. $5C is the first to
	// diverge: a real 65C02 treats it as a three-byte NOP.
	run_case("65C02_extended_opcodes_test.bin", EXTENDED_SUCCESS,
	         "undefined opcodes are one-byte NOPs here; a real 65C02 gives them "
	         "specific sizes and timings. Documented opcodes all pass.");

	return x16_test_summary("klaus");
}
