// Commander X16 Emulator — source file discovery + caching for the debugger's
// native "Source" window. See source_view.h.

#include "source_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── Search paths ───────────────────────────────────────────────────

#define MAX_SEARCH_PATHS 32
static char *search_paths[MAX_SEARCH_PATHS];
static int   search_path_count = 0;

void source_view_add_path(const char *dir)
{
	if (!dir || !dir[0])
		return;
	// Already known: promote it rather than ignoring it. The insertion below
	// only means "most recently added is searched first" if re-adding a path
	// also counts as recent. Without this, loading A then B then A again leaves
	// B's directory ahead of A's, and two modules naming the same relative
	// source file would both resolve to B's copy.
	for (int i = 0; i < search_path_count; i++) {
		if (strcmp(search_paths[i], dir) == 0) {
			char *existing = search_paths[i];
			for (int j = i; j > 0; j--)
				search_paths[j] = search_paths[j - 1];
			search_paths[0] = existing;
			return;
		}
	}
	if (search_path_count >= MAX_SEARCH_PATHS)
		return;

	char *copy = malloc(strlen(dir) + 1);
	if (!copy)
		return;
	strcpy(copy, dir);

	// Insert at the front so the most-recently-added path is searched first.
	for (int i = search_path_count; i > 0; i--)
		search_paths[i] = search_paths[i - 1];
	search_paths[0] = copy;
	search_path_count++;
}

// ─── File cache ─────────────────────────────────────────────────────

#define MAX_CACHED_FILES 8
static source_file_t cache[MAX_CACHED_FILES];
static int           cache_count = 0;

static const char *basename_ptr(const char *path)
{
	const char *last = path;
	for (const char *p = path; *p; p++) {
		if (*p == '/' || *p == '\\')
			last = p + 1;
	}
	return last;
}

// Read an entire file into a NUL-terminated heap buffer. Returns NULL on failure.
static char *read_whole_file(const char *path)
{
	FILE *f;
#ifdef _WIN32
	if (fopen_s(&f, path, "rb") != 0 || !f)
		return NULL;
#else
	f = fopen(path, "rb");
	if (!f)
		return NULL;
#endif
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long size = ftell(f);
	if (size < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);

	char *buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

// Try to open 'name' at 'dir/basename'. Returns a heap buffer or NULL.
static char *try_read_in_dir(const char *dir, const char *base)
{
	size_t need = strlen(dir) + 1 + strlen(base) + 1;
	char *path = malloc(need);
	if (!path)
		return NULL;
	snprintf(path, need, "%s/%s", dir, base);
	char *buf = read_whole_file(path);
	free(path);
	return buf;
}

// Split a NUL-terminated buffer into lines in place (rewrites \r\n / \n to NULs).
// Fills entry->lines / entry->count.
static void split_lines(source_file_t *entry)
{
	// First pass: count lines.
	int lines = 1;
	for (char *p = entry->text; *p; p++) {
		if (*p == '\n')
			lines++;
	}

	entry->lines = malloc((size_t)lines * sizeof(char *));
	if (!entry->lines) {
		entry->count = 0;
		return;
	}

	int idx = 0;
	char *start = entry->text;
	for (char *p = entry->text; ; p++) {
		if (*p == '\n' || *p == '\0') {
			// Trim a trailing CR for CRLF files.
			char *nl = p;
			if (nl > start && nl[-1] == '\r')
				nl[-1] = '\0';
			char terminator = *p;
			*p = '\0';
			if (idx < lines)
				entry->lines[idx++] = start;
			if (terminator == '\0')
				break;
			start = p + 1;
		}
	}
	entry->count = idx;
}

static void free_entry(source_file_t *entry)
{
	free(entry->name);
	free(entry->text);
	free(entry->lines);
	entry->name = NULL;
	entry->text = NULL;
	entry->lines = NULL;
	entry->count = 0;
	entry->found = false;
}

// Locate and read a source file by the name stored in the .dbg.
static void load_entry(source_file_t *entry, const char *name)
{
	entry->name = malloc(strlen(name) + 1);
	if (entry->name)
		strcpy(entry->name, name);
	entry->text = NULL;
	entry->lines = NULL;
	entry->count = 0;
	entry->found = false;

	char *buf = NULL;

	// 1) The path stored in the .dbg, as-is.
	buf = read_whole_file(name);

	// 2) Each registered search root + the file's basename.
	if (!buf) {
		const char *base = basename_ptr(name);
		for (int i = 0; i < search_path_count && !buf; i++)
			buf = try_read_in_dir(search_paths[i], base);
		// 3) Current working directory (basename only).
		if (!buf)
			buf = read_whole_file(base);
	}

	if (buf) {
		entry->text = buf;
		entry->found = true;
		split_lines(entry);
	}
}

const source_file_t *source_view_get(const char *name)
{
	if (!name || !name[0])
		return NULL;

	// Already cached?
	for (int i = 0; i < cache_count; i++) {
		if (cache[i].name && strcmp(cache[i].name, name) == 0)
			return &cache[i];
	}

	// Evict the oldest entry if the cache is full (simple FIFO).
	if (cache_count >= MAX_CACHED_FILES) {
		free_entry(&cache[0]);
		for (int i = 1; i < cache_count; i++)
			cache[i - 1] = cache[i];
		cache_count--;
	}

	source_file_t *entry = &cache[cache_count++];
	memset(entry, 0, sizeof(*entry));
	load_entry(entry, name);
	return entry;
}

void source_view_invalidate(void)
{
	for (int i = 0; i < cache_count; i++)
		free_entry(&cache[i]);
	cache_count = 0;
}

void source_view_free(void)
{
	source_view_invalidate();
	for (int i = 0; i < search_path_count; i++)
		free(search_paths[i]);
	search_path_count = 0;
}
