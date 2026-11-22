// Commander X16 Emulator — where a debugger client may read or write a host file.
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 // realpath, lstat under -std=c11
#endif

#include "host_write_path.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#define WIDE_PATH_MAX 4096
#define path_ncmp _strnicmp
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define path_ncmp strncmp
#endif

#define NARROW_PATH_MAX 4096

static bool
is_sep(char c)
{
#ifdef _WIN32
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

// The real location of something that exists, links followed.
static bool
canonical(const char *path, char *out, size_t n)
{
#ifdef _WIN32
	wchar_t wide[WIDE_PATH_MAX], final_path[WIDE_PATH_MAX];
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, WIDE_PATH_MAX))
		return false;
	HANDLE h = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	const DWORD len = GetFinalPathNameByHandleW(h, final_path, WIDE_PATH_MAX,
	                                            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	CloseHandle(h);
	if (!len || len >= WIDE_PATH_MAX)
		return false;
	const wchar_t *s = final_path;
	if (!wcsncmp(s, L"\\\\?\\UNC\\", 8))
		return false; // a network share is not a folder this rule can vouch for
	if (!wcsncmp(s, L"\\\\?\\", 4))
		s += 4;
	return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, (int)n, NULL, NULL) > 0;
#else
	char *real = realpath(path, NULL);
	if (!real)
		return false;
	const size_t len = strlen(real);
	const bool   fits = len < n;
	if (fits)
		memcpy(out, real, len + 1);
	free(real);
	return fits;
#endif
}

static bool
is_drive_root(const char *p)
{
#ifdef _WIN32
	return strlen(p) == 3 && p[1] == ':' && is_sep(p[2]);
#else
	return !strcmp(p, "/");
#endif
}

// At a separator boundary, so /a/root2 is not inside /a/root.
static bool
is_inside(const char *root, const char *p)
{
	const size_t r = strlen(root);
	return !path_ncmp(root, p, r) && (p[r] == '\0' || is_sep(p[r]));
}

static bool
is_absolute(const char *p)
{
#ifdef _WIN32
	return is_sep(p[0]) || (isalpha((unsigned char)p[0]) && p[1] == ':');
#else
	return p[0] == '/';
#endif
}

#ifdef _WIN32
// CON, NUL, COM1 and friends are devices in every folder, extension or not.
static bool
is_device_name(const char *f)
{
	static const char *const three[] = {"CON", "PRN", "AUX", "NUL"};
	const size_t stem = strcspn(f, ".");
	for (size_t i = 0; i < sizeof three / sizeof three[0]; i++) {
		if (stem == 3 && !_strnicmp(f, three[i], 3))
			return true;
	}
	return stem == 4 && (!_strnicmp(f, "COM", 3) || !_strnicmp(f, "LPT", 3)) &&
	       isdigit((unsigned char)f[3]);
}
#endif

// Is there a link at exactly this path? Followed, it could lead anywhere, even
// when what it points at does not exist yet.
static bool
is_link(const char *p, bool *exists)
{
#ifdef _WIN32
	wchar_t wide[WIDE_PATH_MAX];
	*exists = false;
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, wide, WIDE_PATH_MAX))
		return false;
	const DWORD attrs = GetFileAttributesW(wide);
	if (attrs == INVALID_FILE_ATTRIBUTES)
		return false;
	*exists = true;
	return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
	struct stat st;
	*exists = false;
	if (lstat(p, &st) != 0)
		return false;
	*exists = true;
	return S_ISLNK(st.st_mode);
#endif
}

// Refuse the spellings that are not simply a file in a folder.
static bool
request_is_plain(const char *request, char *err, size_t errn)
{
	if (!request || !request[0]) {
		snprintf(err, errn, "no file name given");
		return false;
	}
#ifdef _WIN32
	if (is_sep(request[0]) && is_sep(request[1])) {
		snprintf(err, errn, "network paths are not allowed");
		return false;
	}
	const bool has_drive = isalpha((unsigned char)request[0]) && request[1] == ':';
	if (has_drive && !is_sep(request[2])) {
		snprintf(err, errn, "a drive letter needs a full path");
		return false;
	}
	if (strchr(request + (has_drive ? 2 : 0), ':')) {
		snprintf(err, errn, "':' is not allowed in a file name");
		return false;
	}
#endif
	return true;
}

static bool
file_name_ok(const char *file, char *err, size_t errn)
{
	if (!file[0] || !strcmp(file, ".") || !strcmp(file, "..")) {
		snprintf(err, errn, "that does not name a file");
		return false;
	}
#ifdef _WIN32
	const char last = file[strlen(file) - 1];
	if (last == '.' || last == ' ' || is_device_name(file)) {
		snprintf(err, errn, "that is not a usable file name on this system");
		return false;
	}
#endif
	return true;
}

// A file, rather than a folder, a device or a pipe that would block the reader.
static bool
is_regular_file(const char *p)
{
#ifdef _WIN32
	wchar_t wide[WIDE_PATH_MAX];
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, wide, WIDE_PATH_MAX))
		return false;
	const DWORD attrs = GetFileAttributesW(wide);
	return attrs != INVALID_FILE_ATTRIBUTES &&
	       !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE));
