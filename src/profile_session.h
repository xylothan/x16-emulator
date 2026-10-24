// Commander X16 Emulator — capture mode, wired to the emulator.
//
// profile_capture.c is the state machine and profile_report.c the analysis;
// both are pure. This is the glue: which recorders a capture arms, the debug
// info the report is named from, and the one place the UI, DAP and the
// command line all start, stop and read a capture through.
#ifndef _PROFILE_SESSION_H_
#define _PROFILE_SESSION_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "profile_capture.h"
#include "profile_report.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the state machine's hooks and the $9FBC marker hook. Once, at start.
void profile_session_init(void);

// An emulated frame finished (both emulator loops).
void profile_session_frame_end(void);

// The report from the last capture that ended, or NULL if none has.
const prof_report_t *profile_session_report(void);
uint32_t             profile_session_report_frames(void); // frames it covered
// Bumped for every new report. A new report can reuse the old one's
// allocations and counts, so this is what tells a cache it is stale.
uint32_t             profile_session_report_generation(void);

// Write the report as a table: totals, then the top `top` rows.
void profile_session_print(FILE *f, int top);

// A call-tree node's display name: its routine, or [main] / [IRQ] / [other],
// or the bare address when nothing names it.
void profile_session_node_name(const prof_report_t *r, int node, char *buf, int size);

// Work against waiting over the capture, from perf_budget's classifier --
// only when the capture ran the budget recorder.
typedef struct {
	bool     have;
	uint32_t frames;     // frames it covers
	uint64_t total;      // cycles
	uint64_t idle;       // of which classified as waiting (WAI, spin loops)
	uint32_t work_max;   // worst frame's work
	uint32_t overruns;   // frames whose work exceeded the budget
	uint32_t budget;     // the per-frame budget, in cycles
} prof_budget_summary_t;
const prof_budget_summary_t *profile_session_budget_summary(void);

// Write the last report to a file. Both return the number of records written,
// or -1 if there is no report or the file cannot be written.
//   folded: one line per call path with self cycles, "a;b;c 1234" -- the
//           collapsed-stack format speedscope and flamegraph.pl read. Needs
//           the call tree.
//   json:   totals, every row, and the call tree, for scripts.
int profile_session_export_folded(const char *path);
int profile_session_export_json(const char *path);

// Whether the tree's self cycles matched the histogram's totals exactly, for
// the last report (true when there was no tree).
bool profile_session_tree_consistent(void);

// Recorder names for DAP and the command line ("hist", "budget", "vram",
// "tree").
uint32_t    profile_session_recorder_bit(const char *name);
const char *profile_session_recorder_name(uint32_t bit);

#ifdef __cplusplus
}
#endif

#endif
