// Commander X16 Emulator — one line of a VICE label file.
//
// ld65 -Ln writes these, and the x16-rom release ships them as .sym files:
//
//     al 00FFD2 .bsout
//
// Header-only so that rom_syms.c and dbg_info.c can share it without either
// gaining a link dependency: each is unit-tested on its own.
#ifndef AL_LABELS_H
#define AL_LABELS_H

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ld65's linker-generated segment symbols: __CODE_RUN__, __KRAM_LOAD__ ...
static inline bool
al_is_segment_symbol(const char *name, size_t len)
{
	static const char *const tails[] = {"_RUN__", "_LOAD__", "_SIZE__", "_START__", "_LAST__",
	                                    "_FILEOFFS__"};
	if (len < 4 || name[0] != '_' || name[1] != '_')
		return false;
	for (size_t i = 0; i < sizeof tails / sizeof tails[0]; i++) {
		const size_t t = strlen(tails[i]);
		if (len > t && !memcmp(name + len - t, tails[i], t))
			return true;
	}
	return false;
}

// Read the line from `p` to `end`, which must lie inside a NUL-terminated
// buffer. False unless it is an "al" line with a name. The name is not
// terminated: it is `*len` bytes at `*name`.
static inline bool
al_parse_line(const char *p, const char *end, uint32_t *addr, const char **name, size_t *len)
{
	if (end - p <= 4 || p[0] != 'a' || p[1] != 'l' || p[2] != ' ')
		return false;
	char         *after;
	unsigned long v = strtoul(p + 3, &after, 16);
	if (after == p + 3 || after > end)
		return false;
	while (after < end && *after == ' ')
		after++;
	if (after < end && *after == '.')
		after++;
	size_t n = 0;
	while (after + n < end && !isspace((unsigned char)after[n]))
		n++;
	if (!n)
		return false;
	*addr = (uint32_t)(v & 0xFFFFFF);
	*name = after;
	*len  = n;
	return true;
}

#endif
