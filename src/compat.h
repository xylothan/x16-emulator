// Commander X16 Emulator
// Platform compatibility header for MSVC/Windows builds
// Include this instead of <unistd.h> for cross-platform support

#ifndef COMPAT_H
#define COMPAT_H

#include <limits.h>

// Portable "this may legitimately go unused" marker. GCC and Clang warn about
// unused statics under -Wunused-function; MSVC has no equivalent attribute and
// rejects __attribute__ outright, so it needs to expand to nothing there.
#ifdef _MSC_VER
#define MAYBE_UNUSED
#else
#define MAYBE_UNUSED __attribute__((unused))
#endif

// Ensure PATH_MAX is defined
#ifndef PATH_MAX
#ifdef _MAX_PATH
#define PATH_MAX _MAX_PATH
#else
#define PATH_MAX 260
#endif
#endif

#ifdef _MSC_VER

#include <io.h>
#include <direct.h>
#include <windows.h>
#include <stdlib.h>

#define access _access
#define F_OK   0

#define sleep(sec)   Sleep((sec) * 1000)
#define usleep(usec) Sleep((usec) / 1000)

#define localtime_r(src, dst) (!localtime_s((dst), (src)))

#define getcwd  _getcwd
#define chdir   _chdir

static inline char *realpath(const char *path, char *resolved_path)
{
	char *ret = _fullpath(resolved_path, path, PATH_MAX);
	if (ret && _access(ret, 0) && errno == ENOENT) {
		if (resolved_path == NULL)
			free(ret);
		return NULL;
	}
	return ret;
}

#else

#include <unistd.h>

#endif // _MSC_VER

#endif // COMPAT_H
