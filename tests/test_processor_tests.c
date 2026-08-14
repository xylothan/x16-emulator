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
// MVP and MVN move a block, and the suite records the whole move as one
// instruction: 100 cycles in the 65C816 emulation-mode set.
#define MAX_CYCLES 160

// Cycle flags, as written by pt_convert.py.
#define PTF_VDA   0x01
#define PTF_VPA   0x02
#define PTF_VPB   0x04
#define PTF_WRITE 0x08

#define PT_CPU_65C02 0
#define PT_CPU_65816 1

typedef struct {
	uint16_t pc, s, a, x, y, d;
	uint8_t  p, e, dbr, pbr;
} pt_state;

typedef struct {
	uint32_t addr;
	uint8_t  value;
	uint8_t  flags;
} pt_cycle;

typedef struct {
	uint8_t  opcode, cpu;
	pt_state initial, final_;
	uint16_t init_ram_n, final_ram_n, cycle_n;
	struct { uint32_t addr; uint8_t value; } init_ram[MAX_RAM], final_ram[MAX_RAM];
	pt_cycle cycles[MAX_CYCLES];
} pt_case;

// ---- Bus log, filled by the fixture's hooks -------------------------------

#define BUS_MAX 160
static pt_cycle bus[BUS_MAX];
static int      bus_n;

static void
note_read(uint32_t addr, uint8_t value)
{
	if (bus_n < BUS_MAX) {
		bus[bus_n].addr = addr;
		bus[bus_n].value = value;
		bus[bus_n].flags = 0;
		bus_n++;
	}
}

