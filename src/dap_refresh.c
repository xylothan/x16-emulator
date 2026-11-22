// Commander X16 Emulator — what an attached editor has yet to be told.

#include "dap_refresh.h"

#include <stdlib.h>
#include <string.h>

// ---- Memory ranges ----------------------------------------------------------

void
refresh_ranges_clear(refresh_ranges_t *s)
{
	s->n = 0;
}

static void
merge_closest(refresh_ranges_t *s)
{
	int      best = 0;
	uint32_t gap  = UINT32_MAX;
	for (int i = 0; i + 1 < s->n; i++) {
		const uint32_t g = s->r[i + 1].start - (s->r[i].start + s->r[i].count);
		if (g < gap) {
			gap  = g;
			best = i;
		}
	}
	s->r[best].count = s->r[best + 1].start + s->r[best + 1].count - s->r[best].start;
	memmove(&s->r[best + 1], &s->r[best + 2], (size_t)(s->n - best - 2) * sizeof s->r[0]);
	s->n--;
}

// [start, end), with end past the last byte.
static void
add_span(refresh_ranges_t *s, uint32_t start, uint32_t end)
{
	int i = 0;
	while (i < s->n && s->r[i].start + s->r[i].count < start) {
		i++;
	}
	int j = i;
	while (j < s->n && s->r[j].start <= end) {
		const uint32_t e = s->r[j].start + s->r[j].count;
		if (s->r[j].start < start) {
			start = s->r[j].start;
		}
		if (e > end) {
			end = e;
		}
		j++;
	}
	memmove(&s->r[i + 1], &s->r[j], (size_t)(s->n - j) * sizeof s->r[0]);
	s->r[i].start = start;
	s->r[i].count = end - start;
	s->n += 1 - (j - i);
	if (s->n > REFRESH_MAX_RANGES) {
		merge_closest(s);
	}
}

void
refresh_ranges_add(refresh_ranges_t *s, uint32_t start, uint32_t count, uint32_t space)
{
	if (!count || !space) {
		return;
	}
	if (count >= space) {
		s->n = 0;
		add_span(s, 0, space);
		return;
	}
	start %= space;
	if (start + count > space) {
		add_span(s, start, space);
		add_span(s, 0, start + count - space);
	} else {
		add_span(s, start, start + count);
	}
}

bool
refresh_ranges_take(refresh_ranges_t *s, refresh_range_t *out)
{
	if (s->n == 0) {
		return false;
	}
	*out = s->r[0];
	memmove(&s->r[0], &s->r[1], (size_t)(s->n - 1) * sizeof s->r[0]);
	s->n--;
	return true;
}

// ---- Loaded sources ---------------------------------------------------------

struct refresh_seen {
	char    *path;
	unsigned revision;
	bool     label;
};

static char *
copy_text(const char *text)
{
	const size_t n    = strlen(text) + 1;
	char        *copy = (char *)malloc(n);
	if (copy) {
		memcpy(copy, text, n);
	}
	return copy;
}

static int
source_cmp(const void *a, const void *b)
{
	return strcmp(((const refresh_source_t *)a)->path, ((const refresh_source_t *)b)->path);
}

// Sorted, one entry per path. Two modules can name one file, and it is one source
// to a client; it counts as re-read when either was.
static int
normalise(refresh_source_t *cur, int n)
{
	if (n <= 0) {
		return 0;
	}
	qsort(cur, (size_t)n, sizeof *cur, source_cmp);
	int kept = 1;
	for (int i = 1; i < n; i++) {
		if (!strcmp(cur[kept - 1].path, cur[i].path)) {
			if (cur[i].revision > cur[kept - 1].revision) {
				cur[kept - 1].revision = cur[i].revision;
			}
		} else {
			cur[kept++] = cur[i];
		}
	}
	return kept;
}

void
refresh_sources_free(refresh_sources_t *s)
{
	for (int i = 0; i < s->n; i++) {
		free(s->v[i].path);
	}
	free(s->v);
	s->v = NULL;
	s->n = 0;
}

void
refresh_sources_baseline(refresh_sources_t *s, refresh_source_t *cur, int n)
{
	refresh_sources_free(s);
	n = normalise(cur, n);
	if (n == 0) {
		return;
	}
	s->v = (refresh_seen_t *)malloc((size_t)n * sizeof *s->v);
	if (!s->v) {
		return;
	}
	for (int i = 0; i < n; i++) {
		char *copy = copy_text(cur[i].path);
		if (!copy) {
			continue;
		}
		s->v[s->n].path     = copy;
		s->v[s->n].revision = cur[i].revision;
		s->v[s->n].label    = cur[i].label;
		s->n++;
	}
}

static bool
report(refresh_source_change_t *out, refresh_source_reason_t reason, const char *path, bool label)
{
	out->path = copy_text(path);
	if (!out->path) {
		return false;
	}
	out->reason = reason;
	out->label  = label;
	return true;
}

int
refresh_sources_diff(refresh_sources_t *s, refresh_source_t *cur, int n,
                     refresh_source_change_t *out, int max, bool *more)
{
	*more = false;
	n = normalise(cur, n);
	const size_t cap = (size_t)s->n + (size_t)n;
	if (cap == 0) {
		return 0;
	}
	refresh_seen_t *next = (refresh_seen_t *)malloc(cap * sizeof *next);
	if (!next) {
		*more = true;
		return 0;
	}

	int a = 0, b = 0, kept = 0, written = 0;
	while (a < s->n || b < n) {
		const int c = a >= s->n ? 1 : b >= n ? -1 : strcmp(s->v[a].path, cur[b].path);
		if (c < 0) {
			if (written < max &&
			    report(&out[written], REFRESH_SOURCE_REMOVED, s->v[a].path, s->v[a].label)) {
				written++;
				free(s->v[a].path);
			} else {
				next[kept++] = s->v[a];
				*more = true;
			}
			a++;
		} else if (c > 0) {
			char *copy = written < max ? copy_text(cur[b].path) : NULL;
			if (copy && report(&out[written], REFRESH_SOURCE_NEW, cur[b].path, cur[b].label)) {
				written++;
				next[kept].path     = copy;
				next[kept].revision = cur[b].revision;
				next[kept].label    = cur[b].label;
				kept++;
			} else {
				free(copy);
				*more = true;
			}
			b++;
		} else {
			next[kept] = s->v[a];
			if (s->v[a].revision != cur[b].revision) {
				if (written < max &&
				    report(&out[written], REFRESH_SOURCE_CHANGED, cur[b].path, cur[b].label)) {
					written++;
					next[kept].revision = cur[b].revision;
					next[kept].label    = cur[b].label;
				} else {
					*more = true;
				}
			}
			kept++;
			a++;
			b++;
		}
	}
	free(s->v);
	s->v = next;
	s->n = kept;
	return written;
}

void
refresh_source_changes_free(refresh_source_change_t *c, int n)
{
	for (int i = 0; i < n; i++) {
		free(c[i].path);
		c[i].path = NULL;
	}
}
