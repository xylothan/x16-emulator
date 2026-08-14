// Conformance against SingleStepTests/ProcessorTests: every opcode, thousands
// of randomised starting states each.
//
// Three claims are checked per case, counted separately because they fail
// independently:
//
//   1. final register and memory state;
//   2. total cycle count;
//   3. the bus trace -- which addresses were touched, in what order, including
//      the dummy accesses real hardware makes.
//
// The core cannot pass (3) as written: it adds an instruction's cycles in one
// go rather than one bus access per cycle, and skips dummy accesses. All three
// are therefore measured against a recorded baseline rather than asserted
// against hardware.
//
// Fixtures come from tests/fetch_pt.py and are not committed. Without them
// this reports skipped.

#include "support/cpu_fixture.h"
#include "support/harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RAM    64
#define MAX_CYCLES 32

typedef struct {
	uint16_t pc;
	uint8_t  s, a, x, y, p;
} pt_state;

typedef struct {
	uint16_t addr;
	uint8_t  value;
	uint8_t  is_write;
} pt_cycle;

typedef struct {
	uint8_t  opcode;
	pt_state initial, final_;
	uint16_t init_ram_n, final_ram_n, cycle_n;
	struct { uint16_t addr; uint8_t value; } init_ram[MAX_RAM], final_ram[MAX_RAM];
	pt_cycle cycles[MAX_CYCLES];
} pt_case;

// ---- Bus log, filled by the fixture's hooks -------------------------------

#define BUS_MAX 64
static pt_cycle bus[BUS_MAX];
static int      bus_n;

static void
note_read(uint32_t addr, uint8_t value)
{
	if (bus_n < BUS_MAX) {
		bus[bus_n].addr = (uint16_t)addr;
		bus[bus_n].value = value;
		bus[bus_n].is_write = 0;
		bus_n++;
	}
}

static void
note_write(uint32_t addr, uint8_t value)
{
	if (bus_n < BUS_MAX) {
		bus[bus_n].addr = (uint16_t)addr;
		bus[bus_n].value = value;
		bus[bus_n].is_write = 1;
		bus_n++;
	}
}

static bool
read_exact(FILE *f, void *dst, size_t n)
{
	return fread(dst, 1, n, f) == n;
}

static bool
read_state(FILE *f, pt_state *s)
{
	return read_exact(f, &s->pc, 2) && read_exact(f, &s->s, 1) &&
	       read_exact(f, &s->a, 1) && read_exact(f, &s->x, 1) &&
	       read_exact(f, &s->y, 1) && read_exact(f, &s->p, 1);
}

static bool
read_case(FILE *f, pt_case *c)
{
	if (!read_exact(f, &c->opcode, 1)) return false;
	if (!read_state(f, &c->initial)) return false;
	if (!read_exact(f, &c->init_ram_n, 2) || c->init_ram_n > MAX_RAM) return false;
	for (int i = 0; i < c->init_ram_n; i++) {
		if (!read_exact(f, &c->init_ram[i].addr, 2)) return false;
		if (!read_exact(f, &c->init_ram[i].value, 1)) return false;
	}
	if (!read_state(f, &c->final_)) return false;
	if (!read_exact(f, &c->final_ram_n, 2) || c->final_ram_n > MAX_RAM) return false;
	for (int i = 0; i < c->final_ram_n; i++) {
		if (!read_exact(f, &c->final_ram[i].addr, 2)) return false;
		if (!read_exact(f, &c->final_ram[i].value, 1)) return false;
	}
	if (!read_exact(f, &c->cycle_n, 2) || c->cycle_n > MAX_CYCLES) return false;
	for (int i = 0; i < c->cycle_n; i++) {
		if (!read_exact(f, &c->cycles[i].addr, 2)) return false;
		if (!read_exact(f, &c->cycles[i].value, 1)) return false;
		if (!read_exact(f, &c->cycles[i].is_write, 1)) return false;
	}
	return true;
}

// ---- Running one case ------------------------------------------------------

typedef struct {
	long total, state_ok, cycles_ok, bus_ok;
	long state_ok_ignoring_b;  // matches but for bit 4 of P
	uint8_t first_bad_opcode;
	bool    have_first_bad;
	char    first_bad[256];
} pt_result;

// Per opcode, so a report names what to fix rather than just how much is wrong.
typedef struct {
	long total, state_ok, state_ok_ignoring_b, cycles_ok, bus_ok;
	char sample[192];   // first state mismatch seen for this opcode
	bool have_sample;
} pt_opcode;

