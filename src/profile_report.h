// Commander X16 Emulator — capture report: where the time went, by routine.
//
// perf_hist records cycles per effective address -- (RAM bank, PC) in the
// banked window, (ROM bank, PC) above $C000, plain PC below $A000. This rolls
// that up to routines: every address is charged to the nearest label at or
// below it, in the same bank, and the routines are ranked by self time.
//
// BANKS DECIDE THE NAME. On an X16 the same $A000-$BFFF address holds a
// different program in every RAM bank, so a lookup that ignores the bank puts
// whichever bank's label happens to be nearest on every banked routine. Labels
// carry the bank their segment was loaded into when the debug info knows it;
// a key is named from its own bank's labels first, and only falls back to a
// label whose bank is unknown -- in which case the row says so (ambiguous).
//
// UNNAMED TIME IS STILL TIME. An address the debug info does not cover (the
// KERNAL, BASIC, anything without a .dbg) is grouped by 256-byte page and
// named by where it is, so the rows always add up to the histogram's total.
//
// Built only when a capture ends, never while recording. The symbol table and
// the coverage test are passed in, so this needs no debug-info module and is
// unit-tested on its own (tests/test_profile_report.c).
#ifndef _PROFILE_REPORT_H_
#define _PROFILE_REPORT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One label. `bank` is its RAM bank for $A000-$BFFF, or -1 when unknown or
// not banked.
typedef struct {
	const char *name;
	uint16_t    addr;
	int16_t     bank;
} prof_sym_t;

// Does debug info cover `pc` (in `ram_bank` for the banked window, -1
// otherwise)? 0 = no, 1 = yes, 2 = yes but which bank is not established.
// Labels are trusted only where this is non-zero: a nearest-label lookup will
// otherwise happily name an address it knows nothing about.
typedef int (*prof_covered_fn)(uint16_t pc, int ram_bank);

#define PROF_NAME_MAX 64

typedef struct {
	char     name[PROF_NAME_MAX];
	uint8_t  region;      // PH_REGION_* (perf_hist.h)
	uint8_t  bank;        // RAM or ROM bank, for the banked regions
	uint16_t addr;        // the label's address, or the page start if unnamed
	bool     named;       // charged to a label, not a page
	bool     ambiguous;   // the label's (or the code's) bank was not established
	uint64_t main_cycles; // self time outside interrupt handlers
	uint64_t irq_cycles;  // self time inside them
	uint64_t instructions;
} prof_row_t;

typedef struct {
	prof_row_t *rows;     // worst first, by main + irq cycles
	int         count;
	uint64_t    main_cycles, irq_cycles, instructions; // the histogram's totals
	int         named_rows, ambiguous_rows;
} prof_report_t;

// Build from the armed perf_hist histogram. `syms` need not be sorted.
// Returns false if the histogram is not armed or memory ran out.
bool prof_report_build(prof_report_t *out, const prof_sym_t *syms, int nsyms,
                       prof_covered_fn covered);
void prof_report_free(prof_report_t *r);

#ifdef __cplusplus
}
#endif

#endif
