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
// THE ROM HAS ITS OWN LABELS. Symbols marked `rom` come from the ROM's .sym
// files (rom_syms.h): above $C000 they name each ROM bank's code; below, they
// name the KERNAL's code in low RAM ($0200-$03FF), and only where the
// program's debug info does not cover the address.
//
// UNNAMED TIME IS STILL TIME. An address no label covers is grouped by
// 256-byte page and named by where it is, so the rows always add up to the
// histogram's total.
//
// INCLUSIVE TIME comes from the calling-context tree (profile_cct.h), added
// with prof_report_add_tree(): each tree node is matched to the row its call
// target falls in, and a row's inclusive time is the sum over its nodes --
// skipping a node that sits under another node of the same row, so recursion
// is not counted twice.
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

// One label. `bank` is its RAM bank for $A000-$BFFF, its ROM bank for
// $C000-$FFFF, or -1 when unknown or not banked. `rom` marks a label from the
// ROM's own symbols rather than the program's debug info.
typedef struct {
	const char *name;
	uint16_t    addr;
	int16_t     bank;
	bool        rom;
} prof_sym_t;

// Where ROM labels may name low memory: the KERNAL's RAM-resident code.
#define PROF_KRAM_START 0x0200u
#define PROF_KRAM_END   0x0400u

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
	// From the tree; zero without one.
	uint64_t incl_cycles; // from entry to return, callees included
	uint32_t calls;
	uint32_t call_max;    // worst single call
	uint32_t frame_max;   // worst frame, over its call sites
} prof_row_t;

// One calling-context tree node, as profile_cct.h records it.
typedef struct {
	int32_t  parent;      // index of the parent node; -1 for a root
	int32_t  row;         // rows[] index of the routine it calls; -1 if none
	uint32_t key;         // perf_hist key of the call target, or a CCT_KEY_*
	uint64_t self, incl;
	uint32_t calls, call_max, frame_max;
} prof_node_t;

// One histogram key that ran, and what it counted.
typedef struct {
	uint32_t key, main, irq, count;
} prof_key_count_t;

typedef struct {
	prof_row_t  *rows;    // worst first, by main + irq cycles
	int          count;
	uint64_t     main_cycles, irq_cycles, instructions; // the histogram's totals
	int          named_rows, ambiguous_rows;
	prof_node_t *nodes;   // the call tree, parents before children; or NULL
	int          node_count;
	void        *lookup;  // private: the symbol tables, kept for add_tree
	// The histogram's keys that ran, in key order, as they were when built;
	// NULL for a report read back from a saved run, which has no counts.
	prof_key_count_t *hist;
	int               hist_count;
} prof_report_t;

// Build from the armed perf_hist histogram. `syms` need not be sorted.
// Returns false if the histogram is not armed or memory ran out.
bool prof_report_build(prof_report_t *out, const prof_sym_t *syms, int nsyms,
                       prof_covered_fn covered);
void prof_report_free(prof_report_t *r);

// Build the rows again from the counts kept at build time, with other
// symbols -- after the debug info was replaced -- and re-attach the tree.
// Rows are regrouped and re-sorted, so a row index from before means nothing
// after. The report is unchanged on failure. False without kept counts
// (`hist` is NULL) or out of memory.
bool prof_report_rename(prof_report_t *r, const prof_sym_t *syms, int nsyms, prof_covered_fn covered);

// Attach a call tree: copies `nodes` (row is filled in here; parents must
// precede children), and fills each row's incl_cycles, calls, call_max and
// frame_max. False if out of memory.
bool prof_report_add_tree(prof_report_t *r, const prof_node_t *nodes, int count);

// The row a perf_hist key is charged to, or -1 if that key never ran.
int prof_report_row_of_key(const prof_report_t *r, uint32_t key);

// One routine's neighbours, merged over every call path: who called it and
// how much of its time each caller caused; what it called and how much each
// cost it. A neighbour is a report row, or for a tree root ([main], [IRQ]) or
// code no row names, a single node: `row` is -1 then and `node` is it.
typedef struct {
	int      row;
	int      node;   // a node of this neighbour, for naming and jumping
	uint64_t cycles; // callers: the routine's time they caused; callees: their cost to it
	uint32_t calls;
} prof_edge_t;

typedef struct {
	prof_edge_t *callers, *callees;  // heaviest first
	int          ncallers, ncallees;
	uint64_t     total;  // the routine's total time (== rows[row].incl_cycles)
	uint64_t     own;    // its own instructions
	uint32_t     calls;
} prof_routine_view_t;

// Build the view for report row `row`. Recursion is folded in: a call from the
// routine to itself is neither a caller nor a callee, and time inside it is
// counted once. Invariants: the callers' cycles sum to `total`, and so do `own`
// plus the callees' cycles. False if the report has no tree or row is out of range.
bool prof_report_routine_view(const prof_report_t *r, int row, prof_routine_view_t *out);
void prof_routine_view_free(prof_routine_view_t *v);

// A routine's neighbourhood, two calls each way, for drawing as boxes and
// arrows. Built from prof_report_routine_view() of the routine and of each
// neighbour, so its numbers are the Callers & callees tab's.
//
// Columns, left to right: callers of its callers, its callers, the routine,
// its callees, their callees. A routine appears once, in the first column that
// reaches it; meeting it again (recursion, a callee that also calls back)
// draws an arrow to that box instead of a second one. Each column keeps its
// `per_col` heaviest, and counts the rest in `hidden`.
#define PROF_GRAPH_COLS   5
#define PROF_GRAPH_CENTRE 2
typedef struct {
	int      row;    // report row; -1 for the top of the tree ([main], [IRQ])
	int      node;   // a node of it, for naming and jumping
	int      col;    // 0 .. PROF_GRAPH_COLS-1
	uint64_t weight; // what placed it: the arrow's time (the routine: its total)
} prof_gbox_t;
typedef struct {
	int      from, to; // boxes: caller -> callee
	uint64_t cycles;   // the callee's time in the calls the caller made
	uint32_t calls;
} prof_gedge_t;
typedef struct {
	prof_gbox_t  *boxes; // boxes[0] is the routine
	int           nboxes;
	prof_gedge_t *edges;
	int           nedges;
	int           hidden[PROF_GRAPH_COLS];
} prof_graph_t;
bool prof_report_graph(const prof_report_t *r, int row, int per_col, prof_graph_t *out);
void prof_graph_free(prof_graph_t *g);

#ifdef __cplusplus
}
#endif

#endif
