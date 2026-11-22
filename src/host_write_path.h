// Commander X16 Emulator — where a debugger client may read or write a host file.
//
// A client names the file, so the name cannot be trusted to stay anywhere in
// particular. Everything is resolved by the operating system before it is
// compared: `..`, links and letter case are its business, not a string match's.
#ifndef HOST_WRITE_PATH_H
#define HOST_WRITE_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// `path` as the process would open it: a relative one joined to the working
// folder, since host_read_path() would otherwise take it from its first root.
// Nothing is opened. False if it does not fit.
bool host_path_from_cwd(const char *path, char *out, size_t n);

// Resolve `request`, relative to `root` unless absolute, into `out`. Its folder
// must exist and lie inside `root`, which must exist and not be a drive root.
// The file itself may not be a link. `*exists` says whether it is already there.
// On failure, `err` says why and nothing has been touched.
bool host_write_path(const char *root, const char *request, char *out, size_t out_size,
                     bool *exists, char *err, size_t errn);

// Create the file host_write_path() resolved, for writing in binary. Fails if
// anything is at that path by now, a link included, so that nothing can be put
// there between the check and the open. NULL on failure.
FILE *host_create_new(const char *path);

// Resolve `request`, relative to the first usable root unless absolute, into
// `out`: an existing regular file that, with every link followed, lies inside
// one of `roots`. NULL roots, missing folders and drive roots are skipped. A file
// that is missing and one that is out of bounds get the same answer, so that this
// cannot be used to find out what exists elsewhere.
bool host_read_path(const char *const *roots, int root_count, const char *request, char *out,
                    size_t out_size, char *err, size_t errn);

// Is anything at exactly this path, a dangling link included?
bool host_path_present(const char *path);

#endif
