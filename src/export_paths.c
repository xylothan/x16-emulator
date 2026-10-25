// Commander X16 Emulator — where files the emulator writes on its own go.
// See export_paths.h.

#include "export_paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define DIR_MAX 1024

static char dirs[EXPORT_KINDS][DIR_MAX];

void
export_set_dir(export_kind_t kind, const char *dir)
{
	if ((int)kind < 0 || kind >= EXPORT_KINDS)
		return;
	snprintf(dirs[kind], sizeof dirs[kind], "%s", dir ? dir : "");
	// A trailing separator would double up when a name is joined on.
	size_t n = strlen(dirs[kind]);
	while (n > 1 && (dirs[kind][n - 1] == '/' || dirs[kind][n - 1] == '\\') && dirs[kind][n - 2] != ':')
		dirs[kind][--n] = '\0';
}

const char *
export_dir(export_kind_t kind)
{
	return ((int)kind >= 0 && kind < EXPORT_KINDS) ? dirs[kind] : "";
}

bool
export_name_has_folder(const char *name)
{
	return name && (strchr(name, '/') || strchr(name, '\\') || (name[0] && name[1] == ':'));
}

bool
export_dir_exists(const char *dir)
{
	if (!dir || !dir[0])
		return true; // the working directory
#ifdef _WIN32
	struct _stat64 st;
	return _stat64(dir, &st) == 0 && (st.st_mode & _S_IFDIR);
#else
	struct stat st;
	return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool
make_one(const char *dir)
{
#ifdef _WIN32
	return _mkdir(dir) == 0 || errno == EEXIST;
#else
	return mkdir(dir, 0777) == 0 || errno == EEXIST;
#endif
}

bool
export_make_dir(const char *dir)
{
	if (!dir || !dir[0] || export_dir_exists(dir))
		return true;
	char p[DIR_MAX];
	if (snprintf(p, sizeof p, "%s", dir) >= (int)sizeof p)
		return false;
	// Each parent in turn; skip a drive ("C:") or a leading separator.
	for (char *c = p + 1; *c; c++) {
		if (*c != '/' && *c != '\\')
			continue;
		if (c[-1] == ':')
			continue;
		const char keep = *c;
		*c = '\0';
		make_one(p);
		*c = keep;
	}
	make_one(p);
	return export_dir_exists(dir);
}

bool
export_path(export_kind_t kind, const char *name, char *out, size_t out_size)
{
	if (!name || !out || out_size == 0)
		return false;
	const char *dir = export_dir(kind);
	int n;
	if (!dir[0] || export_name_has_folder(name))
		n = snprintf(out, out_size, "%s", name);
	else
		// Joined with the separator the folder already uses.
		n = snprintf(out, out_size, "%s%c%s", dir, strchr(dir, '\\') ? '\\' : '/', name);
	if (n < 0 || (size_t)n >= out_size)
		return false;
	return export_name_has_folder(name) || export_make_dir(dir);
}
