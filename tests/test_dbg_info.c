// Parser checks for the cc65 debug-info reader.
// Standalone: no SDL, ROM or emulator; returns 0 when every check passes.
//
// Build: configure with -DBUILD_DBG_INFO_TEST=ON, then build target
// dbg_info_test.
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 // mkfifo, alarm under -std=c11
#endif

#include "dbg_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

// ── Tiny check harness ──────────────────────────────────────────────────────
static int g_fails = 0;

static void
check(int cond, const char *msg)
{
	printf("%s: %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond) {
		g_fails++;
	}
}

// Minimal cc65 .dbg: two source files, two spans, two labels.
static const char *k_dbg =
	"version\tmajor=2,minor=0\n"
	"info\tcsym=0,file=2,lib=0,line=3,mod=1,scope=1,seg=1,span=2,sym=2,type=0\n"
	"file\tid=0,name=\"main.s\",size=200,mtime=0x00000000,mod=0\n"
	"file\tid=1,name=\"util.s\",size=100,mtime=0x00000000,mod=0\n"
	"seg\tid=0,name=\"CODE\",start=0x000801,size=0x0040,addrsize=absolute,type=ro\n"
	"span\tid=0,seg=0,start=0,size=16,type=0\n"
	"span\tid=1,seg=0,start=16,size=16,type=0\n"
	"line\tid=0,file=0,line=10,span=0\n"
	"line\tid=1,file=0,line=11,span=1\n"
	"line\tid=2,file=1,line=42,span=1\n"
	"sym\tid=0,name=\"_main\",addrsize=absolute,size=3,scope=0,def=0,val=0x000801,type=lab\n"
	"sym\tid=1,name=\"_helper\",addrsize=absolute,size=3,scope=0,def=0,val=0x000811,type=lab\n"
	"mod\tid=0,name=\"main.o\",file=0\n";

// Two loads of the same module where an equate's value changed.
static const char *k_dbg_equ_v1 =
	"version\tmajor=2,minor=0\n"
	"file\tid=0,name=\"equ.s\",size=10,mtime=0x00000000,mod=0\n"
	"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro,oname=\"e.prg\"\n"
	"span\tid=0,seg=0,start=0,size=16,type=0\n"
	"line\tid=0,file=0,line=1,span=0\n"
	"sym\tid=0,name=\"SCORE\",addrsize=absolute,size=2,scope=0,def=0,val=0x001234,type=equ\n"
	"sym\tid=1,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
	"mod\tid=0,name=\"equ.o\",file=0\n";

static const char *k_dbg_equ_v2 =
	"version\tmajor=2,minor=0\n"
	"file\tid=0,name=\"equ.s\",size=10,mtime=0x00000000,mod=0\n"
	"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro,oname=\"e.prg\"\n"
	"span\tid=0,seg=0,start=0,size=16,type=0\n"
	"line\tid=0,file=0,line=1,span=0\n"
	"sym\tid=0,name=\"SCORE\",addrsize=absolute,size=2,scope=0,def=0,val=0x00BEEF,type=equ\n"
	"sym\tid=1,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000007,type=equ\n"
	"mod\tid=0,name=\"equ.o\",file=0\n";
// Returns a path valid until this is called WRITE_TEMP_SLOTS more times.
#define WRITE_TEMP_SLOTS 8
static char *
write_temp(const char *contents, const char *leaf)
{
	static char slots[WRITE_TEMP_SLOTS][512];
	static int  next_slot = 0;
	char       *path      = slots[next_slot];
	next_slot = (next_slot + 1) % WRITE_TEMP_SLOTS;
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir) {
		dir = getenv("TEMP");
	}
	if (!dir || !*dir) {
		dir = ".";
	}
	snprintf(path, 512, "%s/%s", dir, leaf);

	FILE *f = fopen(path, "wb");
	if (!f) {
		printf("FAIL: cannot write %s\n", path);
		g_fails++;
		return NULL;
	}
	fwrite(contents, 1, strlen(contents), f);
	fclose(f);
	return path;
}

static char *
write_temp_dbg(void)
{
	return write_temp(k_dbg, "x16_dbg_info_test.dbg");
}

// Two segments sharing the banked window at $A000, one per RAM bank, each with its own source file.
static const char *k_dbg_banked =
	"version\tmajor=2,minor=0\n"
	"file\tid=0,name=\"a_bank5.s\",size=100,mtime=0x00000000,mod=0\n"
	"file\tid=1,name=\"b_bank6.s\",size=100,mtime=0x00000000,mod=0\n"
	"seg\tid=0,name=\"BANKA\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
	"seg\tid=1,name=\"BANKB\",start=0x00a000,size=0x0020,addrsize=absolute,type=ro\n"
	"span\tid=0,seg=0,start=0,size=16,type=0\n"
	"span\tid=1,seg=1,start=0,size=32,type=0\n"
	"line\tid=0,file=0,line=7,span=0\n"
	"line\tid=1,file=1,line=99,span=1\n"
	"sym\tid=0,name=\"EntryB\",addrsize=absolute,val=0xA000,seg=1,type=lab\n"
	"mod\tid=0,name=\"banked.o\",file=0\n";

static char *
write_temp_banked_dbg(void)
{
	return write_temp(k_dbg_banked, "x16_dbg_info_banked.dbg");
}

// cc65 2.19 shape: `csym` ahead of the records it refers to.
#define CSYM_HEADER \
	"version\tmajor=2,minor=0\n" \
	"info\tcsym=0,file=1,lib=0,line=0,mod=1,scope=4,seg=2,span=4,sym=%d,type=1\n" \
	"%s%s"

#define CSYM_FC \
	"csym\tid=0,name=\"fc\",scope=1,type=0,sc=static,sym=23\n" \
	"csym\tid=1,name=\"a\",scope=1,type=0,sc=auto,offs=1\n" \
	"csym\tid=2,name=\"b\",scope=1,type=0,sc=auto\n" \
	"csym\tid=3,name=\"l\",scope=1,type=0,sc=auto,offs=-1\n"

#define CSYM_CD \
	"csym\tid=4,name=\"cd\",scope=2,type=0,sc=static,sym=22\n" \
	"csym\tid=5,name=\"$anon-param-0001\",scope=2,type=0,sc=auto,offs=2\n" \
	"csym\tid=6,name=\"r\",scope=2,type=0,sc=reg,offs=4,sym=15\n" \
	"csym\tid=7,name=\"s\",scope=2,type=0,sc=auto\n"

static const char k_csym_tail[] =
	"file\tid=0,name=\"f.c\",size=10,mtime=0x00000000,mod=0\n"
	"seg\tid=0,name=\"CODE\",start=0x000840,size=0x0100,addrsize=absolute,type=ro\n"
	"seg\tid=1,name=\"DATA\",start=0x001000,size=0x0010,addrsize=absolute,type=rw\n"
	"span\tid=0,seg=0,start=0,size=16\n"
	"span\tid=1,seg=0,start=16,size=16\n"
	"span\tid=2,seg=1,start=0,size=2,type=6\n"
	"span\tid=3,seg=0,start=32,size=8\n"
	"scope\tid=0,name=\"\",mod=0,size=40,span=0+1\n"
	"scope\tid=1,name=\"_fc\",mod=0,type=scope,size=16,parent=0,sym=23,span=0\n"
	"scope\tid=2,name=\"_cd\",mod=0,type=scope,size=16,parent=0,sym=22,span=2+1\n"
	"scope\tid=3,name=\"_asm\",mod=0,type=scope,size=8,parent=0,sym=21,span=3\n"
	"sym\tid=23,name=\"_fc\",addrsize=absolute,size=16,scope=0,def=0,val=0x840,seg=0,type=lab\n"
	"sym\tid=22,name=\"_cd\",addrsize=absolute,size=16,scope=0,def=0,val=0x850,seg=0,type=lab\n"
	"sym\tid=21,name=\"_asm\",addrsize=absolute,size=8,scope=0,def=0,val=0x860,seg=0,type=lab\n"
	"sym\tid=15,name=\"regbank\",addrsize=zeropage,size=6,scope=0,def=0,val=0x22,type=lab\n";

// The same ids, elsewhere in memory.
static const char k_csym_tail_b[] =
	"file\tid=0,name=\"g.c\",size=10,mtime=0x00000000,mod=0\n"
	"seg\tid=0,name=\"CODE\",start=0x002000,size=0x0010,addrsize=absolute,type=ro\n"
	"span\tid=0,seg=0,start=0,size=16\n"
	"scope\tid=1,name=\"_other\",mod=0,type=scope,size=16,parent=0,sym=23,span=0\n"
	"sym\tid=23,name=\"_other\",addrsize=absolute,size=16,scope=0,def=0,val=0x2000,seg=0,type=lab\n";

static int
load_csym(const char *leaf, int syms, const char *csym, const char *tail)
{
	const size_t n    = sizeof CSYM_HEADER + 16 + strlen(csym) + strlen(tail);
	char        *text = (char *)malloc(n);
	if (!text) {
		return -1;
	}
	snprintf(text, n, CSYM_HEADER, syms, csym, tail);
	char *path = write_temp(text, leaf);
	free(text);
	if (!path) {
		return -1;
	}
	const int result = dbg_info_load(path);
	remove(path);
	return result;
}

static int g_change_calls;

static void
count_change(void)
{
	g_change_calls++;
}

static const char *
cfunc_name_at(dbg_addr_t addr)
{
	dbg_cfunc_t f;
	const int   i = dbg_info_cfunc_containing(addr);
	return i >= 0 && dbg_info_cfunc_at(i, &f) ? f.name : "";
}

static bool
cvar_is(int func, int var, const char *name, int offs, bool is_register, bool is_anonymous)
{
	dbg_cvar_t v;
	return dbg_info_cfunc_var_at(func, var, &v) && !strcmp(v.name, name) && v.offs == offs &&
	       v.is_register == is_register && v.is_anonymous == is_anonymous;
}

// fc's records with one fault; cd beside them must survive.
static const char *const k_csym_bad[] = {
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,name=\"a\",scope=1,sc=auto,offs=abc\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,name=\"a\",scope=1,sc=auto,offs=12x\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,name=\"a\",scope=1,sc=auto,offs=\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,name=\"a\",scope=1,sc=auto,offs=32768\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,name=\"a\",scope=1,sc=auto,offs=-32768\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,scope=1,sc=auto\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,name=\"\",scope=1,sc=auto\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,name=\"a b\",scope=1,sc=auto\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,name=\"a\",scope=1\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=x,name=\"a\",scope=1,sc=auto\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=0,name=\"a\",scope=1,sc=auto\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\ncsym\tid=1,name=\"fd\",scope=1,sc=static,sym=23\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=static,sym=22\n",
	"csym\tid=0,name=\"fc\",scope=1,sc=auto,sym=23\n",
};

