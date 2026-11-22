// Checks for runtime debug-info loading (src/dbg_load.c).

#include "dbg_load.h"
#include "dbg_info.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void
check(bool cond, const char *what)
{
	if (!cond) {
		failures++;
		printf("FAIL: %s\n", what);
	} else {
		printf("ok  : %s\n", what);
	}
}

// MACPTR-style transfer: first destination kept, RAM bank steps at the window end.
static void
transfer(int channel, uint16_t addr, uint8_t bank, uint32_t bytes, bool to_eof)
{
	for (uint32_t i = 0; i < bytes; i++) {
		bool last = to_eof && (i + 1 == bytes);
		dbg_load_note_byte(channel, addr, bank, last);
		addr++;
		if (addr == 0xC000) {
			addr = 0xA000;
			bank++;
		}
	}
}

// Write a file, returning false if it could not be created.
static bool
write_file(const char *path, const char *text, size_t len)
{
	FILE *f = NULL;
#ifdef _MSC_VER
	if (fopen_s(&f, path, "wb") != 0)
		f = NULL;
#else
	f = fopen(path, "wb");
#endif
	if (!f)
		return false;
	size_t wrote = fwrite(text, 1, len, f);
	fclose(f);
	return wrote == len;
}

// Minimal cc65 .dbg: one 300-byte CODE segment at $A000, named source file.
static bool
write_dbg(const char *path, const char *source_name, unsigned seg_start)
{
	char text[1024];
	int n = snprintf(text, sizeof(text),
		"version\tmajor=2,minor=0\n"
		"file\tid=0,name=\"%s\",size=10,mtime=0x00000000,mod=0\n"
		"line\tid=0,file=0,line=1,span=0\n"
		"mod\tid=0,file=0\n"
		"scope\tid=0,mod=0,type=global,name=\"\",size=300,span=0\n"
		"seg\tid=0,name=\"CODE\",start=0x%06X,size=0x012C,addrsize=absolute,type=ro,oname=\"ovl.prg\"\n"
		"span\tid=0,seg=0,start=0,size=1,type=0\n"
		"sym\tid=0,name=\"OVL_START\",addrsize=absolute,size=1,scope=0,def=0,val=0x%04X,type=lab\n",
		source_name, seg_start, seg_start);
	if (n <= 0 || (size_t)n >= sizeof(text))
		return false;
	return write_file(path, text, (size_t)n);
}

