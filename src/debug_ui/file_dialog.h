// Commander X16 Emulator — ImGui debugger: the host's own "open a file"
// dialog, where there is one (Windows). Elsewhere there is none and the
// caller keeps its typed-path field; file_dialog_available() says which.
#ifndef X16_FILE_DIALOG_H
#define X16_FILE_DIALOG_H

#include <stddef.h>

bool file_dialog_available(void);

// Ask for an existing file. `what` names the filter ("Debug info (*.dbg)")
// and `pattern` is its wildcard ("*.dbg"); an "All files" choice is added.
// Starts in `start_dir` when it is not empty. Blocks until the dialog closes.
// True with the chosen path (UTF-8) in `out`; false if cancelled or there is
// no dialog. The working directory is left as it was.
bool file_dialog_open(const char *title, const char *what, const char *pattern, const char *start_dir,
                      char *out, size_t out_size);

#endif
