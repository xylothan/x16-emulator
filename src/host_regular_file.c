// Commander X16 Emulator — is a host path a file that is safe to open?
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 // stat, access under -std=c11
#endif

#include "host_regular_file.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// Whether this spelling leads to a device or a pipe rather than a file on a
// volume. Asked of the path text, because GetFileAttributes() and stat() both
// connect to a named pipe, and report CON and COM1 as ordinary files.
static bool
names_device(const char *path)
{
	// Passed through to the NT namespace; GetFullPathName() does not see it.
	if (!strncmp(path, "\\??\\", 4))
		return true;

	// No I/O. Turns / into \, and a reserved name such as COM1 into \\.\COM1.
	char        full[4096];
	const DWORD len = GetFullPathNameA(path, sizeof full, full, NULL);
	if (!len || len >= sizeof full)
		return true;
	if (full[0] != '\\' || full[1] != '\\')
		return false;

	// \\.\ and \\?\ reach any device, but may also spell a path on a drive.
	if ((full[2] == '.' || full[2] == '?') && full[3] == '\\')
		return !(isalpha((unsigned char)full[4]) && full[5] == ':' && full[6] == '\\');

	// \\host\share: every host has these two, and neither holds files.
	const char *share = strchr(full + 2, '\\');
	if (!share)
		return true;
	share++;
	const size_t n = strcspn(share, "\\");
	return (n == 4 && !_strnicmp(share, "pipe", 4)) || (n == 8 && !_strnicmp(share, "mailslot", 8));
}

bool
host_regular_file_readable(const char *path)
{
	if (!path || !path[0] || names_device(path))
		return false;
	const DWORD attrs = GetFileAttributesA(path);
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)))
		return false;

	// An ACL can deny a read that the attributes say nothing about.
	FILE *f = NULL;
	if (fopen_s(&f, path, "rb") != 0 || !f)
		return false;
	fclose(f);
	return true;
}

#else

#include <sys/stat.h>
#include <unistd.h>

bool
host_regular_file_readable(const char *path)
{
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, R_OK) == 0;
}

#endif