static pt_opcode by_opcode[256];

static void
run_case(const pt_case *c, pt_result *r)
{
	cpu_reset_to(CPU_65C02, 0x0000);
	for (int i = 0; i < c->init_ram_n; i++) {
		cpu_mem[c->init_ram[i].addr] = c->init_ram[i].value;
	}
	regs.pc     = c->initial.pc;
	regs.sp     = 0x0100 | c->initial.s;
	regs.a      = c->initial.a;
	regs.xl     = c->initial.x;
	regs.yl     = c->initial.y;
	regs.status = c->initial.p;

	bus_n = 0;
	cpu_bus_read_hook = note_read;
	cpu_bus_write_hook = note_write;
	uint32_t spent = cpu_step();
	cpu_bus_read_hook = NULL;
	cpu_bus_write_hook = NULL;

	r->total++;
	pt_opcode *op = &by_opcode[c->opcode];
	op->total++;

	bool regs_but_p = regs.pc == c->final_.pc &&
	                  (regs.sp & 0xFF) == c->final_.s &&
	                  regs.a == c->final_.a &&
	                  regs.xl == c->final_.x &&
	                  regs.yl == c->final_.y;
	bool ram_ok = true;
	for (int i = 0; i < c->final_ram_n; i++) {
		if (cpu_mem[c->final_ram[i].addr] != c->final_ram[i].value) {
			ram_ok = false;
		}
	}

	bool state = regs_but_p && ram_ok && regs.status == c->final_.p;
	// Bit 4 has no physical existence: it only means anything in a copy of P
	// pushed to the stack. Counting matches that ignore it separates one
	// systematic disagreement from genuinely wrong results.
	bool state_but_b = regs_but_p && ram_ok &&
	                   (regs.status & ~0x10) == (c->final_.p & ~0x10);

	if (state) {
		r->state_ok++;
		op->state_ok++;
	}
	if (state_but_b) {
		r->state_ok_ignoring_b++;
		op->state_ok_ignoring_b++;
	}
	if (!state_but_b && !op->have_sample) {
		op->have_sample = true;
		snprintf(op->sample, sizeof op->sample,
		         "PC $%04X/$%04X A $%02X/$%02X X $%02X/$%02X "
		         "Y $%02X/$%02X P $%02X/$%02X SP $%02X/$%02X (got/want)",
		         regs.pc, c->final_.pc, regs.a, c->final_.a,
		         regs.xl, c->final_.x, regs.yl, c->final_.y,
		         regs.status, c->final_.p, regs.sp & 0xFF, c->final_.s);
	}
	if (!state && !r->have_first_bad) {
		r->have_first_bad = true;
		r->first_bad_opcode = c->opcode;
		snprintf(r->first_bad, sizeof r->first_bad,
		         "opcode $%02X: PC $%04X/$%04X A $%02X/$%02X X $%02X/$%02X "
		         "Y $%02X/$%02X P $%02X/$%02X SP $%02X/$%02X (got/want)",
		         c->opcode, regs.pc, c->final_.pc, regs.a, c->final_.a,
		         regs.xl, c->final_.x, regs.yl, c->final_.y,
		         regs.status, c->final_.p, regs.sp & 0xFF, c->final_.s);
	}

	if (spent == c->cycle_n) {
		r->cycles_ok++;
		op->cycles_ok++;
	}

	bool trace = (bus_n == c->cycle_n);
	for (int i = 0; trace && i < bus_n; i++) {
		if (bus[i].addr != c->cycles[i].addr ||
		    bus[i].is_write != c->cycles[i].is_write) {
			trace = false;
		}
	}
	if (trace) {
		r->bus_ok++;
		op->bus_ok++;
	}
}

// ctest may run this from the source root or the build directory. Resolve once
// rather than giving every file its own candidate list.
static const char *
tests_prefix(void)
{
	static const char *prefix = NULL;
	if (prefix) {
		return prefix;
	}
	const char *candidates[] = { "", "../", "../../" };
	for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
		char probe[256];
		snprintf(probe, sizeof probe, "%stests/pt_baseline_wdc65c02.txt",
		         candidates[i]);
		FILE *f = fopen(probe, "rb");
		if (f) {
			fclose(f);
			prefix = candidates[i];
			return prefix;
		}
	}
	prefix = "";
	return prefix;
}

