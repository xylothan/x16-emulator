// Parser checks for the cc65 debug-info reader.
//
// Standalone: dbg_info.c has no dependency on the emulator core, so this links
// it on its own. No SDL, ROM or emulator required. Returns 0 when every check
// passes.
//
// Build: configure with -DBUILD_DBG_INFO_TEST=ON, then build target
// dbg_info_test.
#include "dbg_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// A minimal but realistic cc65 .dbg file. Two source files so file/line lookup
// has something to disambiguate, two spans so mid-span resolution is
// meaningful, and two labels so symbol lookup and ordering are exercised.
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

// Two loads of the same module where an equate's value changed. Nothing prunes
// equates, and the lookup returns the first match, so appending rather than
// replacing lets the old value win for the rest of the session.
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
static char *
write_temp(const char *contents, const char *leaf)
{
	static char path[512];
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir) {
		dir = getenv("TEMP");
	}
	if (!dir || !*dir) {
		dir = ".";
	}
	snprintf(path, sizeof(path), "%s/%s", dir, leaf);

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

// Two segments sharing the banked window at $A000, one per RAM bank, each
// attributed to its own source file. Different sizes are what lets a LOAD tell
// them apart.
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
	"mod\tid=0,name=\"banked.o\",file=0\n";

static char *
write_temp_banked_dbg(void)
{
	return write_temp(k_dbg_banked, "x16_dbg_info_banked.dbg");
}

int
main(void)
{
	// Nothing loaded yet: every lookup should decline rather than invent an
	// answer, and none of them should trip over the empty tables.
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

	// Landing mid-instruction must still resolve; this is what keeps the
	// current-line highlight steady while stepping.
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

		// Enumeration is sorted by address, which the symbol list relies on.
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

	// Source files referenced by the .dbg, so a UI can offer them before the
	// PC has ever been there.
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
	// Freeing twice must be harmless; shutdown paths are not always ordered.
	dbg_info_free();
	check(1, "double free is harmless");

	// A load that fails must not disturb what is already loaded. The obvious
	// ordering bug here is clearing the address map before opening the file.
	check(dbg_info_load(path) == 0, "reloads after free");
	check(dbg_info_load("this/does/not/exist.dbg") != 0, "second load of a missing file fails");
	{
		const char *file = NULL;
		int         line = 0;
		check(dbg_info_addr_to_source(0x0801, &file, &line),
		      "a failed load leaves the existing mapping intact");
	}

	// Reloading a module, which is what an overlay swap does, must replace it
	// rather than accumulate a second copy of its segments -- duplicates make
	// bank disambiguation ambiguous and it gives up.
	{
		const int files_before = dbg_info_file_count();
		dbg_info_unload_range(0x0801, 0x0840);
		check(dbg_info_load(path) == 0, "reloads the same module");
		const char *file = NULL;
		int         line = 0;
		check(dbg_info_addr_to_source(0x0801, &file, &line),
		      "the reloaded module still resolves");
		check(line == 10, "  ...to the original line");

		// The point of the block, which it used to measure and then discard.
		// Unloading a range deliberately leaves file records alone, so nothing
		// stops a reload appending a second copy of every one of them; the same
		// is true of equates, where the lookup returns the FIRST match and a
		// stale value would therefore win for the rest of the session.
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
	// Two segments share the banked window at $A000, one per RAM bank, which is
	// the normal shape of a banked X16 program. The .dbg format records no
	// bank, so they are told apart by the size of the blob each LOAD placed
	// there. A swap must replace a module rather than accumulate a second copy
	// of its segments: with duplicates present nothing can be told apart any
	// more, and every banked address resolves to whichever module matched first.
	{
		char *bpath = write_temp_banked_dbg();
		if (!bpath) {
			return 1;
		}
		check(dbg_info_load(bpath) == 0, "loads a banked .dbg");

		// Before any LOAD is observed, neither segment's bank is known. The
		// lookup must still answer -- a mapping is never lost -- but it has to
		// say the answer is a guess, because either segment could be the one
		// actually mapped in at $A000.
		const char *fu = NULL;
		int         lu = 0;
		check(dbg_info_addr_to_source_banked_ex(0xA000, 5, &fu, &lu) == DBG_BANK_UNKNOWN,
		      "flags a match against an unknown-bank segment as a guess");
		check(dbg_info_addr_to_source_banked(0xA000, 5, &fu, &lu),
		      "still reports the mapping for an unknown-bank segment");

		// Learning one segment's bank must not make the other one confident:
		// $A018 lies only inside the larger segment, whose bank is still unknown.
		dbg_info_note_bank_load(0xA000, 0x10, 5);
		check(dbg_info_addr_to_source_banked_ex(0xA000, 5, &fu, &lu) == DBG_BANK_RESOLVED,
		      "reports the newly learned segment as resolved");
		check(dbg_info_addr_to_source_banked_ex(0xA018, 5, &fu, &lu) == DBG_BANK_UNKNOWN,
		      "keeps a still-unknown segment flagged as a guess");

		dbg_info_note_bank_load(0xA000, 0x20, 6);

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

		// A bank nothing was ever observed for must NOT be passed off as
		// resolved: the answer comes from another bank's segment.
		check(dbg_info_addr_to_source_banked_ex(0xA000, 9, &f5, &l5) == DBG_BANK_UNKNOWN,
		      "reports an unlearned bank as unknown rather than guessing");

		// And an address with no debug info at all is still a plain miss.
		check(dbg_info_addr_to_source_banked_ex(0x5000, 5, &f5, &l5) == DBG_BANK_NO_MATCH,
		      "reports an unmapped address as no match");

		dbg_info_free();
		remove(bpath);
	}

	// ── 24-bit addresses ────────────────────────────────────────────────────
	// Code on a GS/Gen2 machine lives above $FFFF. A 16-bit address type simply
	// could not describe it, so those mappings used to be dropped at load.
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
	// cc65 lists every reference to a symbol on its `sym` line, so for a
	// heavily used label that line runs to many kilobytes -- and val/type come
	// after the list. A fixed read buffer would truncate it and silently lose
	// the symbol.
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
	// Two segments share $A000, one per bank. Only the OUTER one has been
	// loaded, so only its bank is known. Preferring the innermost span
	// regardless answers with the inner segment's file and line and downgrades
	// a confirmed match to a guess -- the wrong source, reported confidently
	// enough to send someone debugging code that is fine.
	//
	// The block above learns the inner segment's bank first, which is the case
	// that already worked, so this needs its own load to reach the other order.
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
	// Nothing prunes equates, and the lookup returns the FIRST match, so
	// appending on reload would let a value from a module swapped out long ago
	// win for the rest of the session -- and the bank equates seed which RAM
	// bank a segment lives in, so a stale one mislabels code.
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

				// Bank equates have no accessor of their own; what they do is
				// seed which RAM bank a segment is taken to live in, so a stale
				// one mislabels code rather than merely reporting a wrong
				// number. The segment is in the banked window and named to match
				// the equate, so the reload's bank 7 must be the one in force.
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
	printf("\n%s (%d failure%s)\n", g_fails ? "FAILED" : "PASSED", g_fails,
	       g_fails == 1 ? "" : "s");
	return g_fails ? 1 : 0;
}