int
main(void)
{
	// Nothing loaded: every lookup declines.
	check(!dbg_info_is_loaded(), "reports not loaded before any load");
	{
		const char *file = NULL;
		int         line = 0;
		check(!dbg_info_addr_to_source(0x0801, &file, &line),
		      "address lookup declines when nothing is loaded");
		dbg_addr_t addr = 0;
		check(!dbg_info_label_to_addr("_main", &addr),
		      "label lookup declines when nothing is loaded");
	}

	check(dbg_info_load("this/does/not/exist.dbg") != 0,
	      "load of a missing file reports failure");

	char *path = write_temp_dbg();
	if (!path) {
		return 1;
	}
	check(dbg_info_load(path) == 0, "loads a well-formed .dbg");
	check(dbg_info_is_loaded(), "reports loaded afterwards");

	// Address -> source. The first span covers $0801..$0810, attributed to
	// main.s:10.
	{
		const char *file = NULL;
		int         line = 0;
		check(dbg_info_addr_to_source(0x0801, &file, &line), "maps a span-start address");
		check(file && strstr(file, "main.s") != NULL, "  ...to the right file");
		check(line == 10, "  ...to the right line");
	}

	// Landing mid-instruction still resolves.
	{
		const char *file = NULL;
		int         line = 0;
		check(dbg_info_addr_to_source_nearest(0x0805, &file, &line),
		      "maps an address inside a span to the enclosing line");
		check(line == 10, "  ...to the line that opened the span");
	}

	// An address outside every segment has no answer.
	{
		const char *file = NULL;
		int         line = 0;
		check(!dbg_info_addr_to_source(0xC000, &file, &line),
		      "declines an address outside every segment");
	}

	// Source -> address, the direction that breakpoints-by-line need.
	{
		dbg_addr_t addr = 0;
		check(dbg_info_source_to_addr("main.s", 10, &addr), "maps file/line back to an address");
		check(addr == 0x0801, "  ...to the span start");
	}

	// Symbols.
	{
		dbg_addr_t addr = 0;
		check(dbg_info_label_to_addr("_main", &addr) && addr == 0x0801, "finds a label by name");
		check(dbg_info_label_to_addr("_MAIN", &addr) && addr == 0x0801,
		      "label lookup is case-insensitive");
		check(!dbg_info_label_to_addr("_nosuch", &addr), "declines an unknown label");

		const char *name = NULL;
		check(dbg_info_addr_to_label(0x0811, &name) && name && strcmp(name, "_helper") == 0,
		      "finds a label by address");
		check(dbg_info_symbol_count() == 2, "enumerates both labels");

		// Enumeration is sorted by address.
		dbg_addr_t prev   = 0;
		int      sorted = 1;
		for (int i = 0; i < dbg_info_symbol_count(); i++) {
			const char *n = NULL;
			dbg_addr_t  a = 0;
			if (!dbg_info_symbol_at(i, &n, &a) || (i && a < prev)) {
				sorted = 0;
			}
			prev = a;
		}
		check(sorted, "enumeration is sorted by address");

		const char *n = NULL;
		dbg_addr_t  a = 0;
		check(!dbg_info_symbol_at(dbg_info_symbol_count(), &n, &a),
		      "declines an out-of-range index");
		check(!dbg_info_symbol_at(-1, &n, &a), "declines a negative index");
	}

	// Source files referenced by the .dbg.
	check(dbg_info_file_count() == 2, "enumerates both source files");

	// Span starts are what the anchored disassembler falls back to.
	check(dbg_info_is_span_start(0x0801), "recognises a span start");
	check(!dbg_info_is_span_start(0x0805), "does not claim a mid-span address");

	// Unloading a range drops that range's mappings.
	dbg_info_unload_range(0x0801, 0x0840);
	{
		const char *file = NULL;
		int         line = 0;
		check(!dbg_info_addr_to_source(0x0801, &file, &line),
		      "unload_range drops mappings in the range");
	}

	dbg_info_free();
	check(!dbg_info_is_loaded(), "reports not loaded after free");
	// Freeing twice is harmless.
	dbg_info_free();
	check(1, "double free is harmless");

	// A failed load leaves what is already loaded undisturbed.
	check(dbg_info_load(path) == 0, "reloads after free");
	check(dbg_info_load("this/does/not/exist.dbg") != 0, "second load of a missing file fails");
	{
		const char *file = NULL;
		int         line = 0;
		check(dbg_info_addr_to_source(0x0801, &file, &line),
		      "a failed load leaves the existing mapping intact");
	}

	// Reloading a module replaces it rather than duplicating its segments.
	{
		const int files_before = dbg_info_file_count();
		dbg_info_unload_range(0x0801, 0x0840);
		check(dbg_info_load(path) == 0, "reloads the same module");
		const char *file = NULL;
		int         line = 0;
		check(dbg_info_addr_to_source(0x0801, &file, &line),
		      "the reloaded module still resolves");
		check(line == 10, "  ...to the original line");

		// Unloading keeps file records; a reload must not append a second copy.
		check(dbg_info_file_count() == files_before,
		      "  ...without accumulating a second copy of its files");
	}
	// Repeated swaps must stay stable rather than degrading each time.
	const int files_before_swaps = dbg_info_file_count();
	for (int i = 0; i < 5; i++) {
		dbg_info_unload_range(0x0801, 0x0840);
		dbg_info_load(path);
	}
	{
		const char *file = NULL;
		int         line = 0;
		check(dbg_info_addr_to_source(0x0801, &file, &line) && line == 10,
		      "still resolves after repeated module swaps");
		check(dbg_info_file_count() == files_before_swaps,
		      "and does not grow a little on every swap");
	}

	// A path with no room left for the ".dbg" extension must be declined, not
	// written past the end of the caller's buffer.
	{
		char  huge[4096];
		memset(huge, 'a', sizeof(huge) - 1);
		huge[sizeof(huge) - 1] = '\0';
		dbg_addr_t lo = 0, hi = 0;
		check(!dbg_info_peek_file_range(huge, &lo, &hi),
		      "declines an over-long path instead of overflowing");
	}

	dbg_info_free();
	remove(path);

	// ── Bank-aware mapping across a module swap ─────────────────────────────
	// Two segments share $A000, one per bank; a swap replaces a module.
	{
		char *bpath = write_temp_banked_dbg();
		if (!bpath) {
			return 1;
		}
		check(dbg_info_load(bpath) == 0, "loads a banked .dbg");

		// Before any LOAD neither bank is known: the lookup answers but flags a guess.
		const char *fu = NULL;
		int         lu = 0;
		check(dbg_info_addr_to_source_banked_ex(0xA000, 5, &fu, &lu) == DBG_BANK_UNKNOWN,
		      "flags a match against an unknown-bank segment as a guess");
		check(dbg_info_addr_to_source_banked(0xA000, 5, &fu, &lu),
		      "still reports the mapping for an unknown-bank segment");

		// A breakpoint resolved now cannot be pinned to a bank: none is known.
		{
			dbg_addr_t a  = 0;
			int        rb = 0;
			check(dbg_info_source_to_addr_bank("b_bank6.s", 99, &a, &rb) && a == 0xA000 && rb == -1,
			      "source line in an unknown-bank segment reports no bank");
			rb = 0;
			check(dbg_info_label_to_addr_bank("EntryB", &a, &rb) && a == 0xA000 && rb == -1,
			      "label in an unknown-bank segment reports no bank");
		}

		// Learning one segment's bank does not make the other confident.
		dbg_info_note_bank_load(0xA000, 0x10, 5);
		check(dbg_info_addr_to_source_banked_ex(0xA000, 5, &fu, &lu) == DBG_BANK_RESOLVED,
		      "reports the newly learned segment as resolved");
		check(dbg_info_addr_to_source_banked_ex(0xA018, 5, &fu, &lu) == DBG_BANK_UNKNOWN,
		      "keeps a still-unknown segment flagged as a guess");

		dbg_info_note_bank_load(0xA000, 0x20, 6);

		// Once a LOAD has shown the banks, line and label lookups carry them.
		{
			dbg_addr_t a  = 0;
			int        rb = -1;
			check(dbg_info_source_to_addr_bank("a_bank5.s", 7, &a, &rb) && rb == 5,
			      "source line reports the bank its segment was loaded into");
			check(dbg_info_source_to_addr_bank("b_bank6.s", 99, &a, &rb) && rb == 6,
			      "  ...and the other segment's bank for its own line");
			check(dbg_info_label_to_addr_bank("EntryB", &a, &rb) && a == 0xA000 && rb == 6,
			      "label reports the bank of the segment it is defined in");
		}

		const char *f5 = NULL, *f6 = NULL;
		int         l5 = 0, l6 = 0;
		bool ok5 = dbg_info_addr_to_source_banked(0xA000, 5, &f5, &l5);
		bool ok6 = dbg_info_addr_to_source_banked(0xA000, 6, &f6, &l6);
		check(ok5 && ok6, "resolves the banked address in both banks");
		check(ok5 && ok6 && f5 && f6 && strcmp(f5, f6) != 0,
		      "tells the two banks apart");

		// Now swap the module out and back, as an overlay reload would.
		dbg_info_unload_range(0xA000, 0xBFFF);
		check(dbg_info_load(bpath) == 0, "reloads the banked module");
		dbg_info_note_bank_load(0xA000, 0x10, 5);
		dbg_info_note_bank_load(0xA000, 0x20, 6);

		f5 = f6 = NULL;
		ok5 = dbg_info_addr_to_source_banked(0xA000, 5, &f5, &l5);
		ok6 = dbg_info_addr_to_source_banked(0xA000, 6, &f6, &l6);
		check(ok5 && ok6 && f5 && f6 && strcmp(f5, f6) != 0,
		      "still tells the banks apart after a module swap");

		// A bank we actually learned is reported as resolved.
		check(dbg_info_addr_to_source_banked_ex(0xA000, 5, &f5, &l5) == DBG_BANK_RESOLVED,
		      "reports a known bank as resolved");

		// A bank never observed is not reported as resolved.
		check(dbg_info_addr_to_source_banked_ex(0xA000, 9, &f5, &l5) == DBG_BANK_UNKNOWN,
		      "reports an unlearned bank as unknown rather than guessing");

		// And an address with no debug info at all is still a plain miss.
		check(dbg_info_addr_to_source_banked_ex(0x5000, 5, &f5, &l5) == DBG_BANK_NO_MATCH,
		      "reports an unmapped address as no match");

		dbg_info_free();
		remove(bpath);
	}

	// ── 24-bit addresses ────────────────────────────────────────────────────
	// Code above $FFFF on a GS/Gen2 machine keeps its mappings at load.
	{
		static const char *k_dbg_far =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"far.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"FAR\",start=0x018000,size=0x0010,addrsize=far,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=5,span=0\n"
			"sym\tid=0,name=\"_far\",addrsize=far,size=3,scope=0,def=0,val=0x018000,type=lab\n";
		char *fpath = write_temp(k_dbg_far, "x16_dbg_info_far.dbg");
		if (!fpath) {
			return 1;
		}
		check(dbg_info_load(fpath) == 0, "loads a .dbg with addresses above $FFFF");
		const char *file = NULL;
		int         line = 0;
		check(dbg_info_addr_to_source(0x018000, &file, &line) && line == 5,
		      "maps a bank-1 address to source");
		dbg_addr_t addr = 0;
		check(dbg_info_label_to_addr("_far", &addr) && addr == 0x018000,
		      "keeps the full 24-bit value of a far label");
		check(dbg_info_is_span_start(0x018000), "recognises a far span start");
		dbg_info_free();
		remove(fpath);
	}

	// ── Long records ────────────────────────────────────────────────────────
	// A `sym` line with a long reference list must not be truncated or lose the symbol.
	{
		char *big = malloc(70000);
		if (!big) {
			printf("FAIL: out of memory building the long-record case\n");
			return 1;
		}
		int n = sprintf(big,
		                "version\tmajor=2,minor=0\n"
		                "file\tid=0,name=\"long.s\",size=10,mtime=0x00000000,mod=0\n"
		                "seg\tid=0,name=\"CODE\",start=0x000801,size=0x0010,addrsize=absolute,type=ro\n"
		                "span\tid=0,seg=0,start=0,size=16,type=0\n"
		                "line\tid=0,file=0,line=1,span=0\n"
		                "sym\tid=0,name=\"_busy\",addrsize=absolute,ref=");
		for (int i = 0; i < 8000; i++) {
			n += sprintf(big + n, "%d+", i); // a long reference list
		}
		n += sprintf(big + n, "99,size=3,scope=0,def=0,val=0x000801,type=lab\n");

		char *lpath = write_temp(big, "x16_dbg_info_long.dbg");
		free(big);
		if (!lpath) {
			return 1;
		}
		check(dbg_info_load(lpath) == 0, "loads a .dbg with a very long record");
		dbg_addr_t addr = 0;
		check(dbg_info_label_to_addr("_busy", &addr) && addr == 0x0801,
		      "reads fields that follow a multi-kilobyte reference list");
		dbg_info_free();
		remove(lpath);
	}

	// ── A confirmed bank outranks a narrower guess ──────────────────────────
	// Only the outer segment's bank is known; a confirmed bank outranks a narrower guess.
	{
		dbg_info_free();
		char *bpath = write_temp_banked_dbg();   // static buffer; do not free
		if (!bpath) {
			check(false, "could not write the banked fixture");
		} else {
			check(dbg_info_load(bpath) == 0, "reloads the banked fixture");

			dbg_info_note_bank_load(0xA000, 0x20, 6);   // the OUTER segment only

			const char *fb = NULL;
			int         lb = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 6, &fb, &lb)
			          == DBG_BANK_RESOLVED,
			      "a confirmed bank is not demoted by a narrower unknown one");
			check(fb && strstr(fb, "b_bank6") != NULL,
			      "  ...and answers with the segment that matched");
			check(lb == 99, "  ...and with its line");

			dbg_info_free();
			remove(bpath);
		}
	}
	// ── A reloaded equate replaces the old value ────────────────────────────
	// A reloaded equate replaces the old value, bank equates included.
	{
		dbg_info_free();
		char *p1 = write_temp(k_dbg_equ_v1, "x16_dbg_info_equ.dbg");
		if (!p1) {
			check(false, "could not write the equate fixture");
		} else {
			check(dbg_info_load(p1) == 0, "loads a module with equates");
			dbg_addr_t v = 0;
			check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
			      "reads an equate's value");

			// The same module rebuilt, with the constant moved.
			char *p2 = write_temp(k_dbg_equ_v2, "x16_dbg_info_equ.dbg");
			if (p2) {
				dbg_info_unload_range(0xA000, 0xA00F);
				check(dbg_info_load(p2) == 0, "reloads it after a rebuild");
				v = 0;
				check(dbg_info_equate_to_value("SCORE", &v) && v == 0xBEEF,
				      "and the new value replaces the old one");

				// Bank equates seed a segment's RAM bank; the reload's bank 7 must be in force.
				const char *bf = NULL;
				int         bl = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA000, 7, &bf, &bl)
				          == DBG_BANK_RESOLVED,
				      "and a reloaded bank equate reseeds the segment's bank");
			}
			remove(p1);
			dbg_info_free();
		}
	}
	// Equates and files dedupe within one module; two may define a name differently.
	{
		dbg_info_free();
		char *pa = write_temp(k_dbg_equ_v1, "x16_dbg_info_modA.dbg");
		if (!pa) {
			check(false, "could not write module A");
		} else {
			check(dbg_info_load(pa) == 0, "loads module A");
			// A different module, same equate name, different value.
			char *pb = write_temp(k_dbg_equ_v2, "x16_dbg_info_modB.dbg");
			if (pb) {
				check(dbg_info_load(pb) == 0, "loads module B alongside it");
				dbg_addr_t v = 0;
				check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
				      "and B's SCORE does not overwrite A's");
				remove(pb);
			}
			remove(pa);
			dbg_info_free();
		}
	}

	// A `line` that precedes the `file` it names.
	{
		dbg_info_free();
		static const char *k_dbg_line_first =
			"version\tmajor=2,minor=0\n"
			"line\tid=0,file=0,line=7,span=0\n"
			"file\tid=0,name=\"late.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x000801,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"mod\tid=0,name=\"late.o\",file=0\n";
		char *lp = write_temp(k_dbg_line_first, "x16_dbg_info_linefirst.dbg");
		if (!lp) {
			check(false, "could not write the record-order fixture");
		} else {
			check(dbg_info_load(lp) == 0, "loads a .dbg with line before file");
			const char *f = NULL;
			int         n = 0;
			check(dbg_info_addr_to_source(0x0801, &f, &n) && n == 7,
			      "and the line still resolves to its file");

			// The reload moves the ID base, so the reused file record's ID differs.
			dbg_info_unload_range(0x0801, 0x0810);
			check(dbg_info_load(lp) == 0, "reloads it");
			f = NULL;
			n = 0;
			check(dbg_info_addr_to_source(0x0801, &f, &n) && n == 7,
			      "and it still resolves after the IDs have moved on");
			remove(lp);
			dbg_info_free();
		}
	}

	// Prefix names: RAM_BANK_CODE must not claim both CODE and CODE2.
	{
		dbg_info_free();
		static const char *k_dbg_two_banks =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"two.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"seg\tid=1,name=\"CODE2\",start=0x00a020,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"span\tid=1,seg=1,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"line\tid=1,file=0,line=2,span=1\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"sym\tid=1,name=\"RAM_BANK_CODE2\",addrsize=absolute,size=1,scope=0,def=0,val=0x000007,type=equ\n"
			"mod\tid=0,name=\"two.o\",file=0\n";
		char *tp = write_temp(k_dbg_two_banks, "x16_dbg_info_twobanks.dbg");
		if (!tp) {
			check(false, "could not write the prefix-collision fixture");
		} else {
			check(dbg_info_load(tp) == 0, "loads two segments with related names");
			const char *f = NULL;
			int         l = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l)
			          == DBG_BANK_RESOLVED,
			      "CODE takes the bank named for it");
			f = NULL;
			l = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA020, 7, &f, &l)
			          == DBG_BANK_RESOLVED,
			      "and CODE2 is not claimed by CODE's equate");
			remove(tp);
			dbg_info_free();
		}
	}

	// Two equates prefix-match one segment with different banks: the segment stays unknown.
	{
		dbg_info_free();
		static const char *k_dbg_ambig =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"amb.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"STORETILE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_STORE_TILEMAP\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"sym\tid=1,name=\"RAM_BANK_STORE_TILESET\",addrsize=absolute,size=1,scope=0,def=0,val=0x000007,type=equ\n"
			"mod\tid=0,name=\"amb.o\",file=0\n";
		char *ap = write_temp(k_dbg_ambig, "x16_dbg_info_ambig.dbg");
		if (!ap) {
			check(false, "could not write the ambiguity fixture");
		} else {
			check(dbg_info_load(ap) == 0, "loads an ambiguously named segment");
			const char *f = NULL;
			int         l = 0;
			// Neither candidate may be adopted, so neither bank resolves.
			dbg_bank_result_t m3 = dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l);
			f = NULL;
			l = 0;
			dbg_bank_result_t m7 = dbg_info_addr_to_source_banked_ex(0xA000, 7, &f, &l);
			check(m3 != DBG_BANK_RESOLVED && m7 != DBG_BANK_RESOLVED,
			      "and picks neither bank when two equates match equally well");
			remove(ap);
			dbg_info_free();
		}
	}

	// Two overlays each define RAM_BANK_CODE differently; each wins for its own segment.
	{
		dbg_info_free();
		static const char *k_bank_a =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"ova.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"mod\tid=0,name=\"ova.o\",file=0\n";
		static const char *k_bank_b =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"ovb.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a020,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000007,type=equ\n"
			"mod\tid=0,name=\"ovb.o\",file=0\n";
		char *ba = write_temp(k_bank_a, "x16_dbg_info_bankA.dbg");
		if (!ba) {
			check(false, "could not write bank module A");
		} else {
			check(dbg_info_load(ba) == 0, "loads overlay A");
			char *bb = write_temp(k_bank_b, "x16_dbg_info_bankB.dbg");
			if (bb) {
				check(dbg_info_load(bb) == 0, "loads overlay B, same equate name");
				const char *f = NULL;
				int         l = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l)
				          == DBG_BANK_RESOLVED,
				      "A's CODE keeps the bank A declared");
				f = NULL;
				l = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA020, 7, &f, &l)
				          == DBG_BANK_RESOLVED,
				      "and B's CODE keeps the bank B declared");
				remove(bb);
			}
			remove(ba);
			dbg_info_free();
		}
	}

	// Two modules compiling a same-named file keep two records.
	{
		dbg_info_free();
		char *fa = write_temp(k_dbg_equ_v1, "x16_dbg_info_fileA.dbg");
		if (!fa) {
			check(false, "could not write file module A");
		} else {
			check(dbg_info_load(fa) == 0, "loads a module declaring equ.s");
			int after_a = dbg_info_file_count();
			char *fb = write_temp(k_dbg_equ_v2, "x16_dbg_info_fileB.dbg");
			if (fb) {
				check(dbg_info_load(fb) == 0, "loads another declaring its own equ.s");
				check(dbg_info_file_count() == after_a + 1,
				      "and the two same-named files stay separate records");
				remove(fb);
			}
			remove(fa);
			dbg_info_free();
		}
	}

	// A segment given its bank by another module's equate is recomputed when that module reloads.
	{
		dbg_info_free();
		static const char *k_donor_v1 =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"donor.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"DONOR\",start=0x000800,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"mod\tid=0,name=\"donor.o\",file=0\n";
		static const char *k_donor_v2 =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"donor.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"DONOR\",start=0x000800,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000007,type=equ\n"
			"mod\tid=0,name=\"donor.o\",file=0\n";
		// Declares no bank equate of its own, so it is seeded from the donor's.
		static const char *k_consumer =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"consumer.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"mod\tid=0,name=\"consumer.o\",file=0\n";
		char *dp = write_temp(k_donor_v1, "x16_dbg_info_donor.dbg");
		if (!dp) {
			check(false, "could not write the donor fixture");
		} else {
			check(dbg_info_load(dp) == 0, "loads the donor module");
			char *cp = write_temp(k_consumer, "x16_dbg_info_consumer.dbg");
			if (cp) {
				check(dbg_info_load(cp) == 0, "loads a consumer with no equate of its own");
				const char *f = NULL;
				int         l = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l)
				          == DBG_BANK_RESOLVED,
				      "which borrows the donor's bank");

				// The donor is rebuilt with a different bank and reloaded.
				char *dp2 = write_temp(k_donor_v2, "x16_dbg_info_donor.dbg");
				if (dp2) {
					dbg_info_unload_range(0x0800, 0x080F);
					check(dbg_info_load(dp2) == 0, "reloads the donor with a new bank");
					f = NULL;
					l = 0;
					check(dbg_info_addr_to_source_banked_ex(0xA000, 7, &f, &l)
					          == DBG_BANK_RESOLVED,
					      "and the borrowed bank follows it");
					f = NULL;
					l = 0;
					check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l)
					          != DBG_BANK_RESOLVED,
					      "rather than answering to the bank nobody declares");
				}
				remove(cp);
			}
			remove(dp);
			dbg_info_free();
		}
	}

	// Re-deriving equate banks keeps what was observed at runtime.
	{
		dbg_info_free();
		char *op = write_temp(k_dbg_equ_v1, "x16_dbg_info_obs.dbg");
		if (!op) {
			check(false, "could not write the observation fixture");
		} else {
			check(dbg_info_load(op) == 0, "loads a module whose equate says bank 3");
			// The loader saw it land somewhere else.
			dbg_info_note_bank_load(0xA000, 0x10, 9);
			const char *f = NULL;
			int         l = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 9, &f, &l)
			          == DBG_BANK_RESOLVED,
			      "and a runtime observation overrides it");

			// Any later load re-derives equate banks; the observation must stay.
			char *o2 = write_temp(k_dbg_equ_v1, "x16_dbg_info_obs2.dbg");
			if (o2) {
				check(dbg_info_load(o2) == 0, "loads another module afterwards");
				f = NULL;
				l = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA000, 9, &f, &l)
				          == DBG_BANK_RESOLVED,
				      "and re-deriving equate banks leaves the observation alone");
				remove(o2);
			}
			remove(op);
			dbg_info_free();
		}
	}

	// A module's own equate does not outrank another module's better match.
	{
		dbg_info_free();
		static const char *k_rank_main =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"rmain.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"OTHER\",start=0x000900,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE2\",addrsize=absolute,size=1,scope=0,def=0,val=0x000009,type=equ\n"
			"mod\tid=0,name=\"rmain.o\",file=0\n";
		static const char *k_rank_ovl =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"rovl.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE2\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000002,type=equ\n"
			"mod\tid=0,name=\"rovl.o\",file=0\n";
		char *rm = write_temp(k_rank_main, "x16_dbg_info_rankmain.dbg");
		if (!rm) {
			check(false, "could not write the ranking fixture");
		} else {
			check(dbg_info_load(rm) == 0, "loads a module declaring RAM_BANK_CODE2");
			char *ro = write_temp(k_rank_ovl, "x16_dbg_info_rankovl.dbg");
			if (ro) {
				// Its own RAM_BANK_CODE only prefix-matches segment CODE2;
				// the other module's RAM_BANK_CODE2 matches it exactly.
				check(dbg_info_load(ro) == 0, "loads an overlay whose own equate only resembles it");
				const char *f = NULL;
				int         l = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA000, 9, &f, &l)
				          == DBG_BANK_RESOLVED,
				      "an exact match elsewhere beats a resemblance at home");
				f = NULL;
				l = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA000, 2, &f, &l)
				          != DBG_BANK_RESOLVED,
				      "and the resemblance does not also answer");
				remove(ro);
			}
			remove(rm);
			dbg_info_free();
		}
	}

	// Two equates in one module reduce to the segment's name with different banks: no choice.
	{
		dbg_info_free();
		static const char *k_self_contradict =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"sc.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"sym\tid=1,name=\"CODE_BANK\",addrsize=absolute,size=1,scope=0,def=0,val=0x000007,type=equ\n"
			"mod\tid=0,name=\"sc.o\",file=0\n";
		char *sp = write_temp(k_self_contradict, "x16_dbg_info_selfcon.dbg");
		if (!sp) {
			check(false, "could not write the self-contradiction fixture");
		} else {
			check(dbg_info_load(sp) == 0, "loads a module naming one segment twice");
			const char *f = NULL;
			int         l = 0;
			dbg_bank_result_t r3 = dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l);
			f = NULL;
			l = 0;
			dbg_bank_result_t r7 = dbg_info_addr_to_source_banked_ex(0xA000, 7, &f, &l);
			check(r3 != DBG_BANK_RESOLVED && r7 != DBG_BANK_RESOLVED,
			      "and two exact matches that disagree resolve to neither");
			remove(sp);
			dbg_info_free();
		}
	}

	// A common prefix shorter than four characters is not evidence of anything.
	{
		dbg_info_free();
		static const char *k_short_prefix =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"sp.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_COD\",addrsize=absolute,size=1,scope=0,def=0,val=0x000005,type=equ\n"
			"mod\tid=0,name=\"sp.o\",file=0\n";
		char *pp = write_temp(k_short_prefix, "x16_dbg_info_shortpfx.dbg");
		if (!pp) {
			check(false, "could not write the short-prefix fixture");
		} else {
			check(dbg_info_load(pp) == 0, "loads a segment with a near-miss equate");
			const char *f = NULL;
			int         l = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 5, &f, &l)
			          != DBG_BANK_RESOLVED,
			      "and a three-character overlap does not claim it");
			remove(pp);
			dbg_info_free();
		}
	}

	// A rebuilt module that no longer declares an equate stops declaring it.
	{
		dbg_info_free();
		static const char *k_gone_v1 =
			"version\tmajor=2,minor=0\n"
			"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=2,type=0\n"
			"file\tid=0,name=\"gone.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"GONE\",start=0x000800,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"sym\tid=1,name=\"SCORE\",addrsize=absolute,size=2,scope=0,def=0,val=0x001234,type=equ\n"
			"mod\tid=0,name=\"gone.o\",file=0\n";
		// Same module rebuilt with both equates removed; it still has a label.
		static const char *k_gone_v2 =
			"version\tmajor=2,minor=0\n"
			"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=1,type=0\n"
			"file\tid=0,name=\"gone.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"GONE\",start=0x000800,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"_gone_entry\",addrsize=absolute,size=3,scope=0,def=0,val=0x000800,type=lab\n"
			"mod\tid=0,name=\"gone.o\",file=0\n";
		static const char *k_borrower =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"borrow.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"mod\tid=0,name=\"borrow.o\",file=0\n";
		char *gp = write_temp(k_gone_v1, "x16_dbg_info_gone.dbg");
		if (!gp) {
			check(false, "could not write the removed-equate fixture");
		} else {
			check(dbg_info_load(gp) == 0, "loads a module with two equates");
			char *bp = write_temp(k_borrower, "x16_dbg_info_borrow.dbg");
			if (bp) {
				check(dbg_info_load(bp) == 0, "loads a borrower with none of its own");
				const char *f = NULL;
				int         l = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l)
				          == DBG_BANK_RESOLVED,
				      "which borrows the declared bank");

				char *gp2 = write_temp(k_gone_v2, "x16_dbg_info_gone.dbg");
				if (gp2) {
					dbg_info_unload_range(0x0800, 0x080F);
					check(dbg_info_load(gp2) == 0, "reloads it with the equates removed");
					dbg_addr_t v = 0;
					check(!dbg_info_equate_to_value("SCORE", &v),
					      "and the dropped equate stops resolving");
					f = NULL;
					l = 0;
					check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l)
					          != DBG_BANK_RESOLVED,
					      "and the borrowed bank is not still claimed");
				}
				remove(bp);
			}
			remove(gp);
			dbg_info_free();
		}
	}

	// Names too long to reduce are refused, not truncated.
	{
		dbg_info_free();
		// The equate target fits the squash buffer; the longer segment name must not match.
		char longseg[256], longequ[256];
		memset(longseg, 'A', 130);
		memcpy(longseg + 130, "LEFT", 5);
		memset(longequ, 'A', 127);
		longequ[127] = '\0';
		char buf[2048];
		snprintf(buf, sizeof buf,
		         "version\tmajor=2,minor=0\n"
		         "file\tid=0,name=\"lng.s\",size=10,mtime=0x00000000,mod=0\n"
		         "seg\tid=0,name=\"%s\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
		         "span\tid=0,seg=0,start=0,size=16,type=0\n"
		         "line\tid=0,file=0,line=1,span=0\n"
		         "sym\tid=0,name=\"RAM_BANK_%s\",addrsize=absolute,size=1,scope=0,def=0,val=0x000004,type=equ\n"
		         "mod\tid=0,name=\"lng.o\",file=0\n",
		         longseg, longequ);
		char *lp = write_temp(buf, "x16_dbg_info_long_names.dbg");
		if (!lp) {
			check(false, "could not write the long-name fixture");
		} else {
			check(dbg_info_load(lp) == 0, "loads a module with over-long names");
			const char *f = NULL;
			int         l = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 4, &f, &l)
			          != DBG_BANK_RESOLVED,
			      "and two names that differ past the cutoff do not match");
			remove(lp);
			dbg_info_free();
		}
	}

	// An ambiguous winning rank does not fall through to a weaker one.
	{
		dbg_info_free();
		// Owns CODE2, and its own equate only resembles it (own-prefix).
		static const char *k_ft_own =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"ftown.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE2\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000002,type=equ\n"
			"mod\tid=0,name=\"ftown.o\",file=0\n";
		// Two other modules name CODE2 exactly, and disagree.
		static const char *k_ft_x =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"ftx.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"XONE\",start=0x000900,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE2\",addrsize=absolute,size=1,scope=0,def=0,val=0x000005,type=equ\n"
			"mod\tid=0,name=\"ftx.o\",file=0\n";
		static const char *k_ft_y =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"fty.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"YONE\",start=0x000920,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE2\",addrsize=absolute,size=1,scope=0,def=0,val=0x000006,type=equ\n"
			"mod\tid=0,name=\"fty.o\",file=0\n";
		char px[512], py[512], po2[512];
		char *t = write_temp(k_ft_x, "x16_dbg_info_ftx.dbg");
		bool wrote = (t != NULL);
		if (wrote) snprintf(px, sizeof px, "%s", t);
		t = wrote ? write_temp(k_ft_y, "x16_dbg_info_fty.dbg") : NULL;
		wrote = wrote && (t != NULL);
		if (wrote) snprintf(py, sizeof py, "%s", t);
		t = wrote ? write_temp(k_ft_own, "x16_dbg_info_ftown.dbg") : NULL;
		wrote = wrote && (t != NULL);
		if (wrote) snprintf(po2, sizeof po2, "%s", t);
		if (!wrote) {
			check(false, "could not write the fall-through fixtures");
		} else {
			check(dbg_info_load(px) == 0 && dbg_info_load(py) == 0 &&
			          dbg_info_load(po2) == 0,
			      "loads two disagreeing exact namers and one that resembles");
			const char *f = NULL;
			int         l = 0;
			dbg_bank_result_t r5 = dbg_info_addr_to_source_banked_ex(0xA000, 5, &f, &l);
			f = NULL; l = 0;
			dbg_bank_result_t r6 = dbg_info_addr_to_source_banked_ex(0xA000, 6, &f, &l);
			f = NULL; l = 0;
			dbg_bank_result_t r2 = dbg_info_addr_to_source_banked_ex(0xA000, 2, &f, &l);
			check(r5 != DBG_BANK_RESOLVED && r6 != DBG_BANK_RESOLVED,
			      "the contradicting exact matches resolve to neither");
			check(r2 != DBG_BANK_RESOLVED,
			      "and the weaker resemblance is not used to break the tie");
			remove(px);
			remove(py);
			remove(po2);
			dbg_info_free();
		}
	}

	// Among equally good name matches, the segment's own module still wins.
	{
		dbg_info_free();
		static const char *k_pp_other =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"ppo.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"PPOTHER\",start=0x000940,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_STORETILEMAP\",addrsize=absolute,size=1,scope=0,def=0,val=0x000005,type=equ\n"
			"mod\tid=0,name=\"ppo.o\",file=0\n";
		static const char *k_pp_own =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"ppw.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"STORETILE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_STORETILESET\",addrsize=absolute,size=1,scope=0,def=0,val=0x000006,type=equ\n"
			"mod\tid=0,name=\"ppw.o\",file=0\n";
		char ppa[512], ppb[512];
		char *t2 = write_temp(k_pp_other, "x16_dbg_info_ppo.dbg");
		bool wrote2 = (t2 != NULL);
		if (wrote2) snprintf(ppa, sizeof ppa, "%s", t2);
		t2 = wrote2 ? write_temp(k_pp_own, "x16_dbg_info_ppw.dbg") : NULL;
		wrote2 = wrote2 && (t2 != NULL);
		if (wrote2) snprintf(ppb, sizeof ppb, "%s", t2);
		if (!wrote2) {
			check(false, "could not write the prefix-ownership fixtures");
		} else {
			check(dbg_info_load(ppa) == 0 && dbg_info_load(ppb) == 0,
			      "loads two modules that both merely resemble the segment");
			const char *f = NULL;
			int         l = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 6, &f, &l)
			          == DBG_BANK_RESOLVED,
			      "and the segment's own module wins among resemblances");
			remove(ppa);
			remove(ppb);
			dbg_info_free();
		}
	}

	// A file that opens but parses to nothing keeps the previous generation.
	// so a half-written one is a real state, not a hypothetical.
	{
		dbg_info_free();
		static const char *k_tx_good =
			"version\tmajor=2,minor=0\n"
			"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=2,type=0\n"
			"file\tid=0,name=\"tx.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"SCORE\",addrsize=absolute,size=2,scope=0,def=0,val=0x001234,type=equ\n"
			"sym\tid=1,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"mod\tid=0,name=\"tx.o\",file=0\n";
		char txp[512];
		char *t3 = write_temp(k_tx_good, "x16_dbg_info_tx.dbg");
		if (!t3) {
			check(false, "could not write the transactional fixture");
		} else {
			snprintf(txp, sizeof txp, "%s", t3);
			check(dbg_info_load(txp) == 0, "loads a module with equates");
			dbg_addr_t v = 0;
			check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
			      "and reads one back");

			// The same path, rebuilt empty -- as a truncated write leaves it.
			if (write_temp("", "x16_dbg_info_tx.dbg")) {
				check(dbg_info_load(txp) != 0,
				      "an empty rebuild is reported as a failed load");
				v = 0;
				check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
				      "an empty rebuild does not delete what still works");
				const char *f = NULL;
				int         l = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l)
				          == DBG_BANK_RESOLVED,
				      "and the bank it seeded survives too");
			}

			// And a file of the right shape but no recognisable records.
			if (write_temp("nonsense\nmore nonsense\n", "x16_dbg_info_tx.dbg")) {
				check(dbg_info_load(txp) != 0,
				      "an unparseable rebuild is reported as a failed load");
				v = 0;
				check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
				      "nor does an unparseable one");
			}

			// The shape that actually occurs: cut off after the line records
			// but before the symbols. ld65 emits them in that order.
			static const char *k_tx_partial =
				"version\tmajor=2,minor=0\n"
				"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=2,type=0\n"
				"file\tid=0,name=\"tx.s\",size=10,mtime=0x00000000,mod=0\n"
				"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
				"span\tid=0,seg=0,start=0,size=16,type=0\n"
				"line\tid=0,file=0,line=1,span=0\n";
			if (write_temp(k_tx_partial, "x16_dbg_info_tx.dbg")) {
				check(dbg_info_load(txp) != 0,
				      "a write cut off before the symbols is a failed load");
				v = 0;
				check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
				      "and a write cut off before the symbols keeps them too");
			}
			remove(txp);
			dbg_info_free();
		}
	}

	// An equate name too long for the reduction buffer is refused, not copied.
	{
		dbg_info_free();
		char huge[400];
		memset(huge, 'B', 300);
		huge[300] = '\0';
		char buf2[2048];
		snprintf(buf2, sizeof buf2,
		         "version\tmajor=2,minor=0\n"
		         "file\tid=0,name=\"ov.s\",size=10,mtime=0x00000000,mod=0\n"
		         "seg\tid=0,name=\"%s\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
		         "span\tid=0,seg=0,start=0,size=16,type=0\n"
		         "line\tid=0,file=0,line=1,span=0\n"
		         "sym\tid=0,name=\"RAM_BANK_%s\",addrsize=absolute,size=1,scope=0,def=0,val=0x000008,type=equ\n"
		         "mod\tid=0,name=\"ov.o\",file=0\n",
		         huge, huge);
		char *hp = write_temp(buf2, "x16_dbg_info_hugeequ.dbg");
		if (!hp) {
			check(false, "could not write the long-equate fixture");
		} else {
			char hpath[512];
			snprintf(hpath, sizeof hpath, "%s", hp);
			check(dbg_info_load(hpath) == 0, "loads a module with an oversized equate name");
			const char *f = NULL;
			int         l = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 8, &f, &l)
			          != DBG_BANK_RESOLVED,
			      "and an oversized equate name is handled without incident");
			remove(hpath);
			dbg_info_free();
		}
	}

	// ld65 states its symbol count, separating a cut-off file from a smaller one.
	{
		dbg_info_free();
		static const char *k_cnt_full =
			"version\tmajor=2,minor=0\n"
			"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=3,type=0\n"
			"file\tid=0,name=\"cnt.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"_cnt_entry\",addrsize=absolute,size=3,scope=0,def=0,val=0x00a000,type=lab\n"
			"sym\tid=1,name=\"SCORE\",addrsize=absolute,size=2,scope=0,def=0,val=0x001234,type=equ\n"
			"sym\tid=2,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"mod\tid=0,name=\"cnt.o\",file=0\n";
		// Promises three, delivers one: a write still in progress.
		static const char *k_cnt_cut =
			"version\tmajor=2,minor=0\n"
			"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=3,type=0\n"
			"file\tid=0,name=\"cnt.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"_cnt_entry\",addrsize=absolute,size=3,scope=0,def=0,val=0x00a000,type=lab\n";
		// Same shape, but promises one and delivers it: a real rebuild that
		// dropped the constants.
		static const char *k_cnt_slim =
			"version\tmajor=2,minor=0\n"
			"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=1,type=0\n"
			"file\tid=0,name=\"cnt.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"_cnt_entry\",addrsize=absolute,size=3,scope=0,def=0,val=0x00a000,type=lab\n"
			"mod\tid=0,name=\"cnt.o\",file=0\n";
		char cpath[512];
		char *c1 = write_temp(k_cnt_full, "x16_dbg_info_cnt.dbg");
		if (!c1) {
			check(false, "could not write the record-count fixture");
		} else {
			snprintf(cpath, sizeof cpath, "%s", c1);
			check(dbg_info_load(cpath) == 0, "loads a module that declares three symbols");
			dbg_addr_t v = 0;
			check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
			      "and reads a constant from it");

			if (write_temp(k_cnt_cut, "x16_dbg_info_cnt.dbg")) {
				check(dbg_info_load(cpath) != 0,
				      "a file that promised three and gave one is a failed load");
				v = 0;
				check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
				      "a file that promised three and gave one keeps them");
			}

			// Cut inside the symbol block rather than before it.
			static const char *k_cnt_mid =
				"version\tmajor=2,minor=0\n"
				"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=3,type=0\n"
				"file\tid=0,name=\"cnt.s\",size=10,mtime=0x00000000,mod=0\n"
				"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
				"span\tid=0,seg=0,start=0,size=16,type=0\n"
				"line\tid=0,file=0,line=1,span=0\n"
				"sym\tid=0,name=\"_cnt_entry\",addrsize=absolute,size=3,scope=0,def=0,val=0x00a000,type=lab\n"
				"sym\tid=1,name=\"SCORE\",addrsize=absolute,size=2,scope=0,def=0,val=0x001234,type=equ\n";
			if (write_temp(k_cnt_full, "x16_dbg_info_cnt.dbg")) {
				dbg_info_load(cpath);
			}
			if (write_temp(k_cnt_mid, "x16_dbg_info_cnt.dbg")) {
				check(dbg_info_load(cpath) != 0,
				      "a cut inside the symbol block is a failed load too");
				v = 0;
				check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
				      "and the bank equate that follows the cut survives");
				const char *bf = NULL;
				int         bl = 0;
				check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &bf, &bl)
				          == DBG_BANK_RESOLVED,
				      "so the bank it seeded is still known");
			}

			// A symbol line cut off mid-record is not counted.
			static const char *k_cnt_torn =
				"version\tmajor=2,minor=0\n"
				"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=2,type=0\n"
				"file\tid=0,name=\"cnt.s\",size=10,mtime=0x00000000,mod=0\n"
				"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
				"span\tid=0,seg=0,start=0,size=16,type=0\n"
				"line\tid=0,file=0,line=1,span=0\n"
				"sym\tid=0,name=\"_cnt_entry\",addrsize=absolute,size=3,scope=0,def=0,val=0x00a000,type=lab\n"
				"sym\tid=1,name=\"SCO\n";
			if (write_temp(k_cnt_full, "x16_dbg_info_cnt.dbg")) {
				dbg_info_load(cpath);
			}
			if (write_temp(k_cnt_torn, "x16_dbg_info_cnt.dbg")) {
				dbg_info_load(cpath);
				v = 0;
				check(dbg_info_equate_to_value("SCORE", &v) && v == 0x1234,
				      "and a torn final symbol does not count towards the promise");
			}

			// Restore, then the same shape with an honest count.
			if (write_temp(k_cnt_full, "x16_dbg_info_cnt.dbg")) {
				dbg_info_load(cpath);
			}
			if (write_temp(k_cnt_slim, "x16_dbg_info_cnt.dbg")) {
				dbg_info_load(cpath);
				v = 0;
				check(!dbg_info_equate_to_value("SCORE", &v),
				      "but one that promised one and gave one drops them");
			}
			remove(cpath);
			dbg_info_free();
		}
	}

	// One name as a bank constant or an ordinary one: the later definition remains.
	{
		dbg_info_free();
		static const char *k_cross =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"cr.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"sym\tid=1,name=\"RAM_BANK_CODE\",addrsize=absolute,size=2,scope=0,def=0,val=0x001234,type=equ\n"
			"mod\tid=0,name=\"cr.o\",file=0\n";
		char *xp = write_temp(k_cross, "x16_dbg_info_cross.dbg");
		if (!xp) {
			check(false, "could not write the cross-table fixture");
		} else {
			char xpath[512];
			snprintf(xpath, sizeof xpath, "%s", xp);
			check(dbg_info_load(xpath) == 0, "loads a name defined twice across tables");
			dbg_addr_t v = 0;
			check(dbg_info_equate_to_value("RAM_BANK_CODE", &v) && v == 0x1234,
			      "the later definition answers name lookups");
			const char *f = NULL;
			int         l = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &f, &l)
			          != DBG_BANK_RESOLVED,
			      "and the superseded one no longer seeds a bank");
			remove(xpath);
			dbg_info_free();
		}

		// And the same collision in the other order: filed as an ordinary
		// constant first, then redefined as a bank one.
		dbg_info_free();
		static const char *k_cross_rev =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"cr2.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"RAM_BANK_CODE\",addrsize=absolute,size=2,scope=0,def=0,val=0x001234,type=equ\n"
			"sym\tid=1,name=\"RAM_BANK_CODE\",addrsize=absolute,size=1,scope=0,def=0,val=0x000003,type=equ\n"
			"mod\tid=0,name=\"cr2.o\",file=0\n";
		char *yp = write_temp(k_cross_rev, "x16_dbg_info_cross2.dbg");
		if (!yp) {
			check(false, "could not write the reverse cross-table fixture");
		} else {
			char ypath[512];
			snprintf(ypath, sizeof ypath, "%s", yp);
			check(dbg_info_load(ypath) == 0, "loads the same collision reversed");
			dbg_addr_t v2 = 0;
			check(!dbg_info_equate_to_value("RAM_BANK_CODE", &v2),
			      "the superseded constant no longer answers name lookups");
			const char *f2 = NULL;
			int         l2 = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 3, &f2, &l2)
			          == DBG_BANK_RESOLVED,
			      "and the later definition seeds the bank");
			remove(ypath);
			dbg_info_free();
		}
	}

	// A .dbg with no `info` line is merged and does not mean the module declares nothing.
	{
		dbg_info_free();
		static const char *k_ni_full =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"ni.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"NILIMIT\",addrsize=absolute,size=2,scope=0,def=0,val=0x004321,type=equ\n"
			"mod\tid=0,name=\"ni.o\",file=0\n";
		static const char *k_ni_cut =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"ni.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n";
		char nipath[512];
		char *n1 = write_temp(k_ni_full, "x16_dbg_info_noinfo.dbg");
		if (!n1) {
			check(false, "could not write the info-less fixture");
		} else {
			snprintf(nipath, sizeof nipath, "%s", n1);
			check(dbg_info_load(nipath) == 0, "loads a .dbg with no info record");
			dbg_addr_t v = 0;
			check(dbg_info_equate_to_value("NILIMIT", &v) && v == 0x4321,
			      "and reads a constant from it");
			if (write_temp(k_ni_cut, "x16_dbg_info_noinfo.dbg")) {
				dbg_info_load(nipath);
				v = 0;
				check(dbg_info_equate_to_value("NILIMIT", &v) && v == 0x4321,
				      "a short read it cannot detect still keeps the constant");
			}
			remove(nipath);
			dbg_info_free();
		}
	}

	// ── Complete records that are simply not ours to keep ───────────────────
	// ld65 counts every `sym` record, including imports and out-of-range constants.
	{
		dbg_info_free();
		static const char *k_dbg_imp =
			"version\tmajor=2,minor=0\n"
			"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=3,type=0\n"
			"file\tid=0,name=\"imp.s\",size=10,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x000801,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=3,span=0\n"
			"sym\tid=0,name=\"_start\",addrsize=absolute,size=3,scope=0,def=0,val=0x000801,type=lab\n"
			"sym\tid=1,name=\"_puts\",addrsize=absolute,scope=0,def=0,ref=7,type=imp,exp=4\n"
			"sym\tid=2,name=\"EOF\",addrsize=long,scope=5,def=9,val=0xFFFFFFFF,type=equ\n"
			"mod\tid=0,name=\"imp.o\",file=0\n";
		char *ip = write_temp(k_dbg_imp, "x16_dbg_info_imp.dbg");
		if (!ip) {
			check(false, "could not write the import fixture");
		} else {
			check(dbg_info_load(ip) == 0,
			      "merges a .dbg whose declared symbol count includes imports "
			      "and out-of-range constants");
			const char *f = NULL;
			int         n = 0;
			check(dbg_info_addr_to_source(0x0801, &f, &n) && n == 3,
			      "  ...and its mappings are usable");
			dbg_addr_t a = 0;
			check(dbg_info_label_to_addr("_start", &a) && a == 0x0801,
			      "  ...and its labels survived");
			check(!dbg_info_equate_to_value("EOF", &a),
			      "  ...while the out-of-range constant is counted, not kept");
			remove(ip);
			dbg_info_free();
		}
	}

	// ── cc65 C: high-level lines outrank the generated assembly ─────────────
	// ca65 -g: the type=1 C line outranks the smaller type=0 assembly spans.
	// Layout, all in CODE at $0801:
	//   $0801-$080C  x16hello.c:5   over four assembly spans
	//   $080D-$0814  x16hello.c:6   over two
	//   $0815-$0818  x16hello.c:5   again, later (a loop's second half)
	//   $0819-$081E  x16util.s:10   a macro record over an assembly one
	//   $081F-$0824  x16mixed.c:7   over x16mixed.s
	//   $0825-$082A  x16gen.c:9     over cl65's one-step temporary name
	{
		dbg_info_free();
		static const char *k_dbg_cc65 =
			"version\tmajor=2,minor=0\n"
			"info\tcsym=0,file=7,lib=0,line=16,mod=1,scope=1,seg=1,span=16,sym=2,type=0\n"
			"file\tid=0,name=\"x16hello.c\",size=200,mtime=0x00000000,mod=0\n"
			"file\tid=1,name=\"x16hello.s\",size=900,mtime=0x00000000,mod=0\n"
			"file\tid=2,name=\"x16util.s\",size=100,mtime=0x00000000,mod=0\n"
			"file\tid=3,name=\"x16mixed.c\",size=100,mtime=0x00000000,mod=0\n"
			"file\tid=4,name=\"x16mixed.s\",size=100,mtime=0x00000000,mod=0\n"
			"file\tid=5,name=\"x16gen.c\",size=100,mtime=0x00000000,mod=0\n"
			"file\tid=6,name=\"x16gen.c.81128.0.s\",size=900,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x000801,size=0x0040,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=12,type=0\n"
			"span\tid=1,seg=0,start=0,size=3,type=0\n"
			"span\tid=2,seg=0,start=3,size=2,type=0\n"
			"span\tid=3,seg=0,start=5,size=3,type=0\n"
			"span\tid=4,seg=0,start=8,size=4,type=0\n"
			"span\tid=5,seg=0,start=12,size=8,type=0\n"
			"span\tid=6,seg=0,start=12,size=4,type=0\n"
			"span\tid=7,seg=0,start=16,size=4,type=0\n"
			"span\tid=8,seg=0,start=20,size=4,type=0\n"
			"span\tid=9,seg=0,start=20,size=4,type=0\n"
			"span\tid=10,seg=0,start=24,size=6,type=0\n"
			"span\tid=11,seg=0,start=24,size=2,type=0\n"
			"span\tid=12,seg=0,start=30,size=6,type=0\n"
			"span\tid=13,seg=0,start=30,size=3,type=0\n"
			"span\tid=14,seg=0,start=36,size=6,type=0\n"
			"span\tid=15,seg=0,start=36,size=3,type=0\n"
			"line\tid=0,file=0,line=5,type=1,span=0\n"
			"line\tid=1,file=1,line=100,span=1\n"
			"line\tid=2,file=1,line=101,span=2\n"
			"line\tid=3,file=1,line=102,span=3\n"
			"line\tid=4,file=1,line=103,span=4\n"
			"line\tid=5,file=0,line=6,type=1,span=5\n"
			"line\tid=6,file=1,line=104,span=6\n"
			"line\tid=7,file=1,line=105,span=7\n"
			"line\tid=8,file=0,line=5,type=1,span=8\n"
			"line\tid=9,file=1,line=106,span=9\n"
			"line\tid=10,file=2,line=10,type=2,count=1,span=10\n"
			"line\tid=11,file=2,line=20,span=11\n"
			"line\tid=12,file=3,line=7,type=1,span=12\n"
			"line\tid=13,file=4,line=200,span=13\n"
			"line\tid=14,file=5,line=9,type=1,span=14\n"
			"line\tid=15,file=6,line=300,span=15\n"
			"sym\tid=0,name=\"_main\",addrsize=absolute,size=3,scope=0,def=0,val=0x000801,type=lab\n"
			"sym\tid=1,name=\"_printf\",addrsize=absolute,scope=0,def=0,ref=9,type=imp,exp=6\n"
			"mod\tid=0,name=\"x16hello.o\",file=0\n";

		// A hand-written .s on disk must survive the picker filter.
		char *mixed_asm = write_temp("; hand-written\n", "x16mixed.s");
		char *cpath     = write_temp(k_dbg_cc65, "x16_dbg_info_cc65.dbg");
		if (!cpath) {
			check(false, "could not write the cc65 fixture");
		} else {
			check(dbg_info_load(cpath) == 0, "loads a cc65 C .dbg");

			const char *f = NULL;
			int         n = 0;

			// The start of a C statement, where a tiny assembly span starts too.
			check(dbg_info_addr_to_source_nearest(0x0801, &f, &n),
			      "maps the first address of a C statement");
			check(f && strstr(f, "x16hello.c") != NULL, "  ...to the C source");
			check(n == 5, "  ...to the C line");

			// An interior instruction boundary ($0804), where stepping lands.
			f = NULL; n = 0;
			check(dbg_info_addr_to_source_nearest(0x0804, &f, &n)
			          && f && strstr(f, "x16hello.c") != NULL && n == 5,
			      "an interior instruction boundary still reports the C line");

			// And mid-instruction, which is where a multi-byte operand puts it.
			f = NULL; n = 0;
			check(dbg_info_addr_to_source_nearest(0x0807, &f, &n)
			          && f && strstr(f, "x16hello.c") != NULL && n == 5,
			      "an address inside an instruction reports the C line");

			// The last byte of the statement belongs to it, not to the next.
			f = NULL; n = 0;
			check(dbg_info_addr_to_source_nearest(0x080C, &f, &n)
			          && f && strstr(f, "x16hello.c") != NULL && n == 5,
			      "the last byte of a C statement is still that statement");

			// Stepping on: the next statement, not a smeared-out first one.
			f = NULL; n = 0;
			check(dbg_info_addr_to_source_nearest(0x080D, &f, &n)
			          && f && strstr(f, "x16hello.c") != NULL && n == 6,
			      "the next C statement is reported at its own address");

			// The exact-match form feeds DAP disassembly and must not name a hidden file.
			f = NULL; n = 0;
			check(dbg_info_addr_to_source(0x0804, &f, &n)
			          && f && strstr(f, "x16hello.c") != NULL && n == 5,
			      "the exact-address lookup also prefers the C line");

			// Identical bounds: the type decides, not the span size.
			f = NULL; n = 0;
			check(dbg_info_addr_to_source_nearest(0x0815, &f, &n)
			          && f && strstr(f, "x16hello.c") != NULL && n == 5,
			      "a C line wins a same-size tie with the generated assembly");

			// type=1 > type=2 > type=0: a macro record still beats the raw
			// assembly underneath it.
			f = NULL; n = 0;
			check(dbg_info_addr_to_source_nearest(0x0819, &f, &n)
			          && f && strstr(f, "x16util.s") != NULL && n == 10,
			      "a macro record outranks the assembly line inside it");

			// Line breakpoints. This C line generated code twice; the
			// breakpoint belongs at the start of the statement.
			{
				dbg_addr_t a = 0;
				check(dbg_info_source_to_addr("x16hello.c", 5, &a) && a == 0x0801,
				      "a C line breakpoint resolves to the lowest of its spans");
				a = 0;
				check(dbg_info_source_to_addr("x16hello.c", 6, &a) && a == 0x080D,
				      "and each statement resolves to its own start");
			}

			// Assembly spans are the anchored disassembler's alignment anchors and stay in the map.
			check(dbg_info_is_span_start(0x0804), "keeps an assembly span start as an anchor");
			check(dbg_info_is_span_start(0x0809), "keeps the later ones too");
			check(!dbg_info_is_span_start(0x0807), "and still rejects a mid-span address");

			// Picker: x16hello.s is generated, x16util.s has no C, x16mixed.s is on disk.
			{
				int  fc      = dbg_info_file_count();
				bool has_c   = false, has_gen = false, has_util = false;
				bool has_mixed_c = false, has_mixed_s = false;
				bool has_gen_c = false, has_gen_tmp = false;
				for (int i = 0; i < fc; i++) {
					const char *nm = NULL;
					if (!dbg_info_file_at(i, &nm) || !nm)
						continue;
					if (!strcmp(nm, "x16hello.c")) has_c = true;
					if (!strcmp(nm, "x16hello.s")) has_gen = true;
					if (!strcmp(nm, "x16util.s")) has_util = true;
					if (!strcmp(nm, "x16mixed.c")) has_mixed_c = true;
					if (!strcmp(nm, "x16mixed.s")) has_mixed_s = true;
					if (!strcmp(nm, "x16gen.c")) has_gen_c = true;
					if (!strcmp(nm, "x16gen.c.81128.0.s")) has_gen_tmp = true;
				}
				check(fc == 5, "the picker drops only the generated intermediates");
				check(has_c, "  ...keeps the C source");
				check(!has_gen, "  ...hides the .s cc65 generated and deleted");
				check(has_util, "  ...keeps a .s with no C source behind it");
				check(has_mixed_c && has_mixed_s,
				      "  ...keeps a hand-written .s that is on disk");
				// cl65 one-step names the intermediate after the C file plus a temporary suffix.
				check(has_gen_c, "  ...keeps the C source of a one-step build");
				check(!has_gen_tmp, "  ...hides cl65's temporary intermediate");

				// Indices stay dense.
				const char *nm = NULL;
				check(dbg_info_file_at(fc - 1, &nm) && nm, "the last visible index resolves");
				check(!dbg_info_file_at(fc, &nm), "and one past the end does not");
			}

			// Unloading the C module takes its high-level info with it.
			dbg_info_unload_range(0x0801, 0x0840);
			check(dbg_info_file_count() == 7,
			      "unloading the C module puts every file back on offer");

			remove(cpath);
			if (mixed_asm)
				remove(mixed_asm);
			dbg_info_free();
		}
	}

	// Assembly-only .dbg: nothing preferred, nothing hidden, cc65 runtime .s files included.
	{
		dbg_info_free();
		char *apath = write_temp(k_dbg, "x16_dbg_info_asmonly.dbg");
		if (!apath) {
			check(false, "could not write the assembly-only fixture");
		} else {
			check(dbg_info_load(apath) == 0, "reloads the assembly-only fixture");
			check(dbg_info_file_count() == 2,
			      "an assembly-only .dbg still offers every file it names");
			const char *f = NULL;
			int         n = 0;
			check(dbg_info_addr_to_source_nearest(0x0805, &f, &n) && n == 10,
			      "and still resolves by innermost span");
			remove(apath);
			dbg_info_free();
		}
	}

	// ── A refused load must not speak for the ones that succeeded ───────────
	// A refused half-written C .dbg must not switch loaded assembly modules to the C lookup.
	{
		dbg_info_free();
		// An assembly module with two overlapping spans opening at different
		// addresses, so which one gets reported depends on the lookup taken.
		static const char *k_dbg_asm_two =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"two.s\",size=100,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x000801,size=0x0020,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=8,type=0\n"
			"span\tid=1,seg=0,start=3,size=17,type=0\n"
			"line\tid=0,file=0,line=10,span=0\n"
			"line\tid=1,file=0,line=11,span=1\n"
			"mod\tid=0,name=\"two.o\",file=0\n";
		// A C module cut off mid-write: its `info` line promises two symbols
		// and only one arrived.
		static const char *k_dbg_c_cut =
			"version\tmajor=2,minor=0\n"
			"info\tcsym=0,file=1,lib=0,line=1,mod=1,scope=1,seg=1,span=1,sym=2,type=0\n"
			"file\tid=0,name=\"cut.c\",size=100,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"OTHER\",start=0x002000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"line\tid=0,file=0,line=3,type=1,span=0\n"
			"sym\tid=0,name=\"_cut\",addrsize=absolute,size=3,scope=0,def=0,val=0x002000,type=lab\n";

		char *apath = write_temp(k_dbg_asm_two, "x16_dbg_info_two.dbg");
		char *cpath = write_temp(k_dbg_c_cut, "x16_dbg_info_cut.dbg");
		if (!apath || !cpath) {
			check(false, "could not write the refused-load fixtures");
		} else {
			check(dbg_info_load(apath) == 0, "loads an assembly module");
			const char *f = NULL;
			int         before = 0, after = 0;
			check(dbg_info_addr_to_source_nearest(0x0806, &f, &before),
			      "  ...and resolves an address inside two overlapping spans");

			check(dbg_info_load(cpath) != 0, "refuses a half-written C .dbg");

			f = NULL;
			check(dbg_info_addr_to_source_nearest(0x0806, &f, &after),
			      "the assembly module still resolves afterwards");
			check(before == after,
			      "  ...to the same line: a refused load does not change how "
			      "another module is read");
			check(dbg_info_file_count() == 2,
			      "  ...and hides nothing on a refused load's say-so");

			remove(apath);
			remove(cpath);
			dbg_info_free();
		}
	}

	// ld65's segment bank= gives the bank with no LOAD; ignored outside the window.
	{
		dbg_info_free();
		static const char *k_dbg_seg_bank =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"a.s\",size=100,mtime=0x00000000,mod=0\n"
			"file\tid=1,name=\"b.s\",size=100,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"B_ONE\",start=0x00a000,size=0x0010,addrsize=absolute,type=rw,"
			"oname=\"x.prg.47\",ooffs=0,bank=47\n"
			"seg\tid=1,name=\"B_TWO\",start=0x00a000,size=0x0020,addrsize=absolute,type=rw,"
			"oname=\"x.prg.66\",ooffs=0,bank=66\n"
			"seg\tid=2,name=\"CODE\",start=0x000801,size=0x0010,addrsize=absolute,type=ro,bank=3\n"
			"span\tid=0,seg=0,start=0,size=16,type=0\n"
			"span\tid=1,seg=1,start=0,size=32,type=0\n"
			"line\tid=0,file=0,line=7,span=0\n"
			"line\tid=1,file=1,line=99,span=1\n"
			"sym\tid=0,name=\"EntryOne\",addrsize=absolute,val=0xA000,seg=0,type=lab\n"
			"sym\tid=1,name=\"EntryTwo\",addrsize=absolute,val=0xA000,seg=1,type=lab\n"
			"sym\tid=2,name=\"Low\",addrsize=absolute,val=0x0801,seg=2,type=lab\n"
			"mod\tid=0,name=\"banked.o\",file=0\n";
		char *path = write_temp(k_dbg_seg_bank, "x16_dbg_info_seg_bank.dbg"); // static; do not free
		check(path && dbg_info_load(path) == 0, "loads a .dbg whose segments carry bank=");

		const char *f = NULL;
		int         l = 0;
		check(dbg_info_addr_to_source_banked_ex(0xA000, 47, &f, &l) == DBG_BANK_RESOLVED &&
		          f && strcmp(f, "a.s") == 0,
		      "bank= resolves a banked address with no LOAD seen");
		f = NULL;
		check(dbg_info_addr_to_source_banked_ex(0xA000, 66, &f, &l) == DBG_BANK_RESOLVED &&
		          f && strcmp(f, "b.s") == 0,
		      "  ...and the other bank's segment at the same address");

		dbg_addr_t a  = 0;
		int        rb = -1;
		check(dbg_info_label_to_addr_bank("EntryTwo", &a, &rb) && rb == 66,
		      "a label in a bank= segment reports that bank");
		int one = -2, two = -2, low = -2;
		for (int i = 0; i < dbg_info_symbol_count(); i++) {
			const char *nm;
			if (!dbg_info_symbol_at(i, &nm, &a))
				continue;
			if (!strcmp(nm, "EntryOne")) one = dbg_info_symbol_bank_at(i);
			if (!strcmp(nm, "EntryTwo")) two = dbg_info_symbol_bank_at(i);
			if (!strcmp(nm, "Low"))      low = dbg_info_symbol_bank_at(i);
		}
		check(one == 47 && two == 66, "symbol_bank_at gives each label its own segment's bank");
		check(low == -1, "bank= outside $A000-$BFFF is ignored");

		if (path)
			remove(path);
		dbg_info_free();
	}

	// Scope heads: a type=scope record's label opens a routine; marked at end of file.
	{
		dbg_info_free();
		static const char *k_dbg_scopes =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"s.s\",size=100,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x000801,size=0x0040,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=64,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"sym\tid=0,name=\"loop\",addrsize=absolute,scope=1,val=0x0805,seg=0,type=lab\n"
			"sym\tid=1,name=\"ClearRam\",addrsize=absolute,scope=0,val=0x0801,seg=0,type=lab\n"
			"sym\tid=2,name=\"Draw\",addrsize=absolute,scope=0,val=0x0820,seg=0,type=lab\n"
			"scope\tid=0,name=\"\",mod=0,size=64\n"
			"scope\tid=1,name=\"ClearRam\",mod=0,type=scope,size=31,parent=0,sym=1,span=0\n"
			"scope\tid=2,name=\"Draw\",mod=0,type=scope,size=32,parent=0,sym=2,span=0\n"
			"scope\tid=3,name=\"POINT\",mod=0,type=struct,size=4,parent=0\n"
			"mod\tid=0,name=\"s.o\",file=0\n";
		char *path = write_temp(k_dbg_scopes, "x16_dbg_info_scopes.dbg"); // static; do not free
		check(path && dbg_info_load(path) == 0, "loads a .dbg with scope records");
		int clr = -1, drw = -1, lp = -1;
		for (int i = 0; i < dbg_info_symbol_count(); i++) {
			const char *nm;
			dbg_addr_t  a;
			if (!dbg_info_symbol_at(i, &nm, &a))
				continue;
			if (!strcmp(nm, "ClearRam")) clr = dbg_info_symbol_is_scope_at(i);
			if (!strcmp(nm, "Draw"))     drw = dbg_info_symbol_is_scope_at(i);
			if (!strcmp(nm, "loop"))     lp  = dbg_info_symbol_is_scope_at(i);
		}
		check(clr == 1 && drw == 1, "labels named by type=scope records are scope heads");
		check(lp == 0, "a local label is not");
		if (path)
			remove(path);
		dbg_info_free();
	}

	// ── Label files, with no .dbg at all ────────────────────────────────────
	{
		dbg_info_free();
		static const char *k_labels =
			"al 000810 .start\n"
			"al 000600 .fade\r\n"
			"al 00A000 .banked\n"
			"al 000700 .twice\n"
			"al 000710 .twice\n"
			"al 000900 .__CODE_RUN__\n"
			"not a label line\n"
			"al 000000\n";

		check(dbg_info_load_labels_text("a.lbl", "nothing here\n", -1) == 0, "a file with no labels loads nothing");
		check(dbg_info_label_file_count() == 0, "and is not remembered");
		check(dbg_info_load_labels_text("a.lbl", k_labels, 3) == 5, "reads the labels, without segment symbols");
		check(!dbg_info_is_loaded(), "labels alone do not claim there are source lines");
		check(dbg_info_symbol_count() == 5, "they are symbols");

		const char *nm = NULL;
		dbg_addr_t  at = 0;
		int         bank = 0;
		check(dbg_info_addr_to_label(0x0600, &nm) && nm && !strcmp(nm, "fade"), "an address finds its label");
		check(dbg_info_addr_to_label_nearest(0x0814, &nm, &at) && !strcmp(nm, "start") && at == 0x0810,
		      "and the nearest one");
		check(dbg_info_label_to_addr_bank("BANKED", &at, &bank) && at == 0xA000 && bank == 3,
		      "a name finds its address and the file's bank");
		check(dbg_info_label_to_addr_bank("fade", &at, &bank) && bank == -1,
		      "an address below the banked windows has no bank");

		check(dbg_info_addr_to_label_in_bank(0xA000, 3, &nm) && !strcmp(nm, "banked"), "a label in its own bank");
		check(!dbg_info_addr_to_label_in_bank(0xA000, 1, &nm), "and not in another");
		check(dbg_info_addr_to_label_in_bank(0x0600, 1, &nm) && !strcmp(nm, "fade"), "an unbanked label in any");

		dbg_resolved_t r;
		char err[200];
		check(dbg_info_resolve("fade", &r, err, sizeof err) && r.addr == 0x0600, "a hex-looking word is the label");
		check(dbg_info_resolve("$fade", &r, err, sizeof err) && r.addr == 0xFADE, "$ makes it a number");
		check(dbg_info_resolve("0xFADE", &r, err, sizeof err) && r.addr == 0xFADE, "and so does 0x");
		check(dbg_info_resolve("start+4", &r, err, sizeof err) && r.addr == 0x0814, "name+decimal");
		check(dbg_info_resolve("start + $10", &r, err, sizeof err) && r.addr == 0x0820, "name+$hex, spaced");
		check(dbg_info_resolve("start-0x10", &r, err, sizeof err) && r.addr == 0x0800, "name-0xhex");
		check(dbg_info_resolve("banked+1", &r, err, sizeof err) && r.addr == 0xA001 && r.bank == 3,
		      "an offset keeps the bank");
		check(dbg_info_resolve("04:C000", &r, err, sizeof err) && r.addr == 0xC000 && r.bank == 4, "bank:addr");
		check(!dbg_info_resolve("beef", &r, err, sizeof err) && strstr(err, "$beef"),
		      "an unknown hex word says how to write the number");
		check(!dbg_info_resolve("nosuch", &r, err, sizeof err) && strstr(err, "nosuch"), "an unknown name is refused");
		check(!dbg_info_resolve("twice", &r, err, sizeof err) && strstr(err, "$0700") && strstr(err, "$0710"),
		      "an ambiguous name lists where it could be");
		check(!dbg_info_resolve("start-$900", &r, err, sizeof err), "below zero is refused");
		check(!dbg_info_resolve("$FFFFFF+1", &r, err, sizeof err), "past the address space is refused");
		check(!dbg_info_resolve("start+x", &r, err, sizeof err), "a bad offset is refused");
		check(!dbg_info_resolve("", &r, err, sizeof err), "nothing is refused");

		unsigned before = dbg_info_changes();
		dbg_info_unload_range(0, 0xFFFFFF);
		check(dbg_info_symbol_count() == 5, "a program loading over the addresses leaves label files");
		check(dbg_info_load_labels_text("a.lbl", "al 000620 .moved\n", -1) == 1, "reloading replaces");
		check(dbg_info_changes() != before, "and says something changed");
		check(dbg_info_symbol_count() == 1 && !dbg_info_label_to_addr("fade", NULL), "the old labels are gone");

		// A second file, then a .dbg over them.
		dbg_info_load_labels_text("b.lbl", "al 000801 .from_file\nal 000811 ._helper\n", -1);
		check(dbg_info_label_file_count() == 2, "two files");
		char *path = write_temp(k_dbg, "x16_dbg_info_labels.dbg");
		check(path && dbg_info_load(path) == 0, "a .dbg loads alongside");
		check(dbg_info_addr_to_label(0x0801, &nm) && !strcmp(nm, "_main"), "a .dbg symbol wins at an address");
		check(dbg_info_addr_to_label_nearest(0x0805, &nm, &at) && !strcmp(nm, "_main"), "and when nearest");
		check(dbg_info_resolve("_helper", &r, err, sizeof err) && r.addr == 0x0811, "the same name resolves once");
		check(dbg_info_label_to_addr("from_file", &at) && at == 0x0801, "a label file's own name still works");

		dbg_info_free_keep_labels();
		check(!dbg_info_is_loaded() && dbg_info_label_file_count() == 2, "replacing a .dbg keeps label files");
		check(dbg_info_label_to_addr("moved", &at) && at == 0x0620, "with their labels");
		check(!dbg_info_label_to_addr("_main", NULL), "and without the .dbg's");

		check(dbg_info_unload_labels("a.lbl") && dbg_info_label_file_count() == 1, "unloads one file");
		check(!dbg_info_label_to_addr("moved", NULL) && dbg_info_label_to_addr("from_file", NULL),
		      "leaving the other");
		check(dbg_info_load_labels_text("b.lbl", "al 000802 .again\n", -1) == 1 &&
		          dbg_info_symbol_count() == 1,
		      "and the survivor can still be replaced");
		if (path)
			remove(path);
		dbg_info_free();
		check(dbg_info_symbol_count() == 0 && dbg_info_label_file_count() == 0, "free clears everything");
	}

	// File names to lines to addresses.
	{
		static const char *k_dbg_lines =
			"version\tmajor=2,minor=0\n"
			"file\tid=0,name=\"a/main.s\",size=1,mtime=0x00000000,mod=0\n"
			"file\tid=1,name=\"b/main.s\",size=1,mtime=0x00000000,mod=0\n"
			"file\tid=2,name=\"solo.s\",size=1,mtime=0x00000000,mod=0\n"
			"seg\tid=0,name=\"CODE\",start=0x000801,size=0x0010,addrsize=absolute,type=ro\n"
			"seg\tid=1,name=\"BANKA\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"seg\tid=2,name=\"BANKB\",start=0x00a000,size=0x0010,addrsize=absolute,type=ro\n"
			"span\tid=0,seg=0,start=0,size=2,type=0\n"
			"span\tid=1,seg=0,start=2,size=2,type=0\n"
			"span\tid=2,seg=0,start=4,size=1,type=0\n"
			"span\tid=3,seg=1,start=0,size=4,type=0\n"
			"span\tid=4,seg=2,start=0,size=4,type=0\n"
			"span\tid=5,seg=0,start=8,size=1,type=0\n"
			"line\tid=0,file=0,line=1,span=0\n"
			"line\tid=1,file=1,line=1,span=1\n"
			"line\tid=2,file=2,line=5,span=5+2\n"
			"line\tid=3,file=2,line=9,span=3\n"
			"line\tid=4,file=2,line=9,span=4\n"
			"mod\tid=0,name=\"lines.o\",file=0\n";
		const char    *names[4];
		dbg_line_hit_t hits[4];
		char          *lp = write_temp(k_dbg_lines, "x16_dbg_info_lines.dbg");
		check(lp && dbg_info_load(lp) == 0, "line lookup fixture loads");
		check(dbg_info_all_file_count() == 3, "every file is counted");
		check(dbg_info_find_source("main.s", names, 4) == 2, "a bare name fits both folders");
		check(dbg_info_find_source("a/main.s", names, 4) == 1 && !strcmp(names[0], "a/main.s"),
		      "a folder picks one");
		check(dbg_info_find_source("C:\\proj\\B\\MAIN.S", names, 4) == 1 && !strcmp(names[0], "b/main.s"),
		      "an absolute path matches in any case and slash");
		check(dbg_info_find_source("c/main.s", names, 4) == 0, "another folder does not match");
		check(dbg_info_find_source("ain.s", names, 4) == 0, "only whole components match");
		check(dbg_info_line_addrs("solo.s", 5, hits, 4) == 1 && hits[0].addr == 0x0805,
		      "lowest address in a segment");
		check(dbg_info_line_addrs("solo.s", 9, hits, 4) == 2 && hits[0].addr == 0xA000 &&
		          hits[1].addr == 0xA000 && hits[0].ram_bank == -1 && hits[1].ram_bank == -1 &&
		          strcmp(hits[0].segment, hits[1].segment) != 0,
		      "segments at one address stay apart");
		check(dbg_info_line_addrs("solo.s", 4, hits, 4) == 0, "a line with no code");
		if (lp)
			remove(lp);
		dbg_info_free();
	}

	// ── C functions (csym) ──────────────────────────────────────────────────
	{
		dbg_info_free();
		check(dbg_info_cfunc_count() == 0, "csym: none before a load");
		check(dbg_info_cfunc_at_entry(0x0840) == -1 && dbg_info_cfunc_containing(0x0840) == -1,
		      "csym: lookups decline");
		dbg_cfunc_t f = {0};
		dbg_cvar_t  v = {0};
		check(!dbg_info_cfunc_at(0, &f) && !dbg_info_cfunc_var_at(0, 0, &v), "csym: by index declines");

		// Label, name and scope results with no csym, for the pin below.
		check(load_csym("x16_csym_none.dbg", 4, "", k_csym_tail) == 0, "csym: a file with none loads");
		check(dbg_info_cfunc_count() == 0, "csym: and has no functions");
		const int   pin_syms = dbg_info_symbol_count();
		bool        pin_scope[8] = {false};
		for (int i = 0; i < pin_syms && i < 8; i++) {
			pin_scope[i] = dbg_info_symbol_is_scope_at(i);
		}
		dbg_info_free();

		check(load_csym("x16_csym.dbg", 4, CSYM_FC CSYM_CD, k_csym_tail) == 0, "csym: 2.19 shape loads");
		check(dbg_info_cfunc_count() == 2, "csym: two functions, the assembly scope is not one");
		const int fc = dbg_info_cfunc_at_entry(0x0840), cd = dbg_info_cfunc_at_entry(0x0850);
		check(fc >= 0 && dbg_info_cfunc_at(fc, &f) && !strcmp(f.name, "fc") && f.entry == 0x0840 &&
		          f.var_count == 3,
		      "csym: function name, entry, variable count");
		check(cvar_is(fc, 0, "a", 1, false, false), "csym: offs read");
		check(cvar_is(fc, 1, "b", 0, false, false), "csym: missing offs is 0");
		check(cvar_is(fc, 2, "l", -1, false, false), "csym: offs=-1 is not missing");
		check(!dbg_info_cfunc_var_at(fc, 3, &v) && !dbg_info_cfunc_var_at(fc, -1, &v),
		      "csym: variable index bounded");
		check(cd >= 0 && dbg_info_cfunc_at(cd, &f) && f.var_count == 3, "csym: second function");
		check(cvar_is(cd, 0, "$anon-param-0001", 2, false, true), "csym: unnamed parameter keeps its slot");
		check(cvar_is(cd, 1, "r", 4, true, false), "csym: sc=reg with sym= is a variable");
		check(!strcmp(cfunc_name_at(0x084F), "fc") && !strcmp(cfunc_name_at(0x0850), "cd"),
		      "csym: address finds its function");
		check(dbg_info_cfunc_containing(0x1000) == -1, "csym: a span in another segment is not code");
		check(dbg_info_cfunc_containing(0x0860) == -1, "csym: nor is an assembly scope");

		// Unchanged by csym.
		check(dbg_info_symbol_count() == pin_syms, "csym: symbol count unchanged");
		for (int i = 0; i < pin_syms && i < 8; i++) {
			check(dbg_info_symbol_is_scope_at(i) == pin_scope[i], "csym: scope marks unchanged");
		}
		dbg_name_hit_t hits[4];
		dbg_resolved_t r;
		dbg_addr_t     at = 0;
		char           err[200];
		check(dbg_info_find_name("fc", hits, 4) == 0 && dbg_info_find_name("a", hits, 4) == 0,
		      "csym: C names are not found");
		check(dbg_info_find_name("_fc", hits, 4) == 1 && hits[0].addr == 0x0840, "csym: the label still is");
		check(!dbg_info_resolve("fc", &r, err, sizeof err) && !dbg_info_resolve("l", &r, err, sizeof err),
		      "csym: C names do not resolve");
		check(!dbg_info_name_to_value("b", &at, NULL) && !dbg_info_label_to_addr("b", &at),
		      "csym: nor reach label lookups");

		// Reload, rejection.
		check(load_csym("x16_csym.dbg", 4, CSYM_FC CSYM_CD, k_csym_tail) == 0, "csym: reload");
		check(dbg_info_cfunc_count() == 2 && dbg_info_cfunc_at_entry(0x0840) >= 0,
		      "csym: a reload does not duplicate");
		check(load_csym("x16_csym.dbg", 99, CSYM_FC, k_csym_tail) != 0, "csym: a short file is refused");
		check(dbg_info_cfunc_count() == 2 && dbg_info_cfunc_at_entry(0x0850) >= 0,
		      "csym: and leaves the table");
		check(load_csym("x16_csym.dbg", 4, CSYM_FC, k_csym_tail) == 0, "csym: reload with fewer");
		check(dbg_info_cfunc_count() == 1 && dbg_info_cfunc_at_entry(0x0850) == -1, "csym: replaces the old");
		dbg_info_free();
		check(dbg_info_cfunc_count() == 0, "csym: free clears");

		{
			char *only = write_temp("csym\tid=0,name=\"x\",scope=1,sc=static,sym=1\n", "x16_csym_only.dbg");
			check(only && dbg_info_load(only) != 0, "csym: alone is not a usable file");
			check(dbg_info_cfunc_count() == 0, "csym: and adds nothing");
			if (only) {
				remove(only);
			}
			dbg_info_free();
		}

		// 2.18 shape: sc=ext functions, a stray reference record.
		{
			static const char tail218[] =
				"sym\tid=30,name=\"_memcpy\",addrsize=absolute,type=imp\n";
			char buf[2048];
			snprintf(buf, sizeof buf, "%s%s", k_csym_tail, tail218);
			check(load_csym("x16_csym218.dbg", 5,
			                "csym\tid=0,name=\"fc\",scope=1,type=0,sc=ext,sym=23\n"
			                "csym\tid=1,name=\"a\",scope=1,type=0,sc=auto,offs=1\n"
			                "csym\tid=9,name=\"memcpy\",scope=0,type=0,sc=ext,sym=30\n",
			                buf) == 0,
			      "csym: 2.18 shape loads");
			const int i = dbg_info_cfunc_at_entry(0x0840);
			check(dbg_info_cfunc_count() == 1 && i >= 0 && dbg_info_cfunc_at(i, &f) &&
			          !strcmp(f.name, "fc") && f.var_count == 1,
			      "csym: sc=ext function, the reference is not one");
			dbg_info_free();
		}

		// Clashing ids across files; unload.
		check(load_csym("x16_csym_a.dbg", 4, CSYM_FC CSYM_CD, k_csym_tail) == 0 &&
		          load_csym("x16_csym_b.dbg", 1,
		                    "csym\tid=0,name=\"other\",scope=1,type=0,sc=static,sym=23\n"
		                    "csym\tid=1,name=\"o\",scope=1,type=0,sc=auto\n",
		                    k_csym_tail_b) == 0,
		      "csym: two files with the same ids");
		check(dbg_info_cfunc_count() == 3 && !strcmp(cfunc_name_at(0x0840), "fc") &&
		          !strcmp(cfunc_name_at(0x2000), "other"),
		      "csym: each id resolves in its own file");
		dbg_info_unload_range(0x2000, 0x200F);
		check(dbg_info_cfunc_count() == 2 && dbg_info_cfunc_containing(0x2000) == -1 &&
		          !strcmp(cfunc_name_at(0x0850), "cd"),
		      "csym: unload drops that module's functions");
		dbg_info_unload_range(0x0848, 0x0848);
		check(dbg_info_cfunc_at_entry(0x0840) == -1 && dbg_info_cfunc_count() == 1,
		      "csym: overlapping part of a function drops it");
		dbg_info_free_keep_labels();
		check(dbg_info_cfunc_count() == 0, "csym: free keeping labels clears");
		dbg_info_free();

		// Two functions at one address.
		check(load_csym("x16_csym_a.dbg", 4, CSYM_FC, k_csym_tail) == 0 &&
		          load_csym("x16_csym_c.dbg", 4, CSYM_FC, k_csym_tail) == 0,
		      "csym: same code from two files");
		check(dbg_info_cfunc_count() == 2 && dbg_info_cfunc_at_entry(0x0840) == -1 &&
		          dbg_info_cfunc_containing(0x0840) == -1,
		      "csym: an ambiguous address answers none");
		dbg_info_free();

		// Malformed.
		for (size_t c = 0; c < sizeof k_csym_bad / sizeof k_csym_bad[0]; c++) {
			char records[512];
			snprintf(records, sizeof records, "%s%s", k_csym_bad[c], CSYM_CD);
			check(load_csym("x16_csym_bad.dbg", 4, records, k_csym_tail) == 0, "csym: malformed still loads");
			check(dbg_info_cfunc_at_entry(0x0840) == -1, "csym: that function is left out");
			check(dbg_info_cfunc_at_entry(0x0850) >= 0, "csym: others stay");
			dbg_info_free();
		}

		// Bad targets.
		{
			static const char far_entry[] =
				"seg\tid=0,name=\"CODE\",start=0x000840,size=0x0100,addrsize=absolute,type=ro\n"
				"span\tid=0,seg=0,start=0,size=16\n"
				"scope\tid=1,name=\"_fc\",mod=0,type=scope,size=16,parent=0,sym=23,span=0\n"
				"sym\tid=23,name=\"_fc\",addrsize=absolute,val=0x900,seg=0,type=lab\n";
			check(load_csym("x16_csym_bad.dbg", 1, CSYM_FC, far_entry) == 0 && dbg_info_cfunc_count() == 0,
			      "csym: an entry outside its span is refused");
			dbg_info_free();

			static const char import[] =
				"seg\tid=0,name=\"CODE\",start=0x000840,size=0x0100,addrsize=absolute,type=ro\n"
				"span\tid=0,seg=0,start=0,size=16\n"
				"scope\tid=1,name=\"_fc\",mod=0,type=scope,size=16,parent=0,sym=23,span=0\n"
				"sym\tid=23,name=\"_fc\",addrsize=absolute,type=imp\n";
			check(load_csym("x16_csym_bad.dbg", 1, CSYM_FC, import) == 0 && dbg_info_cfunc_count() == 0,
			      "csym: an imported head is refused");
			dbg_info_free();

			static const char twice[] =
				"seg\tid=0,name=\"CODE\",start=0x000840,size=0x0100,addrsize=absolute,type=ro\n"
				"span\tid=0,seg=0,start=0,size=16\n"
				"scope\tid=1,name=\"_fc\",mod=0,type=scope,size=16,parent=0,sym=23,span=0\n"
				"scope\tid=1,name=\"_fd\",mod=0,type=scope,size=16,parent=0,sym=23,span=0\n"
				"sym\tid=23,name=\"_fc\",addrsize=absolute,val=0x840,seg=0,type=lab\n";
			check(load_csym("x16_csym_bad.dbg", 1, CSYM_FC, twice) == 0 && dbg_info_cfunc_count() == 0,
			      "csym: a repeated scope id is refused");
			dbg_info_free();

			static const char dup_sym[] =
				"seg\tid=0,name=\"CODE\",start=0x000840,size=0x0100,addrsize=absolute,type=ro\n"
				"span\tid=0,seg=0,start=0,size=16\n"
				"scope\tid=1,name=\"_fc\",mod=0,type=scope,size=16,parent=0,sym=23,span=0\n"
				"sym\tid=23,name=\"_fc\",addrsize=absolute,val=0x840,seg=0,type=lab\n"
				"sym\tid=23,name=\"_fd\",addrsize=absolute,val=0x844,seg=0,type=lab\n";
			check(load_csym("x16_csym_bad.dbg", 2, CSYM_FC, dup_sym) == 0 && dbg_info_cfunc_count() == 0,
			      "csym: a repeated symbol id is refused");
			dbg_info_free();

			static const char huge_span[] =
				"seg\tid=0,name=\"CODE\",start=0x000840,size=0x0100,addrsize=absolute,type=ro\n"
				"span\tid=0,seg=0,start=0,size=16\n"
				"scope\tid=1,name=\"_fc\",mod=0,type=scope,size=16,parent=0,sym=23,span=2147483647\n"
				"sym\tid=23,name=\"_fc\",addrsize=absolute,val=0x840,seg=0,type=lab\n";
			check(load_csym("x16_csym_a.dbg", 4, CSYM_CD, k_csym_tail) == 0 &&
			          load_csym("x16_csym_bad.dbg", 1, CSYM_FC, huge_span) == 0 &&
			          dbg_info_cfunc_at_entry(0x0840) == -1,
			      "csym: a span id at INT_MAX is refused with a nonzero base");
			dbg_info_free();

			static const char past_24[] =
				"seg\tid=0,name=\"CODE\",start=0xFFFFF0,size=0x0100,addrsize=absolute,type=ro\n"
				"span\tid=0,seg=0,start=0,size=256\n"
				"scope\tid=1,name=\"_fc\",mod=0,type=scope,size=16,parent=0,sym=23,span=0\n"
				"sym\tid=23,name=\"_fc\",addrsize=absolute,val=0xFFFFF0,seg=0,type=lab\n";
			check(load_csym("x16_csym_bad.dbg", 1, CSYM_FC, past_24) == 0 && dbg_info_cfunc_count() == 0,
			      "csym: a span past 24 bits is refused");
			dbg_info_free();
		}

		// Oversized.
		{
			const size_t cap   = 128 * 1100;
			char        *text  = (char *)malloc(cap);
			char        *tail  = (char *)malloc(4096);
			check(text && tail, "csym: test buffers");
			if (text && tail) {
				char name[300];
				memset(name, 'n', 255);
				name[255] = '\0';
				snprintf(text, cap,
				         "csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\n"
				         "csym\tid=1,name=\"%s\",scope=1,sc=auto\n", name);
				check(load_csym("x16_csym_big.dbg", 4, text, k_csym_tail) == 0 &&
				          dbg_info_cfunc_at_entry(0x0840) >= 0,
				      "csym: a 255-character name is kept");
				dbg_info_free();
				memset(name, 'n', 256);
				name[256] = '\0';
				snprintf(text, cap,
				         "csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\n"
				         "csym\tid=1,name=\"%s\",scope=1,sc=auto\n", name);
				check(load_csym("x16_csym_big.dbg", 4, text, k_csym_tail) == 0 &&
				          dbg_info_cfunc_at_entry(0x0840) == -1,
				      "csym: a longer one refuses the function");
				dbg_info_free();

				for (int count = 1024; count <= 1025; count++) {
					size_t o = (size_t)snprintf(text, cap, "csym\tid=0,name=\"fc\",scope=1,sc=static,sym=23\n");
					for (int i = 1; i <= count; i++) {
						o += (size_t)snprintf(text + o, cap - o, "csym\tid=%d,name=\"v%d\",scope=1,sc=auto,offs=-%d\n",
						                      i, i, i);
					}
					check(load_csym("x16_csym_big.dbg", 4, text, k_csym_tail) == 0, "csym: many variables load");
					const int i = dbg_info_cfunc_at_entry(0x0840);
					check(count == 1024 ? (i >= 0 && dbg_info_cfunc_at(i, &f) && f.var_count == 1024) : i == -1,
					      count == 1024 ? "csym: 1024 variables kept" : "csym: 1025 refuses the function");
					dbg_info_free();
				}

				for (int count = 64; count <= 65; count++) {
					size_t o = (size_t)snprintf(tail, 4096,
					                            "seg\tid=0,name=\"CODE\",start=0x000840,size=0x0100,addrsize=absolute,type=ro\n"
					                            "span\tid=0,seg=0,start=0,size=16\n"
					                            "scope\tid=1,name=\"_fc\",mod=0,type=scope,size=16,parent=0,sym=23,span=0");
					for (int i = 1; i < count; i++) {
						o += (size_t)snprintf(tail + o, 4096 - o, "+0");
					}
					snprintf(tail + o, 4096 - o, "\nsym\tid=23,name=\"_fc\",addrsize=absolute,val=0x840,seg=0,type=lab\n");
					check(load_csym("x16_csym_big.dbg", 1, CSYM_FC, tail) == 0, "csym: long span list loads");
					check((dbg_info_cfunc_at_entry(0x0840) >= 0) == (count == 64),
					      count == 64 ? "csym: 64 spans kept" : "csym: 65 refuses the function");
					dbg_info_free();
				}
			}
			free(text);
			free(tail);
		}
	}

	// ── Change hook ─────────────────────────────────────────────────────────
	{
		dbg_info_free();
		dbg_info_set_change_hook(count_change);
		check(load_csym("x16_hook.dbg", 4, CSYM_FC, k_csym_tail) == 0 && g_change_calls == 1, "hook: a load");
		check(load_csym("x16_hook.dbg", 99, CSYM_FC, k_csym_tail) != 0 && g_change_calls == 1,
		      "hook: a refused load changes nothing");
		dbg_info_unload_range(0x0840, 0x084F);
		check(g_change_calls == 2, "hook: an unload");
		check(dbg_info_load_labels_text("hook.lbl", "al 000700 .x\n", -1) == 1 && g_change_calls == 3,
		      "hook: a label file");
		check(dbg_info_unload_labels("hook.lbl") && g_change_calls == 4, "hook: removing it");
		dbg_info_free_keep_labels();
		check(g_change_calls == 5, "hook: free keeping labels");
		dbg_info_free();
		check(g_change_calls == 6, "hook: free");
		dbg_info_set_change_hook(NULL);
		check(load_csym("x16_hook.dbg", 4, CSYM_FC, k_csym_tail) == 0 && g_change_calls == 6, "hook: removed");
		dbg_info_free();
	}

