// Commander X16 Emulator — where files the emulator writes on its own go.
//
// Screenshots, memory dumps and profile exports are named by the emulator, not
// by the user, and used to land in whatever the working directory happened to
// be. Each kind now has a folder (empty: the working directory, as before),
// set from the debugger's Settings window and read here by the C code that
// writes the files. A name that already says where it goes -- an absolute
// path, or one with a folder in it -- is used as it is.
#ifndef EXPORT_PATHS_H
#define EXPORT_PATHS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	EXPORT_PROFILE = 0, // Profile panel: .folded, .json, .trace.json
	EXPORT_SCREENSHOT,  // Ctrl+P
	EXPORT_DUMP,        // Ctrl+S, and the dump made when the PC reaches $FFFF
	EXPORT_KINDS
} export_kind_t;

// The folder for `kind`; "" means the working directory.
void        export_set_dir(export_kind_t kind, const char *dir);
const char *export_dir(export_kind_t kind);

// Where a file called `name` of this kind goes, into `out`. A bare name goes in
// the kind's folder, which is created if it is missing; anything with a folder
// in it is left alone. False if the result does not fit, or the folder cannot
// be created (`out` then holds the path that was wanted).
bool export_path(export_kind_t kind, const char *name, char *out, size_t out_size);

// Does `name` say where it goes (absolute, or containing a folder)?
bool export_name_has_folder(const char *name);

// Create `dir` and any missing parents. True if it exists afterwards.
bool export_make_dir(const char *dir);
bool export_dir_exists(const char *dir);

#ifdef __cplusplus
}
#endif

#endif
