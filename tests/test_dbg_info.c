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
// Returns a path valid until this is called WRITE_TEMP_SLOTS more times.
// It used to hand back one static buffer, so a caller that wrote a second
// fixture before removing the first removed the wrong file -- and a caller that
// wrote three before loading any would have loaded the last one three times.
// Rotating the buffers makes holding a few paths at once safe, which is what
// every multi-module fixture here needs.
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
	// Deduplication is only correct within one module. Two linked modules may
	// each define the same equate name with different values, and may each
	// compile a file of the same name. Pooling those would not merely report a
	// stale number -- it destroys one module's definition outright, since
	// nothing puts it back when the other module unloads.
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

	// A `line` that precedes the `file` it names. cc65 emits them the other way
	// round today, but nothing in the format promises that, and resolving the
	// reference before the record exists silently loses the source mapping.
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

			// The reload is what actually exercises the ordering: only then has
			// the ID base moved on, so the reused file record's ID and the one
			// a `line` would assume for itself are different numbers.
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

	// A segment whose name is a prefix of another's. Matching an equate to a
	// segment by prefix in either direction means RAM_BANK_CODE matches CODE2
	// as readily as CODE, so whichever equate happened to be parsed first used
	// to claim both -- and the exact match for the second was then discarded,
	// putting its code confidently in the wrong bank.
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

	// Two equates that both prefix-match one segment, with no exact match and
	// different banks. There is no basis for choosing between them, so the
	// segment must be left unknown: unknown is recoverable -- a runtime bank
	// observation can still resolve it -- whereas a confident wrong answer is
	// not, and it silently attributes code to the wrong bank.
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

	// Bank equates are owned too. Two overlays commonly each define
	// RAM_BANK_CODE for their own CODE segment, with different values. Each
	// module's own equate has to win for its own segment: pooling them lets the
	// second module's bank decide where the first module's code lives, and
	// merely refusing to choose would leave both unknown.
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

	// Files are owned as well. Two modules that each compile a file of the same
	// name must keep two records; consumers resolve by name today, so the count
	// is what makes the pooling visible.
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

	// A segment can be given its bank by another module's equate, through the
	// fallback pass. Those segments are not unloaded when that other module
	// reloads, so unless equate-derived banks are recomputed they keep a value
	// nobody declares any more.
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

	// Re-deriving equate banks must not discard what was actually observed at
	// runtime. An observation is evidence; an equate is only an inference.
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

	// A module's own equate must not outrank a better match from elsewhere.
	// An own-module prefix match used to end the search before another
	// module's exact match was considered, which is the same "claimed by a name
	// it resembles" failure the ranking exists to prevent.
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

	// Two equates in ONE module that both reduce to the segment's name, with
	// different banks. The module contradicts itself, so there is nothing to
	// choose between them.
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

	// A rebuilt module that no longer declares an equate must not go on
	// declaring it. Replacing equates one by one as they are parsed cannot see
	// this: the record that needs removing is never visited.
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
		// Same module, rebuilt with both equates removed. It still has a label,
		// as any real linked module does -- that is what distinguishes it from
		// a write that was cut off before the symbols.
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

	// Names too long to reduce are refused rather than truncated: a truncated
	// name compares equal to anything sharing its head, inventing exact matches
	// the full names do not support.
	{
		dbg_info_free();
		// The equate's target is exactly as long as the squash buffer allows,
		// so it is accepted. The segment name shares that whole head but runs
		// past the buffer, so truncating it would make the two compare equal
		// even though the full names differ.
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

	// An ambiguous winning rank does not fall through to a weaker one. The
	// better evidence has just said the question is undecidable; answering it
	// from worse evidence would be inventing certainty.
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

	// A file that opens but parses to nothing must not take the previous
	// generation with it. Sweeping the old equates before reading meant an
	// empty or corrupt rebuild silently deleted them and still reported
	// success -- the emulator writes these files while a program is running,
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

	// An equate name too long for the reduction buffer must be refused, not
	// copied into it. The name comes straight out of a file sitting next to a
	// loaded program, so its length is not ours to assume.
	//
	// This is a smoke test, not a guard: squash_name() refuses the name either
	// way, so the resolution outcome is the same with or without the length
	// check, and only a sanitizer can see the difference. Removing the check
	// turns this input into a 300-byte write into a 128-byte stack buffer.
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

	// ld65 states how many symbols it wrote. That is what separates a file cut
	// off partway through its symbol block from one that genuinely declares
	// fewer -- the two have the same shape and differ only in the promise.
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

			// Cut inside the symbol block rather than before it. For a real
			// program the symbols are the bulk of the file, so this is where a
			// partial write is most likely to land -- and the shape the
			// previous guard, which only asked whether any symbol was seen,
			// could not tell from a finished file.
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

			// A symbol line cut off mid-record is not a symbol. Counting it
			// would let a file that stopped halfway look complete.
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

	// The same name can be filed as a bank constant or an ordinary one
	// depending on its value, so a redefinition can cross between the two
	// tables. The later definition has to be the only one left.
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

	// A .dbg with no `info` line gives no record counts to check against, so a
	// short read cannot be detected and the file is merged. It must still not
	// be treated as a statement that the module declares nothing.
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
	// ld65 counts every `sym` record in its `info` line, so the check for a
	// half-written file has to count the records it SAW, not the ones it chose
	// to store. Two shapes broke that, and either one refused the whole file:
	// an import, which names a symbol another module defines and so carries no
	// `val`; and a constant outside the 24-bit space the tables describe --
	// C's EOF is -1, written as 0xFFFFFFFF, so every program that includes
	// <stdio.h> has one.
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
	// Compiling C goes hello.c -> hello.s -> ca65, and with -g ca65 records BOTH
	// line infos over the same bytes: a type=1 record naming the C statement and
	// a type=0 record per instruction naming the generated .s. The per-
	// instruction assembly spans are always the smaller, so the "innermost span
	// wins" rule that is right for assembly inverts here and would step through
	// a generated file that cl65 deletes on the way out.
	//
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

		// A hand-written .s that IS on disk must survive the picker filter, so
		// it has to exist next to the .dbg before the load looks for it.
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

			// The case an equal-start-only preference cannot reach: an interior
			// instruction boundary. The C statement's span opened at $0801, so
			// nothing that starts at $0804 knows about it -- and $0804 is where
			// stepping actually lands.
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

			// The exact-match form feeds the DAP disassembly view, which must
			// not name a file the picker hides.
			f = NULL; n = 0;
			check(dbg_info_addr_to_source(0x0804, &f, &n)
			          && f && strstr(f, "x16hello.c") != NULL && n == 5,
			      "the exact-address lookup also prefers the C line");

			// A C statement and an assembly span with identical bounds: the
			// type has to decide, since the span size cannot.
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

			// The per-instruction assembly spans are the anchored
			// disassembler's alignment anchors and must stay in the map, however
			// little the source view wants to display them.
			check(dbg_info_is_span_start(0x0804), "keeps an assembly span start as an anchor");
			check(dbg_info_is_span_start(0x0809), "keeps the later ones too");
			check(!dbg_info_is_span_start(0x0807), "and still rejects a mid-span address");

			// The picker offers what the user wrote. x16hello.s is generated and
			// gone; x16util.s has no C source it could have been generated from;
			// x16mixed.s is paired with a C file but is on disk, so somebody
			// wanted it.
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
				// cl65 doing compile-and-assemble in one step names the
				// intermediate after the whole C file plus a temporary suffix,
				// which shares no stem with it. This is what a real
				// `cl65 -t cx16 -g` build produces.
				check(has_gen_c, "  ...keeps the C source of a one-step build");
				check(!has_gen_tmp, "  ...hides cl65's temporary intermediate");

				// Indices stay dense: a caller walking 0..count-1 must not hit
				// a gap where the hidden file used to be.
				const char *nm = NULL;
				check(dbg_info_file_at(fc - 1, &nm) && nm, "the last visible index resolves");
				check(!dbg_info_file_at(fc, &nm), "and one past the end does not");
			}

			// Unloading the C module takes the high-level info with it, so
			// nothing stays hidden on account of a module that has gone.
			dbg_info_unload_range(0x0801, 0x0840);
			check(dbg_info_file_count() == 7,
			      "unloading the C module puts every file back on offer");

			remove(cpath);
			if (mixed_asm)
				remove(mixed_asm);
			dbg_info_free();
		}
	}

	// An assembly-only .dbg must be untouched by all of the above: no high-level
	// records, so nothing is preferred and nothing is hidden. cc65's own runtime
	// files (cbm/loadaddr.s and friends) are exactly the ".s referenced only by
	// assembly records and not on disk" shape the filter looks for, and they
	// must keep showing up for an assembly project.
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
	// A .dbg caught mid-write is refused, but its records are already in the
	// tables and the address map is rebuilt on the way out. If deciding
	// "this program is written in C" happened there, a half-written C .dbg --
	// which is exactly what the auto-load path finds while cl65 is still
	// writing -- would switch every already-loaded assembly module over to the
	// C lookup, for good: the refused records stay behind, so every later
	// rebuild would keep agreeing with it.
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

	printf("\n%s (%d failure%s)\n", g_fails ? "FAILED" : "PASSED", g_fails,
	       g_fails == 1 ? "" : "s");
	return g_fails ? 1 : 0;
}
