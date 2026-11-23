// Commander X16 Emulator — is a host path a file that is safe to open?
#pragma once

#include <stdbool.h>

// True if `path` is a regular file this process can read. Nothing that is not a
// regular file is opened to find out, so a FIFO, a device or a named pipe cannot
// block the caller. Paths are read as fopen() reads them.
bool host_regular_file_readable(const char *path);
