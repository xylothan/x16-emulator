// Checks for where the emulator's own files go (src/export_paths.c).

#include "export_paths.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

int
main(void)
{
	char out[256];

	// ── No folder: the working directory, as before ─────────────────────────
	export_set_dir(EXPORT_SCREENSHOT, "");
	check(export_path(EXPORT_SCREENSHOT, "shot.png", out, sizeof out) && strcmp(out, "shot.png") == 0,
	      "no folder: the bare name");

	// ── A folder is joined on, once ─────────────────────────────────────────
	export_set_dir(EXPORT_PROFILE, "exp_test_out/profiles/");
	check(strcmp(export_dir(EXPORT_PROFILE), "exp_test_out/profiles") == 0, "a trailing separator is dropped");
	check(export_path(EXPORT_PROFILE, "run.json", out, sizeof out) &&
	          strcmp(out, "exp_test_out/profiles/run.json") == 0,
	      "a bare name goes in the kind's folder");
	check(export_dir_exists("exp_test_out/profiles"), "and the folder is made, parents too");
	check(strcmp(export_dir(EXPORT_SCREENSHOT), "") == 0, "each kind has its own folder");
#ifdef _WIN32
	// A folder written with backslashes is joined with one.
	export_set_dir(EXPORT_DUMP, "exp_test_out\\dumps");
	check(export_path(EXPORT_DUMP, "d.bin", out, sizeof out) && strcmp(out, "exp_test_out\\dumps\\d.bin") == 0,
	      "a backslash folder is joined with a backslash");
	_rmdir("exp_test_out\\dumps");
#endif

	// ── A name that says where it goes is left alone ────────────────────────
	check(export_name_has_folder("sub/run") && export_name_has_folder("C:\\x\\run") &&
	          export_name_has_folder("C:run") && !export_name_has_folder("run"),
	      "folders are recognised in either separator, and drive letters");
	check(export_path(EXPORT_PROFILE, "elsewhere/run.json", out, sizeof out) &&
	          strcmp(out, "elsewhere/run.json") == 0,
	      "a name with a folder is used as given");
	check(!export_dir_exists("elsewhere"), "and nothing is created for it");

	// ── Too long is refused, not cut ────────────────────────────────────────
	char tiny[8];
	check(!export_path(EXPORT_PROFILE, "run.json", tiny, sizeof tiny), "a path that does not fit fails");

	remove("exp_test_out/profiles/run.json");
#ifdef _WIN32
	_rmdir("exp_test_out/profiles");
	_rmdir("exp_test_out");
#else
	rmdir("exp_test_out/profiles");
	rmdir("exp_test_out");
#endif
	return x16_test_summary("export_paths");
}
