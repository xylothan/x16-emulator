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

// Write the report as a table: totals, then the top `top` rows.
void profile_session_print(FILE *f, int top);

// Recorder names for DAP and the command line ("hist", "budget", "vram").
uint32_t    profile_session_recorder_bit(const char *name);
const char *profile_session_recorder_name(uint32_t bit);

#ifdef __cplusplus
}
#endif

#endif
