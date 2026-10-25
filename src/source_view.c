// Commander X16 Emulator — source file discovery + caching for the debugger's
// native "Source" window. See source_view.h.

#include "source_view.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dirent_win32.h" // portable opendir/readdir (native <dirent.h> elsewhere); needs <stdio.h> first

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

int source_view_path_count(void)
{
	return search_path_count;
}

const char *source_view_path_at(int i)
{
	return (i >= 0 && i < search_path_count) ? search_paths[i] : NULL;
}

bool source_view_remove_path(const char *dir)
{
	for (int i = 0; dir && i < search_path_count; i++) {
		if (strcmp(search_paths[i], dir) == 0) {
			free(search_paths[i]);
			for (int j = i + 1; j < search_path_count; j++)
				search_paths[j - 1] = search_paths[j];
			search_path_count--;
			return true;
		}
	}
	return false;
}

void source_view_retry_missing(void)
{
	int kept = 0;
	for (int i = 0; i < cache_count; i++) {
		if (cache[i].found) {
			cache[kept++] = cache[i];
		} else {
			free_entry(&cache[i]);
		}
	}
	cache_count = kept;
}

static bool file_exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fclose(f);
	return true;
}

bool source_view_can_find(const char *name)
{
	if (!name || !name[0])
		return false;
	if (file_exists(name))
		return true;
	const char *base = basename_ptr(name);
	for (int i = 0; i < search_path_count; i++) {
		size_t need = strlen(search_paths[i]) + 1 + strlen(base) + 1;
		char  *p    = malloc(need);
		if (!p)
			return false;
		snprintf(p, need, "%s/%s", search_paths[i], base);
		const bool ok = file_exists(p);
		free(p);
		if (ok)
			return true;
	}
	return file_exists(base);
}

static bool same_name(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	return !*a && !*b;
}

bool source_view_find_under(const char *root, const char *name, int max_depth, int max_dirs,
                            char *dir, int dir_size)
{
	if (!root || !root[0] || !name || !dir || dir_size <= 0)
		return false;
	const char *base = basename_ptr(name);
	// Breadth-first, so a file near the top wins over a same-named copy deep
	// in a vendored tree. The queue holds folders still to read, with depths.
	int    cap = 64, head = 0, tail = 0, visited = 0;
	char **queue = malloc(sizeof *queue * (size_t)cap);
	int   *depth = malloc(sizeof *depth * (size_t)cap);
	bool   found = false;
	if (!queue || !depth) {
		free(queue);
		free(depth);
		return false;
	}
	queue[tail] = malloc(strlen(root) + 1);
	if (queue[tail])
		strcpy(queue[tail], root);
	depth[tail++] = 0;
	while (head < tail && !found) {
		char     *cur = queue[head];
		const int d   = depth[head++];
		if (!cur)
			continue;
		DIR *dp = opendir(cur);
		if (dp && ++visited <= max_dirs) {
			struct dirent *de;
			while (!found && (de = readdir(dp)) != NULL) {
				const char *n = de->d_name;
				if (n[0] == '.')
					continue; // ".", "..", and hidden folders such as .git
				size_t need  = strlen(cur) + 1 + strlen(n) + 1;
				char  *child = malloc(need);
				if (!child)
					continue;
				snprintf(child, need, "%s/%s", cur, n);
				if (same_name(n, base)) {
					FILE *f = fopen(child, "rb");
					if (f) {
						fclose(f);
						snprintf(dir, (size_t)dir_size, "%s", cur);
						found = true;
					}
					free(child);
					continue;
				}
				// A folder? Try to open it; only folders open. Queue it.
				DIR *sub = d < max_depth ? opendir(child) : NULL;
				if (sub) {
					closedir(sub);
					if (tail == cap) {
						cap *= 2;
						char **q2 = realloc(queue, sizeof *queue * (size_t)cap);
						int   *d2 = realloc(depth, sizeof *depth * (size_t)cap);
						if (q2)
							queue = q2;
						if (d2)
							depth = d2;
						if (!q2 || !d2) {
							free(child);
							cap /= 2;
							continue;
						}
					}
					queue[tail]   = child;
					depth[tail++] = d + 1;
				} else {
					free(child);
				}
			}
		}
		if (dp)
			closedir(dp);
		if (visited > max_dirs)
			break;
	}
	for (int i = 0; i < tail; i++)
		free(queue[i]);
	free(queue);
	free(depth);
	return found;
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
