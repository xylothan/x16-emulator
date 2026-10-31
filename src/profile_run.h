// Commander X16 Emulator — a saved profile run: one capture's report, call
// tree, waterfall and the names and source lines it was shown with, in one
// file, so the Profile panel can show it again later without the emulator,
// the program or its debug info being as they were.
//
// Pure: plain data in and out, and a file. profile_session.c decides what goes
// in and installs what comes out.
//
// The file is "X16PROF\0", a version, then tagged sections, each with its
// length, so a reader skips a section it does not know. Numbers are
// little-endian and written one field at a time.
#ifndef PROFILE_RUN_H
#define PROFILE_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile_report.h"
#include "waterfall.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROF_RUN_EXT ".x16prof"

typedef struct {
	char     captured[32];   // local time the run was saved: "2026-10-25 16:21:41"
	char     revision[64];   // the emulator build that recorded it
	char     program[512];   // the -prg file, if any
	char     dbg[2048];      // the .dbg files loaded, "; " between them
	char     end_reason[48]; // why the capture ended
	double   mhz;            // the emulated clock it was recorded at
	double   elapsed_s;      // host time the capture ran
	uint32_t frames;
	uint32_t recorders;      // CAP_REC_* bits
	bool     tree_consistent;
	// Work against waiting (profile_session.h's prof_budget_summary_t).
	bool     b_have;
	uint32_t b_frames, b_work_max, b_overruns, b_budget;
	uint64_t b_total, b_idle;
	// The target the run was judged against when it was saved, and one
	// vsync's cycles. 0 in runs saved before these were recorded.
	float    b_target_fps;
	uint32_t b_vsync_cycles;
} prof_run_meta_t;

// What one perf_hist key was shown as: the report row it belongs to (-1 for
// none) and its source line ("" for none).
typedef struct {
	uint32_t key;
	int32_t  row;
	char    *source;
} prof_run_key_t;

typedef struct {
	prof_run_meta_t meta;
	prof_report_t   report; // rows and nodes; `lookup` is NULL
	prof_run_key_t *keys;   // sorted by key
	int             nkeys;
	wf_staged_t    *wf;     // the waterfall, for wf_install(); NULL without one
	uint32_t       *work;   // each vsync's CPU work, oldest first; NULL in older runs
	uint32_t        nwork;
} prof_run_t;

// Write a run. `keys` need not be sorted. With `with_waterfall`, the
// waterfall recorder's current state goes in too. `work` is each vsync's CPU
// work, oldest first, so the run can be judged again against another target
// frame rate; NULL for none. Returns 0, or -1 with the reason in `msg`.
int prof_run_write(const char *path, const prof_run_meta_t *meta, const prof_report_t *report,
                   const prof_run_key_t *keys, int nkeys, bool with_waterfall, const uint32_t *work,
                   uint32_t nwork, char *msg, size_t msg_size);

// Read a whole run into `out`. Nothing outside `out` changes, so a bad file
// costs nothing. Returns 0, or -1 with the reason in `msg` (out is then
// empty).
int prof_run_read(const char *path, prof_run_t *out, char *msg, size_t msg_size);

// Free what prof_run_read() allocated that was not handed on.
void prof_run_free(prof_run_t *run);

// A key's entry, or NULL.
const prof_run_key_t *prof_run_find_key(const prof_run_key_t *keys, int n, uint32_t key);

#ifdef __cplusplus
}
#endif

#endif