static const char *
tests_path(char *buf, size_t buflen, const char *rel)
{
	snprintf(buf, buflen, "%stests/%s", tests_prefix(), rel);
	return buf;
}

static const char *
find_fixture(char *buf, size_t buflen)
{
	tests_path(buf, buflen, "pt/wdc65c02.bin");
	FILE *f = fopen(buf, "rb");
	if (f) {
		fclose(f);
		return buf;
	}
	return NULL;
}

// Baseline: cases passing each check, per opcode. Committed; the fixtures are
// not. A diff to this file is the record of the core's conformance changing.
//
// Compared in both directions. A drop is a regression; a rise fails too, since
// a baseline is only useful while it stays true. Both are resolved by
// regenerating it and reading the diff.
//
// Fixes belong in src/cpu and touch nothing here, so they can go upstream.
#define BASELINE_PATH "tests/pt_baseline_wdc65c02.txt"

static const char *
find_baseline(char *buf, size_t buflen)
{
	tests_path(buf, buflen, "pt_baseline_wdc65c02.txt");
	FILE *f = fopen(buf, "rb");
	if (f) {
		fclose(f);
		return buf;
	}
	return NULL;
}

static void
write_baseline(const char *path, long per_opcode)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		printf("     could not write %s\n", path);
		return;
	}
	fprintf(f, "# ProcessorTests results for wdc65c02: cases matching hardware,\n");
	fprintf(f, "# per opcode. Anything short of the case count is a divergence.\n");
	fprintf(f, "# state-b ignores bit 4 of P, which the core forces on.\n");
	fprintf(f, "# Regenerate with X16_PT_WRITE_BASELINE=1 and commit the diff.\n");
	fprintf(f, "cases_per_opcode %ld\n", per_opcode);
	fprintf(f, "# op state state-b cycles bus\n");
	for (int i = 0; i < 256; i++) {
		pt_opcode *op = &by_opcode[i];
		if (op->total > 0) {
			fprintf(f, "%02X %ld %ld %ld %ld\n", i, op->state_ok,
			        op->state_ok_ignoring_b, op->cycles_ok, op->bus_ok);
		}
	}
	fclose(f);
	printf("     wrote %s\n", path);
}

// Returns the number of opcodes disagreeing with the baseline, or -1 if the
// baseline was built against a different sample size and cannot be compared.
static int
compare_baseline(const char *path, long per_opcode)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return 0;
	}
	long want[256][4];
	bool seen[256];
	long baseline_per_opcode = -1;
	char line[160];
	for (int i = 0; i < 256; i++) {
		seen[i] = false;
		for (int j = 0; j < 4; j++) {
			want[i][j] = 0;
		}
	}
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#' || line[0] == '\n') {
			continue;
		}
		long v;
		if (sscanf(line, "cases_per_opcode %ld", &v) == 1) {
			baseline_per_opcode = v;
			continue;
		}
		unsigned op;
		long a, b, cc, d;
		if (sscanf(line, "%x %ld %ld %ld %ld", &op, &a, &b, &cc, &d) == 5 &&
		    op < 256) {
			want[op][0] = a;
			want[op][1] = b;
			want[op][2] = cc;
			want[op][3] = d;
			seen[op] = true;
		}
	}
	fclose(f);

	if (baseline_per_opcode != per_opcode) {
		printf("     baseline covers %ld cases per opcode, this run has %ld\n",
		       baseline_per_opcode, per_opcode);
		return -1;
	}

	static const char *label[4] = { "state", "state-b", "cycles", "bus" };
	int bad = 0;
	for (int i = 0; i < 256; i++) {
		pt_opcode *op = &by_opcode[i];
		if (op->total == 0) {
			continue;
		}
		if (!seen[i]) {
			printf("     opcode $%02X: not in the baseline\n", i);
			bad++;
			continue;
		}
		long got[4] = { op->state_ok, op->state_ok_ignoring_b, op->cycles_ok,
			            op->bus_ok };
		for (int j = 0; j < 4; j++) {
			if (got[j] != want[i][j]) {
				printf("     opcode $%02X %s: %ld/%ld, baseline says %ld (%s)\n",
				       i, label[j], got[j], op->total, want[i][j],
				       got[j] < want[i][j] ? "REGRESSION"
				                           : "improved, record it");
				bad++;
			}
		}
	}
	return bad;
}