int
main(void)
{
	// ── Off by default ──────────────────────────────────────────────────────
	// Off until asked for: loads choose which host files get parsed.
	{
		dbg_load_reset();
		dbg_load_set_enabled(false);
		check(!dbg_load_is_enabled(), "starts disabled");

		dbg_load_begin(1, "/host/GAME.PRG");
		transfer(1, 0x0801, 0, 64, true);

		dbg_load_event_t ev;
		check(!dbg_load_take(&ev), "collects nothing at all while disabled");
	}

	// ── A plain load ────────────────────────────────────────────────────────
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		check(dbg_load_is_enabled(), "can be enabled");

		dbg_load_begin(1, "/host/GAME.PRG");
		transfer(1, 0x0801, 0, 100, true);

		dbg_load_event_t ev;
		check(dbg_load_take(&ev), "reports a completed load");
		check(strcmp(ev.path, "/host/GAME.PRG") == 0, "with the host path it came from");
		check(ev.addr == 0x0801, "and where it started");
		check(ev.size == 100, "and how much went into memory");

		check(!dbg_load_take(&ev), "and reports it only once");
	}

	// ── The bank is the one the load started in ─────────────────────────────
	// A load overrunning $BFFF continues at $A000 in the next bank.
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);

		// Starts near the top of bank 5, finishes in bank 6.
		dbg_load_begin(2, "/host/OVERLAY.PRG");
		transfer(2, 0xBF00, 5, 0x200, true);  // 512 bytes: 256 in bank 5, 256 in bank 6

		dbg_load_event_t ev;
		check(dbg_load_take(&ev), "reports a load that crossed a bank boundary");
		check(ev.addr == 0xBF00, "starting where it started");
		check(ev.bank == 5, "in the bank it started in, not the one it ended in");
		check(ev.size == 0x200, "having counted every byte");
	}

	// ── Two channels open at once ───────────────────────────────────────────
	// The second open must not overwrite the first.
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);

		dbg_load_begin(1, "/host/FIRST.PRG");
		transfer(1, 0x0801, 0, 9, false);       // first channel starts

		dbg_load_begin(2, "/host/SECOND.PRG");
		transfer(2, 0xA000, 3, 19, false);      // second opens and transfers too

		dbg_load_note_byte(1, 0x080A, 0, true);   // first finishes: 10 bytes
		dbg_load_event_t ev;
		check(dbg_load_take(&ev), "reports the first channel's load");
		check(strcmp(ev.path, "/host/FIRST.PRG") == 0, "with its own name");
		check(ev.addr == 0x0801 && ev.size == 10, "and its own address and size");

		dbg_load_note_byte(2, 0xA013, 3, true);   // then the second: 20 bytes
		check(dbg_load_take(&ev), "reports the second channel's load");
		check(strcmp(ev.path, "/host/SECOND.PRG") == 0, "with its own name too");
		check(ev.addr == 0xA000 && ev.bank == 3 && ev.size == 20,
		      "and its own address, bank and size");
	}

	// ── Reading a data file is not loading a program ────────────────────────
	// No bytes reported, no load: only MACPTR calls note_byte, GET#/INPUT# use ACPTR.
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);

		dbg_load_begin(1, "/host/SCORES.DAT");
		dbg_load_abandon(1);             // read to EOF, no byte ever reported

		dbg_load_event_t ev;
		check(!dbg_load_take(&ev), "a channel that reports no bytes is not a load");

		// Same channel and path, reporting bytes: a load.
		dbg_load_begin(1, "/host/SCORES.DAT");
		transfer(1, 0x0801, 0, 8, true);
		check(dbg_load_take(&ev) && ev.size == 8,
		      "and the same channel reporting bytes is one");
	}

	// ── A channel closed without finishing ──────────────────────────────────
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);

		dbg_load_begin(1, "/host/PARTIAL.PRG");
		transfer(1, 0x0801, 0, 50, false);
		dbg_load_abandon(1);             // closed early, never completed

		dbg_load_event_t ev;
		check(!dbg_load_take(&ev), "an abandoned transfer reports nothing");

		// The close forgets the channel: a later byte completes nothing.
		dbg_load_note_byte(1, 0x0801, 0, true);
		check(!dbg_load_take(&ev), "and a byte after the close starts nothing");

		// And the channel is reusable afterwards.
		dbg_load_begin(1, "/host/NEXT.PRG");
		transfer(1, 0x2000, 0, 8, true);
		check(dbg_load_take(&ev) && strcmp(ev.path, "/host/NEXT.PRG") == 0,
		      "and the channel still works afterwards");
	}

	// ── Bytes on a channel that never opened ────────────────────────────────
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		dbg_load_note_byte(3, 0x1000, 0, true);
		dbg_load_event_t ev;
		check(!dbg_load_take(&ev), "a byte on an unopened channel reports nothing");

		// Nor a whole transfer's worth of them.
		transfer(4, 0x1000, 0, 100, true);
		check(!dbg_load_take(&ev), "nor can a transfer on one invent a load");
	}

	// ── Out-of-range channels are ignored, not written through ──────────────
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		dbg_load_begin(-1, "/host/X.PRG");
		dbg_load_begin(DBG_LOAD_CHANNELS, "/host/X.PRG");
		dbg_load_begin(9999, "/host/X.PRG");
		dbg_load_note_byte(-1, 0, 0, false);
		dbg_load_note_byte(9999, 0, 0, true);
		dbg_load_note_byte(DBG_LOAD_CHANNELS, 0, 0, true);
		dbg_load_abandon(-1);
		dbg_load_abandon(9999);
		dbg_load_event_t ev;
		check(!dbg_load_take(&ev), "an out-of-range channel is ignored");
	}

	// ── Only the first destination is kept ──────────────────────────────────
	// MACPTR reports every byte; the first is where the program begins.
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		dbg_load_begin(1, "/host/P.PRG");
		dbg_load_note_byte(1, 0x0801, 0, false);
		dbg_load_note_byte(1, 0x9999, 7, true);   // later bytes must not move it

		dbg_load_event_t ev;
		check(dbg_load_take(&ev) && ev.addr == 0x0801 && ev.bank == 0,
		      "keeps the first destination, not the last");
	}

	// ── Reset clears everything in flight ───────────────────────────────────
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		dbg_load_begin(1, "/host/A.PRG");
		transfer(1, 0x0801, 0, 10, true);   // a completed load is now pending
		dbg_load_begin(2, "/host/B.PRG");
		transfer(2, 0x2000, 0, 10, false);      // and another is in flight

		dbg_load_reset();

		dbg_load_event_t ev;
		check(!dbg_load_take(&ev), "reset drops a completed load");
		dbg_load_note_byte(2, 0x200A, 0, true);
		check(!dbg_load_take(&ev), "and one that was in flight");
	}

	// ── A very long path is refused, not truncated ──────────────────────────
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		char longpath[DBG_LOAD_PATH_MAX + 64];
		memset(longpath, 'a', sizeof(longpath) - 1);
		longpath[sizeof(longpath) - 1] = '\0';

		dbg_load_begin(1, longpath);
		transfer(1, 0x0801, 0, 4, true);

		// Refused: a truncated path names a different host file.
		dbg_load_event_t ev;
		check(!dbg_load_take(&ev), "an over-long path is refused, not truncated");

		// One character shorter than the buffer fits.
		char fits[DBG_LOAD_PATH_MAX];
		memset(fits, 'a', sizeof(fits) - 1);
		fits[sizeof(fits) - 1] = '\0';
		dbg_load_begin(1, fits);
		transfer(1, 0x0801, 0, 4, true);
		check(dbg_load_take(&ev) && strcmp(ev.path, fits) == 0,
		      "a path that exactly fits is kept whole");
	}

	// ── Every channel works ─────────────────────────────────────────────────
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		bool all_ok = true;
		for (int ch = 0; ch < DBG_LOAD_CHANNELS; ch++) {
			char p[64];
			snprintf(p, sizeof(p), "/host/CH%d.PRG", ch);
			dbg_load_begin(ch, p);
			transfer(ch, (uint16_t)(0x1000 + ch), 0, 4, true);

			dbg_load_event_t ev;
			if (!dbg_load_take(&ev) || strcmp(ev.path, p) != 0
			    || ev.addr != (uint16_t)(0x1000 + ch)) {
				all_ok = false;
			}
		}
		check(all_ok, "every channel reports its own load");
	}

	// ── The final byte is counted ───────────────────────────────────────────
	// Reporting the last byte finishes the load, so the count includes it.
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		dbg_load_begin(1, "/host/P.PRG");
		for (int i = 0; i < 9; i++)
			dbg_load_note_byte(1, (uint16_t)(0x0801 + i), 0, false);
		dbg_load_note_byte(1, 0x080A, 0, true);   // the tenth byte ends the file

		dbg_load_event_t ev;
		check(dbg_load_take(&ev) && ev.size == 10,
		      "the byte that ends the file is counted");
	}

	// ── Nothing is reported after the load finishes ─────────────────────────
	// Reports after the last byte belong to no load.
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		dbg_load_begin(1, "/host/P.PRG");
		dbg_load_note_byte(1, 0x0801, 0, true);

		dbg_load_event_t ev;
		check(dbg_load_take(&ev) && ev.size == 1, "a one-byte file is a load");

		dbg_load_note_byte(1, 0x0802, 0, true);   // stray report after the end
		check(!dbg_load_take(&ev), "a stray byte afterwards starts nothing");
	}
	// ── A .dbg loaded as if it were a program ───────────────────────────────
	// A wildcard can match GAME.dbg; it is refused as debug info for itself.
	{
		check(dbg_load_path_is_dbg("/host/GAME.dbg"), "recognises a .dbg path");
		check(dbg_load_path_is_dbg("/host/GAME.DBG"), "whatever its case");
		check(dbg_load_path_is_dbg("GAME.Dbg"),       "with no directory");
		check(!dbg_load_path_is_dbg("/host/GAME.PRG"), "a .prg is not one");
		check(!dbg_load_path_is_dbg("/host/GAME.dbginfo"), "nor a longer suffix");
		check(!dbg_load_path_is_dbg("/host/dbg"),      "nor a bare name");
		check(dbg_load_path_is_dbg(".dbg"),            "a bare extension counts");
		check(!dbg_load_path_is_dbg(""),               "an empty path is not");
		check(!dbg_load_path_is_dbg(NULL),             "nor a missing one");
	}

	// ── The -prg file ───────────────────────────────────────────────────────
	// -prg bypasses filename resolution, so the path is recorded up front.
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		dbg_load_set_prg_path("/host/GAME.PRG");

		dbg_load_begin_prg(1);
		transfer(1, 0xA000, 2, 128, true);

		dbg_load_event_t ev;
		check(dbg_load_take(&ev), "reports the -prg load");
		check(strcmp(ev.path, "/host/GAME.PRG") == 0, "with the path -prg was given");
		check(ev.addr == 0xA000 && ev.bank == 2 && ev.size == 128,
		      "and where it landed");
	}

	// ── No -prg file ────────────────────────────────────────────────────────
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		dbg_load_set_prg_path(NULL);

		dbg_load_begin_prg(1);
		transfer(1, 0xA000, 2, 16, true);

		dbg_load_event_t ev;
		check(!dbg_load_take(&ev), "no -prg path means no load is reported");
	}

	// ── A -prg path too long to hold ────────────────────────────────────────
	// Truncating it would name a different file, so it is refused outright.
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		char longpath[DBG_LOAD_PATH_MAX + 64];
		memset(longpath, 'b', sizeof(longpath) - 1);
		longpath[sizeof(longpath) - 1] = '\0';
		dbg_load_set_prg_path(longpath);

		dbg_load_begin_prg(1);
		transfer(1, 0xA000, 2, 16, true);

		dbg_load_event_t ev;
		check(!dbg_load_take(&ev), "an over-long -prg path is refused, not truncated");
	}

	// ── Setting a new -prg path replaces the old one ────────────────────────
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);
		dbg_load_set_prg_path("/host/FIRST.PRG");
		dbg_load_set_prg_path("/host/SECOND.PRG");

		dbg_load_begin_prg(1);
		transfer(1, 0x0801, 0, 4, true);

		dbg_load_event_t ev;
		check(dbg_load_take(&ev) && strcmp(ev.path, "/host/SECOND.PRG") == 0,
		      "the most recent -prg path is the one used");
	}

	// ── A path is merged into the debug info only once ──────────────────────
	// Repeated loads of one path each produce an event; de-duplication is at the merge.
	{
		dbg_load_reset();
		dbg_load_set_enabled(true);

		for (int i = 0; i < 3; i++) {
			dbg_load_begin(1, "/host/OVERLAY.PRG");
			transfer(1, 0xA000, (uint8_t)(4 + i), 64, true);

			dbg_load_event_t ev;
			check(dbg_load_take(&ev) && ev.bank == (uint8_t)(4 + i),
			      "reloading the same overlay still reports its new bank");
		}
	}

	// ── Merging the debug info, against real files ──────────────────────────
	// Drives dbg_load_poll() end to end: repeated loads stay reported.
	{
		const char *prg = "dbgload_test_tmp.prg";
		const char *dbg = "dbgload_test_tmp.dbg";

		if (!write_dbg(dbg, "ovl.s", 0xA000)) {
			check(false, "could not write the temporary .dbg");
		} else {
			dbg_load_reset();
			dbg_load_set_enabled(true);

			bool all_reported = true;
			for (int i = 0; i < 3; i++) {
				dbg_load_begin(1, prg);
				transfer(1, 0xA000, (uint8_t)(4 + i), 0x12C, true);
				if (!dbg_load_poll())
					all_reported = false;
			}
			check(all_reported, "every load of the same overlay is acted on");

			// Merging three times would append three copies of the file records.
			check(dbg_info_file_count() == 1,
			      "but its debug info is merged only once");

			// The last load's bank is in force, even when the merge is skipped.
			const char *src_file = NULL;
			int         src_line = 0;
			check(dbg_info_addr_to_source_banked_ex(0xA000, 6, &src_file, &src_line)
			          == DBG_BANK_RESOLVED,
			      "and the bank from the most recent load is the one that sticks");
			// The first bank now reports DBG_BANK_UNKNOWN, not NO_MATCH.
			check(dbg_info_addr_to_source_banked_ex(0xA000, 4, &src_file, &src_line)
			          != DBG_BANK_RESOLVED,
			      "not the bank it was first loaded into");

			// A load with no .dbg beside it is simply not acted on.
			dbg_load_begin(1, "dbgload_test_missing.prg");
			transfer(1, 0xA000, 4, 0x12C, true);
			check(!dbg_load_poll(), "a program with no .dbg beside it is skipped");

			// The .dbg read as a program (wildcard LOAD): only the guard stops it.
			dbg_load_begin(1, dbg);
			transfer(1, 0xA000, 4, 0x12C, true);
			check(!dbg_load_poll(), "a .dbg read as a program is refused");

			// And polling with nothing pending does nothing.
			check(!dbg_load_poll(), "polling with no load pending does nothing");

			remove(dbg);
		}
		remove(prg);
	}

	// ── A displaced module comes back ───────────────────────────────────────
	// Two overlays at one address: merging one destroys the other, re-merging restores it.
	{
		const char *dbg1 = "dbgload_test_ovl1.dbg";
		const char *dbg2 = "dbgload_test_ovl2.dbg";
		const char *prg1 = "dbgload_test_ovl1.prg";
		const char *prg2 = "dbgload_test_ovl2.prg";

		if (!write_dbg(dbg1, "ovl1.s", 0xA000) || !write_dbg(dbg2, "ovl2.s", 0xA000)) {
			check(false, "could not write the temporary overlay .dbg files");
		} else {
			dbg_load_reset();
			dbg_load_set_enabled(true);

			const char *src  = NULL;
			int         line = 0;

			dbg_load_begin(1, prg1);
			transfer(1, 0xA000, 5, 0x12C, true);
			dbg_load_poll();
			check(dbg_info_addr_to_source(0xA000, &src, &line)
			      && strstr(src, "ovl1") != NULL,
			      "the first overlay describes its own code");

			dbg_load_begin(1, prg2);
			transfer(1, 0xA000, 5, 0x12C, true);
			dbg_load_poll();
			check(dbg_info_addr_to_source(0xA000, &src, &line)
			      && strstr(src, "ovl2") != NULL,
			      "loading the second one takes over that address");

			// The second destroyed its records, so this merges again.
			dbg_load_begin(1, prg1);
			transfer(1, 0xA000, 5, 0x12C, true);
			dbg_load_poll();
			check(dbg_info_addr_to_source(0xA000, &src, &line)
			      && strstr(src, "ovl1") != NULL,
			      "and swapping the first one back restores it");

			remove(dbg1);
			remove(dbg2);
		}
	}

	// ── A module somewhere else is left alone ───────────────────────────────
	// Only modules a merge displaces are forgotten.
	{
		const char *dbg_lo  = "dbgload_test_lo.dbg";
		const char *dbg_hi  = "dbgload_test_hi.dbg";
		const char *prg_lo  = "dbgload_test_lo.prg";
		const char *prg_hi  = "dbgload_test_hi.prg";

		if (!write_dbg(dbg_lo, "lo.s", 0x6000) || !write_dbg(dbg_hi, "hi.s", 0xA000)) {
			check(false, "could not write the temporary .dbg files");
		} else {
			dbg_load_reset();
			dbg_load_set_enabled(true);

			dbg_load_begin(1, prg_lo);
			transfer(1, 0x6000, 0, 0x12C, true);
			dbg_load_poll();

			dbg_load_begin(1, prg_hi);
			transfer(1, 0xA000, 5, 0x12C, true);
			dbg_load_poll();

			// Both live: re-loading the low one does not merge.
			int before = dbg_info_file_count();
			dbg_load_begin(1, prg_lo);
			transfer(1, 0x6000, 0, 0x12C, true);
			dbg_load_poll();
			check(dbg_info_file_count() == before,
			      "a module the others never displaced is not merged twice");

			// And both still describe their own code.
			const char *src = NULL;
			int line = 0;
			check(dbg_info_addr_to_source(0x6000, &src, &line)
			      && strstr(src, "lo") != NULL, "the low module is intact");
			check(dbg_info_addr_to_source(0xA000, &src, &line)
			      && strstr(src, "hi") != NULL, "and so is the high one");

			remove(dbg_lo);
			remove(dbg_hi);
		}
	}

	// ── Following the debugger, not a snapshot of it ────────────────────────
	// Follows the debugger state as it changes at runtime (emu_write).
	{
		dbg_load_reset();

		dbg_load_set_policy(-1);          // follow
		dbg_load_note_debugger(false);
		check(!dbg_load_is_enabled(), "follows the debugger: off when it is off");
		dbg_load_note_debugger(true);
		check(dbg_load_is_enabled(), "and on once it is switched on");
		dbg_load_note_debugger(false);
		check(!dbg_load_is_enabled(), "and off again when it is switched off");

		dbg_load_set_policy(1);           // forced on
		dbg_load_note_debugger(false);
		check(dbg_load_is_enabled(), "forced on ignores the debugger");

		dbg_load_set_policy(0);           // forced off
		dbg_load_note_debugger(true);
		check(!dbg_load_is_enabled(), "and forced off ignores it too");

		// Policy and debugger state may be set in either order.
		dbg_load_note_debugger(true);
		dbg_load_set_policy(-1);
		check(dbg_load_is_enabled(), "the two can be set in either order");
	}

	// ── A load we cannot describe drops what described that range ───────────
	// An undescribed load over a module's range drops that module's records.
	{
		const char *dbg_a = "dbgload_test_a.dbg";
		const char *prg_a = "dbgload_test_a.prg";
		const char *prg_b = "dbgload_test_b.prg";   // deliberately has no .dbg

		if (!write_dbg(dbg_a, "a.s", 0xA000)) {
			check(false, "could not write the temporary .dbg");
		} else {
			dbg_load_reset();
			dbg_load_set_enabled(true);

			dbg_load_begin(1, prg_a);
			transfer(1, 0xA000, 5, 0x12C, true);
			dbg_load_poll();

			const char *src = NULL;
			int line = 0;
			check(dbg_info_addr_to_source(0xA000, &src, &line),
			      "the described module resolves to start with");

			// Something with no debug info lands on top of it.
			dbg_load_begin(1, prg_b);
			transfer(1, 0xA000, 5, 0x12C, true);
			check(!dbg_load_poll(), "a load with no .dbg reports nothing");
			check(!dbg_info_addr_to_source(0xA000, &src, &line),
			      "and takes the overwritten module's source with it");

			// And the displaced module can come back.
			dbg_load_begin(1, prg_a);
			transfer(1, 0xA000, 5, 0x12C, true);
			dbg_load_poll();
			check(dbg_info_addr_to_source(0xA000, &src, &line),
			      "the described module can be loaded again afterwards");

			remove(dbg_a);
		}
	}

	// ── A load somewhere else leaves it alone ───────────────────────────────
	// Applies only to the range actually overwritten.
	{
		const char *dbg_a = "dbgload_test_keep.dbg";
		const char *prg_a = "dbgload_test_keep.prg";
		const char *prg_x = "dbgload_test_elsewhere.prg";  // no .dbg

		if (!write_dbg(dbg_a, "keep.s", 0xA000)) {
			check(false, "could not write the temporary .dbg");
		} else {
			dbg_load_reset();
			dbg_load_set_enabled(true);

			dbg_load_begin(1, prg_a);
			transfer(1, 0xA000, 5, 0x12C, true);
			dbg_load_poll();

			// An undescribed load well clear of it.
			dbg_load_begin(1, prg_x);
			transfer(1, 0x2000, 0, 0x100, true);
			dbg_load_poll();

			const char *src = NULL;
			int line = 0;
			check(dbg_info_addr_to_source(0xA000, &src, &line),
			      "an undescribed load elsewhere leaves other modules alone");

			remove(dbg_a);
		}
	}

	// ── Loading data into your own buffer keeps your debug info ─────────────
	// A load inside a module's envelope that does not cover it keeps its debug info.
	{
		const char *dbg_p = "dbgload_test_partial.dbg";
		const char *prg_p = "dbgload_test_partial.prg";
		const char *dat_p = "dbgload_test_level.dat";   // no .dbg beside it

		if (!write_dbg(dbg_p, "partial.s", 0xA000)) {
			check(false, "could not write the temporary .dbg");
		} else {
			dbg_load_reset();
			dbg_load_set_enabled(true);

			dbg_load_begin(1, prg_p);
			transfer(1, 0xA000, 5, 0x12C, true);   // envelope $A000-$A12B
			dbg_load_poll();

			const char *src = NULL;
			int line = 0;
			check(dbg_info_addr_to_source(0xA000, &src, &line),
			      "the program describes itself once loaded");

			// Data lands inside its envelope, but nowhere near covering it.
			dbg_load_begin(1, dat_p);
			transfer(1, 0xA100, 5, 0x10, true);
			dbg_load_poll();
			check(dbg_info_addr_to_source(0xA000, &src, &line),
			      "loading data into its own range does not discard it");

			// Data starting before it but still not covering it.
			dbg_load_begin(1, dat_p);
			transfer(1, 0x9F00, 5, 0x200, true);
			dbg_load_poll();
			check(dbg_info_addr_to_source(0xA000, &src, &line),
			      "nor does a load that only partly covers it");

			// A load that covers it whole genuinely does replace it.
			dbg_load_begin(1, dat_p);
			transfer(1, 0x9F00, 5, 0x400, true);
			dbg_load_poll();
			check(!dbg_info_addr_to_source(0xA000, &src, &line),
			      "but a load covering it entirely does replace it");

			remove(dbg_p);
		}
	}

	// ── Replacing all debug info with one file ──────────────────────────────
	{
		const char *a_p = "replace_a.dbg", *b_p = "replace_b.dbg", *bad_p = "replace_bad.dbg";
		const char *src = NULL;
		int         line = 0;
		char        msg[512];
		check(write_dbg(a_p, "a.s", 0xA000) && write_dbg(b_p, "b.s", 0xA000) &&
		          write_file(bad_p, "not debug info\n", 15),
		      "replace: fixtures written");
		dbg_info_free();
		dbg_load_reset();
		check(dbg_info_load(a_p) == 0, "replace: the wrong build's info is loaded");
		check(dbg_info_loaded_count() == 1 && strcmp(dbg_info_loaded_at(0), a_p) == 0,
		      "replace: and listed as loaded");

		check(dbg_load_replace(b_p, msg, sizeof msg) == 0, "replace: the right one replaces it");
		check(dbg_info_addr_to_source(0xA000, &src, &line) && src && strstr(src, "b.s"),
		      "replace: lookups answer from the new file");
		check(dbg_info_loaded_count() == 1 && strcmp(dbg_info_loaded_at(0), b_p) == 0,
		      "replace: and only it is listed");

		check(dbg_load_replace("no_such_file.dbg", msg, sizeof msg) != 0,
		      "replace: a missing file is refused");
		check(dbg_info_addr_to_source(0xA000, &src, &line) && src && strstr(src, "b.s"),
		      "replace: and what was loaded is untouched");

		check(dbg_load_replace(bad_p, msg, sizeof msg) != 0, "replace: a file that is not debug info fails");
		check(dbg_info_addr_to_source(0xA000, &src, &line) && src && strstr(src, "b.s"),
		      "replace: and the files loaded before are read again");
		check(strstr(msg, "read again") != NULL, "replace: and it says so");

		dbg_info_free();
		remove(a_p);
		remove(b_p);
		remove(bad_p);
	}

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
