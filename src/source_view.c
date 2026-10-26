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

struct source_search {
	char   sep;            // the separator found folders are written with
	char **names;          // base names
	int    count, nfound;  // names asked for; of which found somewhere
	char **queue;          // folders still to read, breadth-first
	int   *depth;
	int    head, tail, cap;
	int    max_depth, max_dirs, read;
	bool   limited;
	// Every folder any of the names turned up in, with how many of them it
	// holds. A project usually has stale copies (build output, old work
	// folders) beside its real sources; the real ones are where most of the
	// files are, so each name goes to the folder with the most matches.
	char **dirs;
	int   *hits;
	int    ndirs, dircap;
	int  **cand;           // per name: indices into dirs, first found first
	int   *ncand, *capcand;
	int   *best;           // per name, once done: its dirs index, or -1
	bool   decided;
};

// `a` and `b` joined with one separator.
static char *
join_path(const char *a, const char *b, char sep)
{
	const size_t la   = strlen(a);
	const bool   ends = la && (a[la - 1] == '/' || a[la - 1] == '\\');
	const size_t need = la + 1 + strlen(b) + 1;
	char        *p    = malloc(need);
	if (!p)
		return NULL;
	if (ends)
		snprintf(p, need, "%s%s", a, b);
	else
		snprintf(p, need, "%s%c%s", a, sep, b);
	return p;
}

static bool
queue_push(source_search_t *s, char *dir, int depth)
{
	if (s->tail == s->cap) {
		const int cap = s->cap ? s->cap * 2 : 64;
		char    **q   = realloc(s->queue, sizeof *q * (size_t)cap);
		if (!q)
			return false;
		s->queue = q;
		int *d   = realloc(s->depth, sizeof *d * (size_t)cap);
		if (!d)
			return false;
		s->depth = d;
		s->cap   = cap;
	}
	s->queue[s->tail]   = dir;
	s->depth[s->tail++] = depth;
	return true;
}

source_search_t *
source_search_begin(const char *root, const char *const *names, int count, int max_depth, int max_dirs)
{
	if (!root || !root[0] || count < 0)
		return NULL;
	source_search_t *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	s->max_depth = max_depth;
	s->max_dirs  = max_dirs;
	// One separator style: the root's. A root written with a backslash is a
	// Windows path, so its forward slashes become backslashes too.
	char *r = malloc(strlen(root) + 1);
	if (!r) {
		free(s);
		return NULL;
	}
	strcpy(r, root);
	s->sep = strchr(r, '\\') ? '\\' : '/';
	if (s->sep == '\\')
		for (char *c = r; *c; c++)
			if (*c == '/')
				*c = '\\';
	// No trailing separator (but keep "/" and "C:\" whole).
	size_t n = strlen(r);
	while (n > 1 && (r[n - 1] == '/' || r[n - 1] == '\\') && r[n - 2] != ':')
		r[--n] = '\0';
	const size_t nn = (size_t)(count ? count : 1);
	s->names        = calloc(nn, sizeof *s->names);
	s->cand         = calloc(nn, sizeof *s->cand);
	s->ncand        = calloc(nn, sizeof *s->ncand);
	s->capcand      = calloc(nn, sizeof *s->capcand);
	s->best         = calloc(nn, sizeof *s->best);
	if (!s->names || !s->cand || !s->ncand || !s->capcand || !s->best || !queue_push(s, r, 0)) {
		free(r);
		source_search_end(s);
		return NULL;
	}
	for (int k = 0; k < count; k++) {
		const char *b = basename_ptr(names[k]);
		s->names[k]   = malloc(strlen(b) + 1);
		if (s->names[k])
			strcpy(s->names[k], b);
		s->count = k + 1;
	}
	return s;
}

// Name `k` is in folder `cur`, whose dirs index is *at (-1 until its first
// match is recorded).
static void
note_match(source_search_t *s, int k, const char *cur, int *at)
{
	if (*at < 0) {
		if (s->ndirs == s->dircap) {
			const int cap = s->dircap ? s->dircap * 2 : 16;
			char    **d   = realloc(s->dirs, sizeof *d * (size_t)cap);
			if (!d)
				return;
			s->dirs = d;
			int *h  = realloc(s->hits, sizeof *h * (size_t)cap);
			if (!h)
				return;
			s->hits   = h;
			s->dircap = cap;
		}
		char *copy = malloc(strlen(cur) + 1);
		if (!copy)
			return;
		strcpy(copy, cur);
		s->dirs[s->ndirs] = copy;
		s->hits[s->ndirs] = 0;
		*at               = s->ndirs++;
	}
	if (s->ncand[k] == s->capcand[k]) {
		const int cap = s->capcand[k] ? s->capcand[k] * 2 : 2;
		int      *c   = realloc(s->cand[k], sizeof *c * (size_t)cap);
		if (!c)
			return;
		s->cand[k]    = c;
		s->capcand[k] = cap;
	}
	if (s->ncand[k] == 0)
		s->nfound++;
	s->cand[k][s->ncand[k]++] = *at;
	s->hits[*at]++;
}