static void
write_report(long per_opcode)
{
	char rpath[256];
	tests_path(rpath, sizeof rpath, "../pt-report.txt");
	FILE *f = fopen(rpath, "wb");
	if (!f) {
		return;
	}
	fprintf(f, "# ProcessorTests per-opcode results, %ld cases per opcode.\n", per_opcode);
	fprintf(f, "# state-b ignores bit 4 of P, which the core forces on.\n");
	fprintf(f, "opcode  state       state-b     cycles      bus         first mismatch (ignoring P bit 4)\n");
	for (int i = 0; i < 256; i++) {
		pt_opcode *op = &by_opcode[i];
		if (op->total == 0) {
			continue;
		}
		fprintf(f, "$%02X     %5ld/%-5ld %5ld/%-5ld %5ld/%-5ld %5ld/%-5ld %s\n",
		        i, op->state_ok, op->total, op->state_ok_ignoring_b, op->total,
		        op->cycles_ok, op->total, op->bus_ok, op->total,
		        op->have_sample ? op->sample : "");
	}
	fclose(f);
	printf("     wrote %s (%ld cases per opcode)\n", rpath, per_opcode);
}

int
main(void)
{
	char path[256];
	if (!find_fixture(path, sizeof path)) {
		printf("skip: no ProcessorTests fixture (see tests/fetch_pt.py)\n");
		return x16_test_summary("processor_tests");
	}

	FILE *f = fopen(path, "rb");
	char magic[4];
	uint32_t version, count;
	if (!read_exact(f, magic, 4) || memcmp(magic, "X16P", 4) != 0 ||
	    !read_exact(f, &version, 4) || !read_exact(f, &count, 4)) {
		printf("FAIL: %s is not a fixture file\n", path);
		fclose(f);
		return 1;
	}

	printf("     %s: %u cases\n", path, count);

	pt_result r;
	memset(&r, 0, sizeof r);
	pt_case c;
	while (read_case(f, &c)) {
		run_case(&c, &r);
	}
	fclose(f);

	// The baseline holds per-opcode counts, so it only compares against a run
	// that converted the same number for each.
	long per_opcode = -1;
	int  opcodes_seen = 0;
	bool even = true;
	for (int i = 0; i < 256; i++) {
		if (by_opcode[i].total == 0) {
			continue;
		}
		opcodes_seen++;
		if (per_opcode < 0) {
			per_opcode = by_opcode[i].total;
		} else if (by_opcode[i].total != per_opcode) {
			even = false;
		}
	}

	printf("     %d opcodes, %ld cases each\n", opcodes_seen, per_opcode);
	printf("     final state : %ld/%ld\n", r.state_ok, r.total);
	printf("     ...ignoring P bit 4 : %ld/%ld\n", r.state_ok_ignoring_b, r.total);
	printf("     cycle count : %ld/%ld\n", r.cycles_ok, r.total);
	printf("     bus trace   : %ld/%ld\n", r.bus_ok, r.total);
	if (r.have_first_bad) {
		printf("     first state mismatch: %s\n", r.first_bad);
	}
	write_report(per_opcode);

	// A run matching its baseline is still wrong; say so, or green reads as
	// correct.
	int diverging = 0;
	for (int i = 0; i < 256; i++) {
		pt_opcode *op = &by_opcode[i];
		if (op->total > 0 && (op->state_ok_ignoring_b < op->total ||
		                      op->cycles_ok < op->total)) {
			diverging++;
		}
	}
	if (diverging > 0) {
		printf("DIVERGE: %d of %d opcodes do not match hardware on state or "
		       "cycles -- see pt-report.txt\n", diverging, opcodes_seen);
	}

	check(r.total > 0, "the fixture contained cases");

	char bpath[256];
	if (getenv("X16_PT_WRITE_BASELINE")) {
		write_baseline(find_baseline(bpath, sizeof bpath) ? bpath : BASELINE_PATH,
		               per_opcode);
	} else if (!even) {
		printf("     uneven case counts per opcode; skipping the baseline check\n");
	} else if (!find_baseline(bpath, sizeof bpath)) {
		printf("     no baseline yet; run with X16_PT_WRITE_BASELINE=1\n");
	} else {
		int bad = compare_baseline(bpath, per_opcode);
		check(bad >= 0, "the baseline was built for this many cases per opcode");
		check(bad <= 0, "every opcode matches its recorded baseline");
	}

	return x16_test_summary("processor_tests");
}
