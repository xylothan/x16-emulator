// Commander X16 Emulator — ImGui debugger: where to look for source files.
//
// The debug info names source files, often by a path from the machine that
// built the program. These are the folders the debugger searches for them,
// managed from the Settings window's Source folders page (and the Source
// panel). Folders added
// here are saved in imgui.ini; -srcpath and the debug info's own folder are
// added at startup and are not saved.
#pragma once

// The Settings page. Open it from anywhere with debug_ui_open_source_folders().
void source_folders_draw_page(void);
void debug_ui_open_source_folders(void);

// Add a folder to the search list, and look again for every file not found
// so far. `remember` saves it in imgui.ini.
void source_folders_add(const char *dir, bool remember);

// Look under `root` for source files the debug info names but that are not
// found yet -- just `only` when it is non-NULL -- and add each folder they turn
// up in. The search runs a few milliseconds per debugger frame, so the UI and
// the machine keep running while a big tree is read. Starting one cancels any
// still running. False (with the reason in the status) when there is nothing
// to search for.
bool source_folders_search_start(const char *root, const char *only);
void source_folders_search_cancel(void);
bool source_folders_search_busy(void);
// Progress while it runs, then what it found; "" before the first search.
const char *source_folders_search_status(void);
// Advance the search. Once per debugger frame, whether or not anything
// showing it is open.
void source_folders_search_tick(void);
