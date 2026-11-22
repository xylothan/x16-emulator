// Coalescing of memory ranges and diffing of loaded sources for DAP refresh events.
// Event bodies: DAP specification, MemoryEvent and LoadedSourceEvent.

#include "support/harness.h"

#include "../src/dap_refresh.h"

#include <string.h>

#define SPACE 0x10000u

static void
test_ranges(void)
{
	refresh_ranges_t s;
	refresh_range_t  r;
	refresh_ranges_clear(&s);

	check(!refresh_ranges_take(&s, &r), "empty set yields nothing");
	refresh_ranges_add(&s, 0x100, 0, SPACE);
	check_eq((uint32_t)s.n, 0, "zero count adds nothing");

	refresh_ranges_add(&s, 0x200, 0x10, SPACE);
	refresh_ranges_add(&s, 0x210, 0x10, SPACE);
	refresh_ranges_add(&s, 0x208, 0x04, SPACE);
	check_eq((uint32_t)s.n, 1, "touching and overlapping merge");
	check(refresh_ranges_take(&s, &r), "merged range comes out");
	check_eq(r.start, 0x200, "merged start");
	check_eq(r.count, 0x20, "merged count");

	refresh_ranges_add(&s, 0x300, 1, SPACE);
	refresh_ranges_add(&s, 0x100, 1, SPACE);
	refresh_ranges_take(&s, &r);
	check_eq(r.start, 0x100, "lowest first");
	refresh_ranges_take(&s, &r);
	check_eq(r.start, 0x300, "then the next");

	refresh_ranges_add(&s, 0xFFFE, 4, SPACE);
	check_eq((uint32_t)s.n, 2, "a wrapping write splits");
	refresh_ranges_take(&s, &r);
	check_eq(r.start, 0, "wrapped part start");
	check_eq(r.count, 2, "wrapped part count");
	refresh_ranges_take(&s, &r);
	check_eq(r.start, 0xFFFE, "tail start");
	check_eq(r.count, 2, "tail count");

	refresh_ranges_add(&s, 5, SPACE + 7, SPACE);
	check_eq((uint32_t)s.n, 1, "a write of the whole space is one range");
	refresh_ranges_take(&s, &r);
	check_eq(r.start, 0, "whole space start");
	check_eq(r.count, SPACE, "whole space count");

	for (uint32_t i = 0; i < 4096; i++) {
		refresh_ranges_add(&s, i * 16, 1, SPACE);
		check(s.n <= REFRESH_MAX_RANGES, "bounded");
	}
	uint32_t covered_end = 0;
	bool     sorted      = true;
	while (refresh_ranges_take(&s, &r)) {
		sorted = sorted && r.start >= covered_end;
		covered_end = r.start + r.count;
	}
	check(sorted, "ranges stay sorted and disjoint");
	check_eq(covered_end, 4095 * 16 + 1, "the last write is covered");

	refresh_ranges_clear(&s);
	for (uint32_t i = 0; i < REFRESH_MAX_RANGES; i++) {
		refresh_ranges_add(&s, i * 0x100, 1, SPACE);
	}
	refresh_ranges_add(&s, 0x101, 1, SPACE);
	check_eq((uint32_t)s.n, REFRESH_MAX_RANGES, "a touching write at the limit merges");
	refresh_ranges_add(&s, 0x8000, 1, SPACE);
	check_eq((uint32_t)s.n, REFRESH_MAX_RANGES, "a ninth range merges the closest pair");
	bool has_far = false, has_first = false;
	while (refresh_ranges_take(&s, &r)) {
		has_far   = has_far || (r.start <= 0x8000 && 0x8000 < r.start + r.count);
		has_first = has_first || r.start == 0;
	}
	check(has_far && has_first, "both ends stay covered");
}

static const refresh_source_change_t *
find(const refresh_source_change_t *c, int n, const char *path)
{
	for (int i = 0; i < n; i++) {
		if (!strcmp(c[i].path, path)) {
			return &c[i];
		}
	}
	return NULL;
}

static void
test_sources(void)
{
	refresh_sources_t       s = { 0 };
	refresh_source_change_t c[8];
	bool                    more;

	refresh_source_t base[] = { { "b.c", 1, false }, { "a.c", 1, false } };
	refresh_sources_baseline(&s, base, 2);
	refresh_source_t same[] = { { "a.c", 1, false }, { "b.c", 1, false } };
	check_eq((uint32_t)refresh_sources_diff(&s, same, 2, c, 8, &more), 0, "baseline reports nothing");
	check(!more, "nothing left over");

	refresh_source_t next[] = {
		{ "b.c", 2, false }, { "c.c", 2, false }, { "x.lbl", 3, true },
	};
	int n = refresh_sources_diff(&s, next, 3, c, 8, &more);
	check_eq((uint32_t)n, 4, "one removed, one changed, two new");
	const refresh_source_change_t *e = find(c, n, "a.c");
	check(e && e->reason == REFRESH_SOURCE_REMOVED, "a.c removed");
	e = find(c, n, "b.c");
	check(e && e->reason == REFRESH_SOURCE_CHANGED, "b.c changed");
	e = find(c, n, "c.c");
	check(e && e->reason == REFRESH_SOURCE_NEW && !e->label, "c.c new");
	e = find(c, n, "x.lbl");
	check(e && e->reason == REFRESH_SOURCE_NEW && e->label, "label file new, marked");
	refresh_source_changes_free(c, n);

	refresh_source_t again[] = {
		{ "b.c", 2, false }, { "c.c", 2, false }, { "x.lbl", 3, true },
	};
	check_eq((uint32_t)refresh_sources_diff(&s, again, 3, c, 8, &more), 0, "reported once");

	refresh_source_t dup[] = {
		{ "b.c", 2, false }, { "b.c", 9, false }, { "c.c", 2, false }, { "x.lbl", 3, true },
	};
	n = refresh_sources_diff(&s, dup, 4, c, 8, &more);
	check_eq((uint32_t)n, 1, "one path is one source");
	check(c[0].reason == REFRESH_SOURCE_CHANGED && !strcmp(c[0].path, "b.c"), "newest revision wins");
	refresh_source_changes_free(c, n);

	refresh_source_t many[] = {
		{ "d.c", 1, false }, { "e.c", 1, false }, { "f.c", 1, false }, { "g.c", 1, false },
	};
	n = refresh_sources_diff(&s, many, 4, c, 2, &more);
	check_eq((uint32_t)n, 2, "stops at the limit");
	check(more, "and says more is left");
	refresh_source_changes_free(c, n);
	int total = 2;
	for (int guard = 0; more && guard < 8; guard++) {
		refresh_source_t rest[] = {
			{ "d.c", 1, false }, { "e.c", 1, false }, { "f.c", 1, false }, { "g.c", 1, false },
		};
		n = refresh_sources_diff(&s, rest, 4, c, 2, &more);
		total += n;
		refresh_source_changes_free(c, n);
	}
	check_eq((uint32_t)total, 7, "every change arrives across calls");
	check(!more, "and the walk ends");

	check_eq((uint32_t)refresh_sources_diff(&s, NULL, 0, c, 8, &more), 4, "all removed");
	refresh_source_changes_free(c, 4);
	check_eq((uint32_t)s.n, 0, "nothing known");
	refresh_sources_free(&s);
}

int
main(void)
{
	test_ranges();
	test_sources();
	return x16_test_summary("dap_refresh");
}