#ifndef _WIN32
	// ── A FIFO named as debug info ──────────────────────────────────────────
	{
		static const char dir[] = "fifo_info_dir", real[] = "fifo_info_dir/real.dbg",
		                  fifo[] = "fifo_info_dir/prog.dbg", prog[] = "fifo_info_dir/prog.prg";
		remove(fifo);
		remove(real);
		rmdir(dir);
		FILE *rf = NULL;
		if (mkdir(dir, 0700) != 0 || mkfifo(fifo, 0600) != 0) {
			printf("skip: no FIFOs on this filesystem\n");
		} else if ((rf = fopen(real, "w")) != NULL) {
			fputs(k_dbg, rf);
			fclose(rf);
			check(dbg_info_load(real) == 0, "fifo: a regular .dbg loads");

			char       found[4][DBG_INFO_PATH_MAX];
			dbg_addr_t lo = 0, hi = 0;
			alarm(10); // a blocked open ends the test
			check(dbg_info_load(fifo) != 0, "fifo: dbg_info_load refuses it");
			check(dbg_info_load_labels(fifo, -1) < 0, "fifo: dbg_info_load_labels refuses it");
			check(!dbg_info_peek_file_range(prog, &lo, &hi), "fifo: the range peek refuses it");
			check(dbg_info_load_for_file(prog, 0x0801) != 0, "fifo: the load beside a program refuses it");
			alarm(0);
			check(dbg_info_loaded_count() == 1 && !strcmp(dbg_info_loaded_at(0), real),
			      "fifo: what was loaded stays");
			check(dbg_info_scan_dbg_files(dir, found, 4) == 1 && !strcmp(found[0], real),
			      "fifo: a folder scan offers only the regular file");
			dbg_info_free();
		}
		remove(fifo);
		remove(real);
		rmdir(dir);
	}
#endif

	printf("\n%s (%d failure%s)\n", g_fails ? "FAILED" : "PASSED", g_fails,
	       g_fails == 1 ? "" : "s");
	return g_fails ? 1 : 0;
}
