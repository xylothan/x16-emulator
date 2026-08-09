// Commander X16 Emulator — source file discovery + caching for the debugger's
// native "Source" window. Locates the real .s/.c files referenced (by name only)
// in cc65 .dbg info and caches their contents split into lines.

#ifndef _SOURCE_VIEW_H_
#define _SOURCE_VIEW_H_

#include <stdbool.h>

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

// Free everything (cached files and registered paths). Call on shutdown.
void source_view_free(void);

#endif
