// Commander X16 Emulator — ImGui debugger: where to look for source files.
//
// The debug info names source files, often by a path from the machine that
// built the program. These are the folders the debugger searches for them,
// managed from System > Source folders (and the Source panel). Folders added
// here are saved in imgui.ini; -srcpath and the debug info's own folder are
// added at startup and are not saved.
#pragma once

// The window. Open it from anywhere with debug_ui_open_source_folders().
void debug_ui_draw_source_folders_window(void);
void debug_ui_open_source_folders(void);

// Add a folder to the search list, and look again for every file not found
// so far. `remember` saves it in imgui.ini.
void source_folders_add(const char *dir, bool remember);

// Look under `root` for source files the debug info names but that are not
// found yet -- just `only` when it is non-NULL -- and add each folder they turn
// up in. Writes a one-line summary into `msg`. Returns how many were found.
int source_folders_find_under(const char *root, const char *only, char *msg, int msg_size);
