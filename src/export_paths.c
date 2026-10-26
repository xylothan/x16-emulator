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
#include "dirent_win32.h" // portable opendir/readdir; needs <stdio.h> first

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

static bool
ends_with_ci(const char *s, const char *ext)
{
	const size_t a = strlen(s), b = strlen(ext);
	if (b > a)
		return false;
	for (size_t i = 0; i < b; i++) {
		char x = s[a - b + i], y = ext[i];
		if (x >= 'A' && x <= 'Z')
			x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = (char)(y - 'A' + 'a');
		if (x != y)
			return false;
	}
	return true;
}

int
export_list(const char *dir, const char *ext, char *out, size_t each, int max)
{
	const char *d  = dir && dir[0] ? dir : ".";
	DIR        *dp = opendir(d);
	if (!dp)
		return 0;
	const size_t len  = strlen(d);
	const char   sep  = strchr(d, '\\') ? '\\' : '/';
	const bool   join = len && d[len - 1] != '/' && d[len - 1] != '\\';
	int          n    = 0;
	struct dirent *de;
	while (n < max && (de = readdir(dp)) != NULL) {
		if (!ends_with_ci(de->d_name, ext) || strlen(de->d_name) <= strlen(ext))
			continue;
		const int w = dir && dir[0] ? snprintf(out + (size_t)n * each, each, "%s%s%s", d, join ? (sep == '/' ? "/" : "\\") : "", de->d_name)
		                            : snprintf(out + (size_t)n * each, each, "%s", de->d_name);
		if (w > 0 && (size_t)w < each)
			n++;
	}
	closedir(dp);
	return n;
}