// Each name to the folder holding the most of the names; ties to the one
// found first (the shallowest).
static void
decide(source_search_t *s)
{
	if (s->decided)
		return;
	s->decided = true;
	for (int k = 0; k < s->count; k++) {
		int b = -1;
		for (int c = 0; c < s->ncand[k]; c++)
			if (b < 0 || s->hits[s->cand[k][c]] > s->hits[b])
				b = s->cand[k][c];
		s->best[k] = b;
	}
}

// Nothing left to do: every folder read, the limit reached, or -- for one
// name, where there is nothing to compare -- it was found.
static bool
search_done(const source_search_t *s)
{
	return s->head >= s->tail || s->limited || (s->count == 1 && s->nfound == 1) || s->count == 0;
}

bool
source_search_step(source_search_t *s, int dirs)
{
	if (!s)
		return false;
	while (dirs-- > 0) {
		if (search_done(s)) {
			decide(s);
			return false;
		}
		char     *cur = s->queue[s->head];
		const int d   = s->depth[s->head];
		s->queue[s->head++] = NULL;
		if (s->read >= s->max_dirs) {
			s->limited = true;
			free(cur);
			decide(s);
			return false;
		}
		DIR *dp = opendir(cur);
		if (dp) {
			s->read++;
			int            at = -1; // this folder's dirs index, once it has a match
			struct dirent *de;
			while ((de = readdir(dp)) != NULL) {
				const char *nm = de->d_name;
				if (nm[0] == '.')
					continue; // ".", "..", and hidden folders such as .git
				// Folder or file? d_type says, when the filesystem fills it in;
				// otherwise only a folder opens.
				bool is_dir;
#ifdef DT_DIR
				if (de->d_type == DT_DIR || de->d_type == DT_REG) {
					is_dir = de->d_type == DT_DIR;
				} else
#endif
				{
					char *probe = join_path(cur, nm, s->sep);
					DIR  *sub   = probe ? opendir(probe) : NULL;
					is_dir      = sub != NULL;
					if (sub)
						closedir(sub);
					free(probe);
				}
				if (is_dir) {
					if (d < s->max_depth) {
						char *child = join_path(cur, nm, s->sep);
						if (child && !queue_push(s, child, d + 1))
							free(child);
					}
					continue;
				}
				for (int k = 0; k < s->count; k++)
					if (s->names[k] && same_name(nm, s->names[k]))
						note_match(s, k, cur, &at);
			}
			closedir(dp);
		}
		free(cur);
	}
	if (search_done(s)) {
		decide(s);
		return false;
	}
	return true;
}

int         source_search_dirs_read(const source_search_t *s) { return s ? s->read : 0; }
int         source_search_count(const source_search_t *s)     { return s ? s->count : 0; }
int         source_search_found(const source_search_t *s)     { return s ? s->nfound : 0; }
bool        source_search_limited(const source_search_t *s)   { return s && s->limited; }

const char *
source_search_dir_of(const source_search_t *s, int i)
{
	if (!s || i < 0 || i >= s->count || !s->decided || s->best[i] < 0)
		return NULL;
	return s->dirs[s->best[i]];
}

int
source_search_places(const source_search_t *s, int i)
{
	return (s && i >= 0 && i < s->count) ? s->ncand[i] : 0;
}

void
source_search_end(source_search_t *s)
{
	if (!s)
		return;
	for (int k = 0; k < s->count; k++) {
		free(s->names[k]);
		free(s->cand ? s->cand[k] : NULL);
	}
	for (int k = 0; k < s->ndirs; k++)
		free(s->dirs[k]);
	for (int k = s->head; k < s->tail; k++)
		free(s->queue[k]);
	free(s->names);
	free(s->cand);
	free(s->ncand);
	free(s->capcand);
	free(s->best);
	free(s->dirs);
	free(s->hits);
	free(s->queue);
	free(s->depth);
	free(s);
}

bool source_view_find_under(const char *root, const char *name, int max_depth, int max_dirs,
                            char *dir, int dir_size)
{
	if (!root || !root[0] || !name || !dir || dir_size <= 0)
		return false;
	source_search_t *s = source_search_begin(root, &name, 1, max_depth, max_dirs);
	while (source_search_step(s, 64))
		;
	const char *at = source_search_dir_of(s, 0);
	if (at)
		snprintf(dir, (size_t)dir_size, "%s", at);
	source_search_end(s);
	return at != NULL;
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