static void
note_write(uint32_t addr, uint8_t value)
{
	if (bus_n < BUS_MAX) {
		bus[bus_n].addr = addr;
		bus[bus_n].value = value;
		bus[bus_n].flags = PTF_WRITE;
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
	return read_exact(f, &s->pc, 2) && read_exact(f, &s->s, 2) &&
	       read_exact(f, &s->a, 2) && read_exact(f, &s->x, 2) &&
	       read_exact(f, &s->y, 2) && read_exact(f, &s->p, 1) &&
	       read_exact(f, &s->e, 1) && read_exact(f, &s->dbr, 1) &&
	       read_exact(f, &s->pbr, 1) && read_exact(f, &s->d, 2);
}

static bool
read_addr24(FILE *f, uint32_t *out)
{
	uint32_t v = 0;
	if (!read_exact(f, &v, 4)) return false;
	*out = v & 0xFFFFFF;
	return true;
}

// Why read_case() last returned false. A truncated fixture and a case too large
// for these buffers look identical otherwise, and the loop just stops early.
static const char *read_error = NULL;

static bool
read_case(FILE *f, pt_case *c)
{
	if (!read_exact(f, &c->opcode, 1)) return false;  // clean end of file
	if (!read_exact(f, &c->cpu, 1)) { read_error = "truncated"; return false; }
	if (!read_state(f, &c->initial)) { read_error = "truncated"; return false; }
	if (!read_exact(f, &c->init_ram_n, 2)) { read_error = "truncated"; return false; }
	if (c->init_ram_n > MAX_RAM) { read_error = "initial ram too large"; return false; }
	for (int i = 0; i < c->init_ram_n; i++) {
		if (!read_addr24(f, &c->init_ram[i].addr)) return false;
		if (!read_exact(f, &c->init_ram[i].value, 1)) return false;
	}
	if (!read_state(f, &c->final_)) { read_error = "truncated"; return false; }
	if (!read_exact(f, &c->final_ram_n, 2)) { read_error = "truncated"; return false; }
	if (c->final_ram_n > MAX_RAM) { read_error = "final ram too large"; return false; }
	for (int i = 0; i < c->final_ram_n; i++) {
		if (!read_addr24(f, &c->final_ram[i].addr)) return false;
		if (!read_exact(f, &c->final_ram[i].value, 1)) return false;
	}
	if (!read_exact(f, &c->cycle_n, 2)) { read_error = "truncated"; return false; }
	if (c->cycle_n > MAX_CYCLES) { read_error = "too many cycles"; return false; }
	for (int i = 0; i < c->cycle_n; i++) {
		if (!read_addr24(f, &c->cycles[i].addr)) return false;
		if (!read_exact(f, &c->cycles[i].value, 1)) return false;
		if (!read_exact(f, &c->cycles[i].flags, 1)) return false;
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
	bool not_comparable;
} pt_opcode;

static pt_opcode by_opcode[256];

// Native mode picks the accumulator and index widths independently, so a fault
// in one width looks like scattered opcode failures until the results are split
// this way.
enum { MODE_EMU, MODE_M8X8, MODE_M8X16, MODE_M16X8, MODE_M16X16, MODE_COUNT };

static const char *mode_name[MODE_COUNT] = {
	"emulation", "native M8 X8", "native M8 X16", "native M16 X8",
	"native M16 X16",
};

static struct {
	long total, state_ok, cycles_ok;
} by_mode[MODE_COUNT];

static int
mode_of(const pt_case *c)
{
	if (c->cpu != PT_CPU_65816 || c->initial.e) {
		return MODE_EMU;
	}
	bool m8 = (c->initial.p & FLAG_MEMORY_WIDTH) != 0;
	bool x8 = (c->initial.p & FLAG_INDEX_WIDTH) != 0;
	if (m8) {
		return x8 ? MODE_M8X8 : MODE_M8X16;
	}
	return x8 ? MODE_M16X8 : MODE_M16X16;
}

// Opcodes the suite records in a way that cannot be compared against a single
// step of this emulator. Counted separately rather than left to read as
// failure, since none of them is a fault in the core.
//
//   $44 MVP, $54 MVN   Move one byte per execution and rewind PC so the
//                      instruction runs again, which is what makes a block move
//                      interruptible. The suite records a fixed 100-cycle
//                      prefix of the whole move -- fourteen iterations of a
//                      transfer that may run for thousands.
//
//   $CB WAI            Halts until an interrupt arrives. Both WDC datasheets
//                      give three cycles; the suite records a fourth from the
//                      wait state, which on hardware continues indefinitely.
//
//   $DB                STP on a stock 65C816. This emulator maps it to a
//                      debugger trap instead, which is a deliberate difference
//                      rather than a timing error.
static bool
is_not_comparable(const pt_case *c)
{
	if (c->cpu != PT_CPU_65816) {
		return false;
	}
	switch (c->opcode) {
		case 0x44: case 0x54: case 0xCB: case 0xDB:
			return true;
		default:
			return false;
	}
}

static void
run_case(const pt_case *c, pt_result *r)
{
	const bool is816 = (c->cpu == PT_CPU_65816);

	// The suite chooses the mode per case, through the E bit and the width
	// flags in P, so reset into the plainest configuration for the core and
	// let the case's own registers select the rest.
	cpu_reset_to(is816 ? CPU_816_EMU : CPU_65C02, 0x0000);
	for (int i = 0; i < c->init_ram_n; i++) {
		cpu_seed(c->init_ram[i].addr, c->init_ram[i].value);
	}
	regs.pc     = c->initial.pc;
	regs.status = c->initial.p;

	if (is816) {
		regs.e  = c->initial.e;
		regs.sp = c->initial.s;
		regs.c  = c->initial.a;
		regs.x  = c->initial.x;
		regs.y  = c->initial.y;
		regs.dp = c->initial.d;
		regs.db = c->initial.dbr;
		regs.k  = c->initial.pbr;
		// Emulation mode pins the stack to page one and forces both widths;
		// the suite supplies a full 16-bit S even there.
		if (regs.e) {
			regs.sp = 0x0100 | (c->initial.s & 0xFF);
			regs.status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;
		}
	} else {
		regs.sp = 0x0100 | (c->initial.s & 0xFF);
		regs.a  = (uint8_t)c->initial.a;
		regs.xl = (uint8_t)c->initial.x;
		regs.yl = (uint8_t)c->initial.y;
	}

	bus_n = 0;
	cpu_bus_read_hook = note_read;
	cpu_bus_write_hook = note_write;
	uint32_t spent = cpu_step();
	cpu_bus_read_hook = NULL;
	cpu_bus_write_hook = NULL;

	r->total++;
	pt_opcode *op = &by_opcode[c->opcode];
	op->total++;
	op->not_comparable = is_not_comparable(c);
	const int mode = mode_of(c);
	by_mode[mode].total++;

	// Compare at the width the case ran in. Reading regs.a in a 16-bit case
	// would drop the high byte and report a false pass.
	uint16_t got_a = is816 ? regs.c : (uint16_t)regs.a;
	uint16_t got_x = is816 ? regs.x : (uint16_t)regs.xl;
	uint16_t got_y = is816 ? regs.y : (uint16_t)regs.yl;
	uint16_t got_s = is816 && !regs.e ? regs.sp
	                                  : (uint16_t)(regs.sp & 0xFF);
	uint16_t want_s = is816 && !c->final_.e ? c->final_.s
	                                        : (uint16_t)(c->final_.s & 0xFF);

	bool regs_but_p = regs.pc == c->final_.pc &&
	                  got_s == want_s &&
	                  got_a == c->final_.a &&
	                  got_x == c->final_.x &&
	                  got_y == c->final_.y;
	if (is816) {
		regs_but_p = regs_but_p && regs.dp == c->final_.d &&
		             regs.db == c->final_.dbr && regs.k == c->final_.pbr &&
		             regs.e == c->final_.e;
	}
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
		by_mode[mode].state_ok++;
	}
	if (state_but_b) {
		r->state_ok_ignoring_b++;
		op->state_ok_ignoring_b++;
	}
	if (!state_but_b && !op->have_sample) {
		op->have_sample = true;
		snprintf(op->sample, sizeof op->sample,
		         "PC $%04X/$%04X A $%04X/$%04X X $%04X/$%04X "
		         "Y $%04X/$%04X P $%02X/$%02X S $%04X/$%04X D $%04X/$%04X "
		         "DBR $%02X/$%02X PBR $%02X/$%02X E %d/%d (got/want)",
		         regs.pc, c->final_.pc, got_a, c->final_.a,
		         got_x, c->final_.x, got_y, c->final_.y,
		         regs.status, c->final_.p, got_s, want_s,
		         regs.dp, c->final_.d, regs.db, c->final_.dbr,
		         regs.k, c->final_.pbr, regs.e, c->final_.e);
	}
	if (!state && !r->have_first_bad) {
		r->have_first_bad = true;
		r->first_bad_opcode = c->opcode;
		snprintf(r->first_bad, sizeof r->first_bad,
		         "opcode $%02X: PC $%04X/$%04X A $%04X/$%04X X $%04X/$%04X "
		         "Y $%04X/$%04X P $%02X/$%02X S $%04X/$%04X",
		         c->opcode, regs.pc, c->final_.pc, got_a, c->final_.a,
		         got_x, c->final_.x, got_y, c->final_.y,
		         regs.status, c->final_.p, got_s, want_s);
	}

	if (spent == c->cycle_n) {
		r->cycles_ok++;
		op->cycles_ok++;
		by_mode[mode].cycles_ok++;
	}

	bool trace = (bus_n == c->cycle_n);
	for (int i = 0; trace && i < bus_n; i++) {
		if (bus[i].addr != c->cycles[i].addr ||
		    (bus[i].flags & PTF_WRITE) != (c->cycles[i].flags & PTF_WRITE)) {
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
		snprintf(probe, sizeof probe, "%stests/pt_convert.py", candidates[i]);
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

// Which suite to run. One binary rather than three: the fixtures differ only in
// which cases they hold, and the runner already reads the CPU from each case.
static const char *variant = "wdc65c02";

static const char *
find_fixture(char *buf, size_t buflen)
{
	char rel[128];
	snprintf(rel, sizeof rel, "pt/%s.bin", variant);
	tests_path(buf, buflen, rel);
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
static const char *
find_baseline(char *buf, size_t buflen)
{
	char rel[128];
	snprintf(rel, sizeof rel, "pt_baseline_%s.txt", variant);
	tests_path(buf, buflen, rel);
	FILE *f = fopen(buf, "rb");
	if (f) {
		fclose(f);
		return buf;
	}
	return NULL;
}

static const char *
baseline_path(char *buf, size_t buflen)
{
	char rel[128];
	snprintf(rel, sizeof rel, "pt_baseline_%s.txt", variant);
	return tests_path(buf, buflen, rel);
}

static void
write_baseline(const char *path, long per_opcode)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		printf("     could not write %s\n", path);
		return;
	}
	fprintf(f, "# ProcessorTests results for %s: cases matching hardware,\n", variant);
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
	char rpath[256], rel[128];
	snprintf(rel, sizeof rel, "../pt-report-%s.txt", variant);
	tests_path(rpath, sizeof rpath, rel);
	FILE *f = fopen(rpath, "wb");
	if (!f) {
		return;
	}
	fprintf(f, "# ProcessorTests per-opcode results for %s, %ld cases per opcode.\n",
	        variant, per_opcode);
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
main(int argc, char **argv)
{
	if (argc > 1) {
		variant = argv[1];
	}

	char label[64];
	snprintf(label, sizeof label, "processor_tests[%s]", variant);

	char path[256];
	if (!find_fixture(path, sizeof path)) {
		printf("skip: no %s fixture (see tests/fetch_pt.py)\n", variant);
		return x16_test_summary(label);
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
	if (version != 2) {
		printf("FAIL: %s is format %u, this reads 2 -- rebuild with "
		       "tests/fetch_pt.py --force\n", path, version);
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

	// Stopping early would otherwise look like a small, clean suite.
	if (r.total != (long)count) {
		printf("FAIL: read %ld of %u cases (%s)\n", r.total, count,
		       read_error ? read_error : "unexpected end of file");
		return 1;
	}

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
	for (int i = 0; i < MODE_COUNT; i++) {
		if (by_mode[i].total > 0) {
			printf("     %-15s state %ld/%ld  cycles %ld/%ld\n", mode_name[i],
			       by_mode[i].state_ok, by_mode[i].total,
			       by_mode[i].cycles_ok, by_mode[i].total);
		}
	}
	if (r.have_first_bad) {
		printf("     first state mismatch: %s\n", r.first_bad);
	}
	write_report(per_opcode);

	// A run matching its baseline is still wrong; say so, or green reads as
	// correct.
	int diverging = 0;
	int not_comparable = 0;
	for (int i = 0; i < 256; i++) {
		pt_opcode *op = &by_opcode[i];
		if (op->total == 0) {
			continue;
		}
		if (op->not_comparable) {
			not_comparable++;
			continue;
		}
		if (op->state_ok_ignoring_b < op->total || op->cycles_ok < op->total) {
			diverging++;
		}
	}
	if (not_comparable > 0) {
		printf("     %d opcodes not comparable (see is_not_comparable)\n",
		       not_comparable);
	}
	if (diverging > 0) {
		printf("DIVERGE: %s: %d of %d opcodes do not match hardware on state "
		       "or cycles -- see pt-report-%s.txt\n",
		       variant, diverging, opcodes_seen, variant);
	}

	check(r.total > 0, "the fixture contained cases");

	char bpath[256];
	if (getenv("X16_PT_WRITE_BASELINE")) {
		write_baseline(baseline_path(bpath, sizeof bpath), per_opcode);
	} else if (!even) {
		printf("     uneven case counts per opcode; skipping the baseline check\n");
	} else if (!find_baseline(bpath, sizeof bpath)) {
		printf("     no baseline yet; run with X16_PT_WRITE_BASELINE=1\n");
	} else {
		int bad = compare_baseline(bpath, per_opcode);
		check(bad >= 0, "the baseline was built for this many cases per opcode");
		check(bad <= 0, "every opcode matches its recorded baseline");
	}

	return x16_test_summary(label);
}
