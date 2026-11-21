// Commander X16 Emulator — where a debugger client may write a host file.
//
// A client names the file, so the name cannot be trusted to stay anywhere in
// particular. Everything is resolved by the operating system before it is
// compared: `..`, links and letter case are its business, not a string match's.
#ifndef HOST_WRITE_PATH_H
#define HOST_WRITE_PATH_H

#include <stdbool.h>
#include <stddef.h>

// Resolve `request`, relative to `root` unless absolute, into `out`. Its folder
// must exist and lie inside `root`, which must exist and not be a drive root.
// The file itself may not be a link. `*exists` says whether it is already there.
// On failure, `err` says why and nothing has been touched.
bool host_write_path(const char *root, const char *request, char *out, size_t out_size,
                     bool *exists, char *err, size_t errn);

#endif
