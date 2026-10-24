// Commander X16 Emulator — ROM symbols. See rom_syms.h.

#include "rom_syms.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rom_sym_t *syms;
static int        count, cap;

static const struct {
	const char *file;
	int         bank;
} files[] = {
	{"kernal.sym", 0},   {"keymap.sym", 1}, {"dos.sym", 2},   {"fat32.sym", 3},
	{"basic.sym", 4},    {"monitor.sym", 5}, {"charset.sym", 6}, {"diag.sym", 7},
	{"graph.sym", 8},    {"demo.sym", 9},   {"audio.sym", 10}, {"util.sym", 11},
	{"bannex.sym", 12},
};

int
rom_syms_bank_of_file(const char *basename)
{
	for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
		if (!strcmp(basename, files[i].file))
			return files[i].bank;
	return -1;
}

// ld65's linker-generated segment symbols: __CODE_RUN__, __KRAM_LOAD__ ...
static bool
is_segment_symbol(const char *name, size_t len)
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

static bool
push(const char *name, size_t len, uint16_t addr, int16_t bank)
{
	// Several names for one address are kept; the report picks one by name.
	if (count == cap) {
		const int  ncap  = cap ? cap * 2 : 1024;
		rom_sym_t *grown = realloc(syms, sizeof *syms * (size_t)ncap);
		if (!grown)
			return false;
		syms = grown;
		cap  = ncap;
	}
	char *copy = malloc(len + 1);
	if (!copy)
		return false;
	memcpy(copy, name, len);
	copy[len] = 0;
	syms[count++] = (rom_sym_t){copy, addr, bank};
	return true;
}

int
rom_syms_add_text(const char *text, int bank)
{
	int         kept = 0;
	const char *p    = text ? text : "";
	while (*p) {
		const char *eol = strchr(p, '\n');
		const char *end = eol ? eol : p + strlen(p);
		// "al <hex> .<name>"
		if (end - p > 4 && p[0] == 'a' && p[1] == 'l' && p[2] == ' ') {
			char         *after;
			unsigned long v = strtoul(p + 3, &after, 16);
			while (after < end && *after == ' ')
				after++;
			if (after < end && *after == '.')
				after++;
			const char *name = after;
			size_t      len  = 0;
			while (name + len < end && !isspace((unsigned char)name[len]))
				len++;
			const uint16_t addr = (uint16_t)(v & 0xFFFF);
			if (len && !is_segment_symbol(name, len)) {
				if (addr >= 0xC000)
					kept += push(name, len, addr, (int16_t)bank);
				else if (bank == 0 && addr >= ROM_SYMS_KRAM_START && addr < ROM_SYMS_KRAM_END)
					kept += push(name, len, addr, -1);
			}
		}
		p = eol ? eol + 1 : end;
	}
	return kept;
}

int
rom_syms_load_dir(const char *dir)
{
	int total = 0;
	for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
		char path[1024];
		const size_t n = strlen(dir);
		const bool   sep = n && (dir[n - 1] == '/' || dir[n - 1] == '\\');
		snprintf(path, sizeof path, "%s%s%s", dir, sep || !n ? "" : "/", files[i].file);
		FILE *f = fopen(path, "rb");
		if (!f)
			continue;
		fseek(f, 0, SEEK_END);
		const long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *text = len > 0 ? malloc((size_t)len + 1) : NULL;
		if (text) {
			const size_t got = fread(text, 1, (size_t)len, f);
			text[got]        = 0;
			total += rom_syms_add_text(text, files[i].bank);
			free(text);
		}
		fclose(f);
	}
	return total;
}

void
rom_syms_clear(void)
{
	for (int i = 0; i < count; i++)
		free((char *)syms[i].name);
	free(syms);
	syms  = NULL;
	count = cap = 0;
}

int
rom_syms_count(void)
{
	return count;
}

const rom_sym_t *
rom_syms_at(int i)
{
	return (i >= 0 && i < count) ? &syms[i] : NULL;
}
