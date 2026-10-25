// Commander X16 Emulator — source file discovery + caching for the debugger's
// native "Source" window. Locates the real .s/.c files referenced (by name only)
// in cc65 .dbg info and caches their contents split into lines.

#ifndef _SOURCE_VIEW_H_
#define _SOURCE_VIEW_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// A loaded source file, split into individual lines.
typedef struct {
	char  *name;      // the file name as referenced by the .dbg (key)
	char  *text;      // owned backing buffer (lines point into this)
	char **lines;     // array of 'count' pointers into 'text'
	int    count;     // number of lines
	bool   found;     // false = file could not be located on disk
} source_file_t;

// Register a directory to search for source files. Duplicates are ignored;
// most-recently-added roots are searched first. Safe to pass NULL/empty.
void source_view_add_path(const char *dir);

// Look up (and cache) a source file by the name stored in the .dbg. Resolution
// order: the name as-is, then each registered search root (most recent first),
// then the current working directory. Returns a cached entry (never freed by the
// caller). The returned entry is always non-NULL while a lookup slot is available;
// check ->found to know whether the file was actually located.
const source_file_t *source_view_get(const char *name);

// Drop all cached file contents (call when loaded modules change, so overlays
// re-read fresh). Registered search paths are kept.
void source_view_invalidate(void);

// ---- Managing the search folders from the debugger ------------------------
// The registered folders, most recently added first.
int         source_view_path_count(void);
const char *source_view_path_at(int i);
// Forget a folder. False if it was not registered.
bool        source_view_remove_path(const char *dir);

// Forget every cached "not found", so the next lookup searches again: call
// after adding a folder. Files that were found stay cached.
void source_view_retry_missing(void);

// Whether `name` would be found, by the same rules as source_view_get(), without
// reading or caching it. The cache holds only a few files, so a caller that
// checks many (a list of every file in the debug info) must use this instead.
bool source_view_can_find(const char *name);

// Look for a file named like `name` (its last path component, compared without
// regard to case) anywhere under `root`, and write the folder that holds it to
// `dir`. Searches breadth-first, at most `max_depth` folders deep and `max_dirs`
// folders in all, and skips hidden folders (.git and friends). For a debugger
// UI that must not hang on a huge tree. True if found.
bool source_view_find_under(const char *root, const char *name, int max_depth, int max_dirs,
                            char *dir, int dir_size);

// Free everything (cached files and registered paths). Call on shutdown.
void source_view_free(void);

#ifdef __cplusplus
}
#endif

#endif
