// Commander X16 Emulator — ImGui debugger: profile recordings as files.
//
// Saving the shown recording as a run (profile_run.h), opening a saved run,
// and exporting for other tools -- the File menu's commands. Files go in the
// profile export folder (export_paths.h) under the name and date-stamp rule
// set in Settings > Export folders, unless a dialog chose otherwise. What the
// last command did is in profile_files_status(), which the Profile panel
// shows.
#ifndef X16_PROFILE_FILES_H
#define X16_PROFILE_FILES_H

enum ProfileExport {
    PROFILE_EXPORT_FOLDED, // call paths with own time, for speedscope / flamegraph.pl
    PROFILE_EXPORT_JSON,   // every routine and the call tree, for scripts
    PROFILE_EXPORT_TRACE,  // the waterfall's frames as a Chrome trace
};

// Whether there is a recording to save or export, and nothing recording.
bool profile_files_can_save(void);
bool profile_files_can_export(ProfileExport kind);
// Whether a run can be opened now (nothing recording).
bool profile_files_can_open(void);

// Save to the export folder, named with the date and time: a run records one
// recording and is never replaced.
void profile_files_save_run(void);
// Save where a dialog says (Windows); elsewhere the same as save_run.
void profile_files_save_run_as(void);
// A dialog (Windows), or a window listing the runs in the export folder with
// a path box (elsewhere).
void profile_files_open_run(void);
// Open one, by path. The Profile panel is brought up to show it.
void profile_files_open_path(const char *path);
void profile_files_export(ProfileExport kind);

// The saved runs in the export folder, newest first, at most `max`: for a
// "recent" list. Paths stay valid until the next call.
int         profile_files_recent(int max);
const char *profile_files_recent_at(int i);

// What the last command did; "" before the first.
const char *profile_files_status(void);

// Once per debugger frame: the Open run window, when it is open.
void profile_files_draw(void);

#endif
