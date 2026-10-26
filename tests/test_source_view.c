// Checks for source_view's folder management (src/source_view.c): adding and
// removing search folders, forgetting cached misses, and finding a file in a
// folder tree. Uses real folders under the system temp directory.

#include "source_view.h"
#include "support/harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#define remove_dir(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(p) mkdir(p, 0755)
#define remove_dir(p) rmdir(p)
#endif

static char root[512];

static void
path(char *out, size_t n, const char *rel)
{
	snprintf(out, n, "%s/%s", root, rel);
}

static void
mkd(const char *rel)
{
	char p[600];
	path(p, sizeof p, rel);
	make_dir(p);
}

static void
put(const char *rel, const char *text)
{
	char p[600];
	path(p, sizeof p, rel);
	FILE *f = fopen(p, "wb");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

// The same path, whichever separator each uses.
static bool
same_path(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		const char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
		if (x != y)
			return false;
	}
	return !*a && !*b;
}

int
main(void)
{
	const char *tmp = getenv("TEMP");
	if (!tmp)
		tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/tmp";
	snprintf(root, sizeof root, "%s/x16_srcview_test_%ld", tmp, (long)rand());
	make_dir(root);
	mkd("proj");
	mkd("proj/src");
	mkd("proj/src/engine");
	mkd("proj/.git");
	put("proj/.git/hidden.s", "; never found: hidden folders are skipped\n");
	put("proj/src/engine/Draw.S", "lda #1\nrts\n");

	// ── A missing file stays missing until a folder makes it findable ───────
	const source_file_t *f = source_view_get("build/obj/Draw.S");
	check(f && !f->found, "draw.s is not found before any folder is known");
	check(!source_view_can_find("build/obj/Draw.S"), "can_find agrees");

	char dir[600];
	char top[600];
	path(top, sizeof top, "proj");
	check(source_view_find_under(top, "build/obj/draw.s", 8, 1000, dir, sizeof dir),
	      "found under the project root, case-insensitively, from a .dbg-style path");
	char want[600];
	path(want, sizeof want, "proj/src/engine");
	check(same_path(dir, want), "in the folder that holds it");
	check(!source_view_find_under(top, "hidden.s", 8, 1000, dir, sizeof dir),
	      "hidden folders (.git) are not searched");
	check(!source_view_find_under(top, "draw.s", 1, 1000, dir, sizeof dir),
	      "the depth limit is honoured (engine is two levels down)");
	check(!source_view_find_under(top, "nothing.s", 8, 1000, dir, sizeof dir), "a missing file is not found");

	// ── Many files in one pass, a folder at a time ──────────────────────────
	{
		mkd("proj/lib");
		put("proj/lib/util.s", "rts\n");
		put("proj/main.s", "rts\n");
		const char *names[] = {"obj/Draw.S", "util.s", "main.s", "absent.s"};
		source_search_t *all = source_search_begin(top, names, 4, 8, 1000);
		while (source_search_step(all, 1000))
			;
		check(source_search_count(all) == 4 && source_search_found(all) == 3, "three of four found in one pass");
		char w2[600];
		path(w2, sizeof w2, "proj/lib");
		check(source_search_dir_of(all, 1) && same_path(source_search_dir_of(all, 1), w2), "util.s in lib");
		check(source_search_dir_of(all, 2) && same_path(source_search_dir_of(all, 2), top), "main.s at the top");
		check(source_search_dir_of(all, 3) == NULL, "absent.s is not");
		const int read_all = source_search_dirs_read(all);
		check(read_all == 4, "every folder read once: proj, src, lib, engine (.git skipped)");
		source_search_end(all);

		// A folder per step: the same answer, step by step.
		source_search_t *one = source_search_begin(top, names, 4, 8, 1000);
		int steps = 0;
		while (source_search_step(one, 1))
			steps++;
		check(source_search_found(one) == 3 && source_search_dirs_read(one) == read_all && steps >= 3,
		      "one folder at a time gets the same result");
		source_search_end(one);

		source_search_t *cap = source_search_begin(top, names, 4, 8, 2);
		while (source_search_step(cap, 1))
			;
		check(source_search_limited(cap) && source_search_dirs_read(cap) == 2, "stops at the folder limit, and says so");
		source_search_end(cap);

		// A stale copy beside the real sources: "aaa_copy" holds util.s too,
		// and is read before lib, but lib holds more of the names.
		mkd("proj/aaa_copy");
		put("proj/aaa_copy/util.s", "old\n");
		put("proj/lib/extra.s", "rts\n");
		const char *more[] = {"util.s", "extra.s"};
		source_search_t *pick = source_search_begin(top, more, 2, 8, 1000);
		while (source_search_step(pick, 1))
			;
		check(source_search_places(pick, 0) == 2, "util.s is in two folders");
		check(source_search_dir_of(pick, 0) && same_path(source_search_dir_of(pick, 0), w2),
		      "and goes to lib, which holds more of the names, not the copy read first");
		source_search_end(pick);
		char wc[600];
		path(wc, sizeof wc, "proj/aaa_copy");
		// Which of two folders at the same depth is read first is the
		// filesystem's order, so either is right here.
		check(source_view_find_under(top, "util.s", 8, 1000, dir, sizeof dir) &&
		          (same_path(dir, w2) || same_path(dir, wc)),
		      "one name alone stops at the first folder that has it");

		// A root with a trailing separator joins with one, not two.
		char slashed[620];
		snprintf(slashed, sizeof slashed, "%s/", top);
		check(source_view_find_under(slashed, "extra.s", 8, 1000, dir, sizeof dir) && same_path(dir, w2) &&
		          !strstr(dir, "//") && !strstr(dir, "\\/") && !strstr(dir, "/\\"),
		      "a trailing separator on the root is not doubled");
#ifdef _WIN32
		// A Windows root: every separator found is a backslash.
		char win[620];
		snprintf(win, sizeof win, "%s\\", top);
		for (char *c = win; *c; c++)
			if (*c == '/')
				*c = '\\';
		check(source_view_find_under(win, "extra.s", 8, 1000, dir, sizeof dir) && !strchr(dir, '/'),
		      "a backslash root gives backslashes only");
#endif
		const char *made[] = {"proj/aaa_copy/util.s", "proj/lib/extra.s", "proj/lib/util.s", "proj/main.s"};
		for (size_t m = 0; m < sizeof made / sizeof made[0]; m++) {
			char f2[600];
			path(f2, sizeof f2, made[m]);
			remove(f2);
		}
		char d2[600];
		path(d2, sizeof d2, "proj/aaa_copy");
		remove_dir(d2);
		path(d2, sizeof d2, "proj/lib");
		remove_dir(d2);
	}

	source_view_add_path(want);
	f = source_view_get("build/obj/Draw.S");
	check(f && !f->found, "the cached miss is still a miss");
	source_view_retry_missing();
	f = source_view_get("build/obj/Draw.S");
	check(f && f->found && f->count >= 2 && !strcmp(f->lines[0], "lda #1") && !strcmp(f->lines[1], "rts"),
	      "after retry_missing it is read from the added folder");
	check(source_view_can_find("build/obj/Draw.S"), "and can_find sees it too");
	check(!source_view_can_find("build/obj/Nope.S"), "but not a file that is nowhere");

	// ── Listing and removing folders ────────────────────────────────────────
	char other[600];
	path(other, sizeof other, "proj/src");
	source_view_add_path(other);
	check_eq((uint32_t)source_view_path_count(), 2, "two folders");
	check(strcmp(source_view_path_at(0), other) == 0, "most recently added first");
	check(source_view_remove_path(other), "removing a known folder");
	check(!source_view_remove_path(other), "removing it again does nothing");
	check_eq((uint32_t)source_view_path_count(), 1, "one folder left");
	check(source_view_path_at(1) == NULL, "out of range is NULL");

	source_view_free();
	return x16_test_summary("source_view");
}
