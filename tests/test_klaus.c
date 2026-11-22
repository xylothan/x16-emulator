// Klaus Dormann's 6502/65C02 functional tests, run against the CPU core.
//
// Sequential composition (~30M instructions); reports one trap address per run.
//
// No timing coverage: results only, not cycle counts.
//
// 64K image, start $0400; success is a branch-to-self at one listing address.
//
// Binaries are fetched by tests/fetch_klaus.py; without them this reports skipped.

#include "support/cpu_fixture.h"
#include "support/harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Where a passing run comes to rest, per bin_files/*.lst in Klaus's repo.
#define FUNCTIONAL_SUCCESS 0x3469
#define EXTENDED_SUCCESS   0x24F1

#define START_PC 0x0400

// The images are full 64K dumps.
#define KLAUS_IMAGE_SIZE 0x10000

// The ceiling only stops a broken core spinning forever.
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
	size_t got = fread(cpu_mem, 1, KLAUS_IMAGE_SIZE, f);
	fclose(f);
	if (got != KLAUS_IMAGE_SIZE) {
		printf("     image is %zu bytes, expected %d\n", got, KLAUS_IMAGE_SIZE);
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

// Print the source around the trap address, mapped back through the listing.
static void
print_listing_context(const char *bin_name, uint16_t addr)
{
	char lst_name[128];
	char path[256];

	// 6502_functional_test.bin -> 6502_functional_test.lst
	snprintf(lst_name, sizeof lst_name, "%.*s.lst",
	         (int)(strlen(bin_name) - 4), bin_name);

	if (!find_binary(lst_name, path, sizeof path)) {
		printf("     (%s not fetched; re-run tests/fetch_klaus.py for the source)\n",
		       lst_name);
		return;
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}

	// Keep a window of what came before: the opcode under test is above the line that trapped.
	enum { BEFORE = 14 };
	static char ring[BEFORE][256];
	int    count = 0;
	char   line[256];
	char   want[8];
	snprintf(want, sizeof want, "%04x :", addr);

	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, want, strlen(want)) == 0) {
			printf("     --- %s around $%04X ---\n", lst_name, addr);
			int first = count > BEFORE ? count - BEFORE : 0;
			for (int i = first; i < count; i++) {
				printf("     %s", ring[i % BEFORE]);
			}
			printf("  >> %s", line);
			for (int i = 0; i < 2 && fgets(line, sizeof line, f); i++) {
				printf("     %s", line);
			}
			printf("     --- end ---\n");
			fclose(f);
			return;
		}
		snprintf(ring[count % BEFORE], sizeof ring[0], "%s", line);
		count++;
	}

	fclose(f);
	printf("     ($%04X not found in %s)\n", addr, lst_name);
}

// A missing image is a failure rather than a skip when CI sets this; unset locally.
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
		printf("     A=$%02X X=$%02X Y=$%02X SP=$%04X P=$%02X after %s\n",
		       regs.a, regs.xl, regs.yl, regs.sp, regs.status, name);
		print_listing_context(name, trap);
	}
}

int
main(void)
{
	// Documented opcodes and addressing modes. This is the one that gates.
	run_case("6502_functional_test.bin", FUNCTIONAL_SUCCESS, NULL);

	// The extended test also covers undefined opcodes' byte counts.
	run_case("65C02_extended_opcodes_test.bin", EXTENDED_SUCCESS, NULL);

	return x16_test_summary("klaus");
}
