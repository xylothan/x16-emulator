// Commander X16 Emulator
// POSIX dirent.h compatibility shim for MSVC
// Wraps Windows FindFirstFile/FindNextFile APIs

#ifndef DIRENT_WIN32_H
#define DIRENT_WIN32_H

#ifdef _MSC_VER

#include <windows.h>
#include <errno.h>
#include <string.h>

struct dirent {
	char d_name[MAX_PATH];
};

typedef struct {
	HANDLE           hFind;
	WIN32_FIND_DATAA fdata;
	struct dirent    entry;
	bool             first;
} DIR;

static inline DIR *opendir(const char *name)
{
	char path[MAX_PATH];
	size_t len = strlen(name);

	if (len == 0 || len + 3 > MAX_PATH) {
		errno = ENOENT;
		return NULL;
	}

	snprintf(path, MAX_PATH, "%s\\*", name);

	DIR *dir = (DIR *)malloc(sizeof(DIR));
	if (!dir) {
		errno = ENOMEM;
		return NULL;
	}

	dir->hFind = FindFirstFileA(path, &dir->fdata);
	if (dir->hFind == INVALID_HANDLE_VALUE) {
		free(dir);
		errno = ENOENT;
		return NULL;
	}

	dir->first = true;
	return dir;
}

static inline struct dirent *readdir(DIR *dir)
{
	if (!dir)
		return NULL;

	if (dir->first) {
		dir->first = false;
	} else {
		if (!FindNextFileA(dir->hFind, &dir->fdata))
			return NULL;
	}

	strncpy(dir->entry.d_name, dir->fdata.cFileName, MAX_PATH);
	dir->entry.d_name[MAX_PATH - 1] = '\0';
	return &dir->entry;
}

static inline int closedir(DIR *dir)
{
	if (!dir)
		return -1;

	FindClose(dir->hFind);
	free(dir);
	return 0;
}

#else

#include <dirent.h>

#endif // _MSC_VER

#endif // DIRENT_WIN32_H