#else
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool
host_write_path(const char *root, const char *request, char *out, size_t out_size, bool *exists,
                char *err, size_t errn)
{
	char real_root[NARROW_PATH_MAX], full[NARROW_PATH_MAX], real_parent[NARROW_PATH_MAX];

	if (!request_is_plain(request, err, errn))
		return false;

	if (!root || !canonical(root, real_root, sizeof real_root)) {
		snprintf(err, errn, "the folder for written files does not exist");
		return false;
	}
	if (is_drive_root(real_root)) {
		snprintf(err, errn, "will not write to a drive root; start with -fsroot <folder>");
		return false;
	}

	const int made = is_absolute(request)
	                     ? snprintf(full, sizeof full, "%s", request)
	                     : snprintf(full, sizeof full, "%s/%s", real_root, request);
	if (made < 0 || (size_t)made >= sizeof full) {
		snprintf(err, errn, "that path is too long");
		return false;
	}

	// Split into folder and file. Only the folder has to exist yet.
	size_t sep = strlen(full);
	while (sep > 0 && !is_sep(full[sep - 1]))
		sep--;
	const char *file = full + sep;
	if (!file_name_ok(file, err, errn))
		return false;

	char parent[NARROW_PATH_MAX];
	size_t parent_len = sep ? sep - 1 : 0;
#ifdef _WIN32
	if (parent_len == 2 && full[1] == ':')
		parent_len = 3; // "C:" alone would mean that drive's current folder
#endif
	if (parent_len == 0)
		parent_len = sep; // the root itself
	memcpy(parent, full, parent_len);
	parent[parent_len] = '\0';

	if (!canonical(parent, real_parent, sizeof real_parent)) {
		snprintf(err, errn, "that folder does not exist");
		return false;
	}
	if (!is_inside(real_root, real_parent)) {
		snprintf(err, errn, "that is outside the folder for written files (%s)", real_root);
		return false;
	}

	const int joined = snprintf(out, out_size, "%s/%s", real_parent, file);
	if (joined < 0 || (size_t)joined >= out_size) {
		snprintf(err, errn, "that path is too long");
		return false;
	}
	if (is_link(out, exists)) {
		snprintf(err, errn, "will not write through a link");
		return false;
	}
	return true;
}

FILE *
host_create_new(const char *path)
{
#ifdef _WIN32
	wchar_t wide[WIDE_PATH_MAX];
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, WIDE_PATH_MAX))
		return NULL;
	HANDLE h = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
	                       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return NULL;
	const int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
	if (fd < 0) {
		CloseHandle(h);
		return NULL;
	}
	FILE *f = _fdopen(fd, "wb");
	if (!f)
		_close(fd);
	return f;
#else
	const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (fd < 0)
		return NULL;
	FILE *f = fdopen(fd, "wb");
	if (!f)
		close(fd);
	return f;
#endif
}

bool
host_read_path(const char *const *roots, int root_count, const char *request, char *out,
               size_t out_size, char *err, size_t errn)
{
	char base[NARROW_PATH_MAX], real_root[NARROW_PATH_MAX];
	char full[NARROW_PATH_MAX], found[NARROW_PATH_MAX];

	if (!request_is_plain(request, err, errn))
		return false;

	bool have_base = false;
	for (int i = 0; i < root_count && !have_base; i++)
		have_base = roots[i] && canonical(roots[i], base, sizeof base) && !is_drive_root(base);
	if (!have_base) {
		snprintf(err, errn, "no folder is open for reading; start with -fsroot <folder>");
		return false;
	}

	const int made = is_absolute(request) ? snprintf(full, sizeof full, "%s", request)
	                                      : snprintf(full, sizeof full, "%s/%s", base, request);
	if (made < 0 || (size_t)made >= sizeof full) {
		snprintf(err, errn, "that path is too long");
		return false;
	}
	size_t sep = strlen(full);
	while (sep > 0 && !is_sep(full[sep - 1]))
		sep--;
	if (!file_name_ok(full + sep, err, errn))
		return false;

	if (canonical(full, found, sizeof found) && is_regular_file(found)) {
		for (int i = 0; i < root_count; i++) {
			if (!roots[i] || !canonical(roots[i], real_root, sizeof real_root) ||
			    is_drive_root(real_root) || !is_inside(real_root, found))
				continue;
			const int copied = snprintf(out, out_size, "%s", found);
			if (copied < 0 || (size_t)copied >= out_size) {
				snprintf(err, errn, "that path is too long");
				return false;
			}
			return true;
		}
	}
	snprintf(err, errn, "no such file in the folders a client may read (%s)", base);
	return false;
}

bool
host_path_from_cwd(const char *path, char *out, size_t n)
{
	int w;
	if (is_absolute(path)) {
		w = snprintf(out, n, "%s", path);
	} else {
		char cwd[NARROW_PATH_MAX];
#ifdef _WIN32
		wchar_t wide[WIDE_PATH_MAX];
		const DWORD len = GetCurrentDirectoryW(WIDE_PATH_MAX, wide);
		if (!len || len >= WIDE_PATH_MAX ||
		    !WideCharToMultiByte(CP_UTF8, 0, wide, -1, cwd, (int)sizeof cwd, NULL, NULL))
			return false;
#else
		if (!getcwd(cwd, sizeof cwd))
			return false;
#endif
		w = snprintf(out, n, "%s/%s", cwd, path);
	}
	return w >= 0 && (size_t)w < n;
}

bool
host_path_present(const char *path)
{
	bool exists = false;
	(void)is_link(path, &exists);
	return exists;
}
