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
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
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
	check(strcmp(dir, want) == 0, "in the folder that holds it");
	check(!source_view_find_under(top, "hidden.s", 8, 1000, dir, sizeof dir),
	      "hidden folders (.git) are not searched");
	check(!source_view_find_under(top, "draw.s", 1, 1000, dir, sizeof dir),
	      "the depth limit is honoured (engine is two levels down)");
	check(!source_view_find_under(top, "nothing.s", 8, 1000, dir, sizeof dir), "a missing file is not found");

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
