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
#include "waterfall.h"
#include "profile_run.h"

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
// Bumped only for a new recording or a saved run opened -- not when the same
// one is named again -- so a view can tell "start over" from "redraw".
uint32_t             profile_session_recording_generation(void);

// Name the report again if the debug info changed since it was built: a .dbg
// replaced, reloaded or added. Rows are regrouped under the new labels and the
// report generation bumps. A saved run keeps the names it was saved with (it
// has no per-address counts to regroup). Call once a frame, before anything
// reads the report -- the debugger does, whether or not the machine runs.
void profile_session_sync_names(void);
// Whether the debug info changed since a saved run was opened, so what it
// shows was named by debug info that is no longer loaded.
bool profile_session_run_names_stale(void);

// Write the report as a table: totals, then the top `top` rows.
void profile_session_print(FILE *f, int top);

// A call-tree node's display name: its routine, or [main] / [IRQ] / [other],
// or the bare address when nothing names it.
void profile_session_node_name(const prof_report_t *r, int node, char *buf, int size);

// Work against waiting over the capture, from perf_budget's classifier --
// only when the capture ran the budget recorder.
//
// Judged against the target frame rate (perf_budget_get_target_fps(), the
// setting the Performance panel shares): a game frame is `span` vsyncs, and
// its work is the sum over them, the same rolling sum the Performance panel
// uses. Changing the target judges the same recording again, because each
// vsync's work is kept -- except in runs saved before that was, which keep
// the verdict they were saved with (`rejudged` false).
typedef struct {
	bool     have;
	uint32_t frames;     // vsyncs it covers
	uint64_t total;      // cycles
	uint64_t idle;       // of which classified as waiting (WAI, spin loops)
	uint32_t work_max;   // busiest single vsync's work
	uint32_t frame_max;  // busiest game frame's work: `span` vsyncs summed
	uint32_t overruns;   // game frames whose work exceeded the budget
	uint32_t budget;     // a game frame's budget, in cycles
	int      span;       // vsyncs in a game frame
	float    target_fps; // what it was judged against
	bool     rejudged;   // judged against the current target, not as saved
} prof_budget_summary_t;
const prof_budget_summary_t *profile_session_budget_summary(void);

// The target frame rate a saved run was judged against when it was saved; 0
// for a live recording or a run too old to say.
float profile_session_recorded_target_fps(void);

// Write the last report to a file. Both return the number of records written,
// or -1 if there is no report or the file cannot be written.
//   folded: one line per call path with self cycles, "a;b;c 1234" -- the
//           collapsed-stack format speedscope and flamegraph.pl read. Needs
//           the call tree.
//   json:   totals, every row, and the call tree, for scripts.
int profile_session_export_folded(const char *path);
int profile_session_export_json(const char *path);
//   trace:  the waterfall's detailed frames (the last few and the worst) as
//           Chrome trace-event JSON, for ui.perfetto.dev or chrome://tracing:
//           routine spans on CPU / IRQ tracks, every device write as an
//           instant event with its routine and source line. Needs the
//           waterfall recorder.
int profile_session_export_trace(const char *path);

// Whether the last capture recorded the waterfall.
bool profile_session_has_waterfall(void);

// The waterfall's write counts rolled up by routine: one entry per (report
// row, lane), heaviest first. `row` is -1 for writes from code no row covers.
// Valid until the next call; NULL with *count 0 when there is none.
typedef struct {
	int      row;
	int      lane;
	uint32_t key;    // one of the writing instructions, for naming a row of -1
	uint32_t writes;
} prof_writes_t;
const prof_writes_t *profile_session_writes_by_routine(int *count);

// A perf_hist key, for display: its routine (or address), its bank:PC, and
// its source line as "file:line" (false when the debug info has none).
void profile_session_key_name(uint32_t key, char *buf, int size);

// The emulated CPU clock, in MHz: cycles per microsecond.
double profile_session_mhz(void);
void profile_session_key_where(uint32_t key, char *buf, int size);
bool profile_session_key_source(uint32_t key, char *buf, int size);

// Whether the tree's self cycles matched the histogram's totals exactly, for
// the last report (true when there was no tree).
bool profile_session_tree_consistent(void);

// Saved runs (profile_run.h): the last recording -- report, call tree,
// waterfall, and the names and source lines it was shown with -- in one file,
// to be opened again later. Loading one shows it in place of the last
// recording until the next recording starts; neither works while one runs.
// Both return 0, or -1 with the reason in `msg`.
int  profile_session_save_run(const char *path, char *msg, size_t msg_size);
int  profile_session_load_run(const char *path, char *msg, size_t msg_size);
// Whether a saved run is what is shown, and what it says about itself.
bool                   profile_session_run_loaded(void);
const prof_run_meta_t *profile_session_run_meta(void);
const char            *profile_session_run_path(void);

// Recorder names for DAP and the command line ("hist", "budget", "vram",
// "tree").
uint32_t    profile_session_recorder_bit(const char *name);
const char *profile_session_recorder_name(uint32_t bit);

#ifdef __cplusplus
}
#endif

#endif
