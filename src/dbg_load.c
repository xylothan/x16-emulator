// Commander X16 Emulator — debug info for programs the guest loads. See dbg_load.h.

#include "dbg_load.h"
#include "dbg_info.h"
#include "debug_server.h"
#include "source_view.h"

#include <stdlib.h>
#include <string.h>

// What a channel has told us so far. Kept per channel because two can be open
// at once, and the second one to be opened must not overwrite what the first is
// still in the middle of doing.
struct channel_load {
	char     path[DBG_LOAD_PATH_MAX];
	uint16_t addr;
	uint8_t  bank;
	uint32_t size;       // bytes reported; zero means no byte has landed yet
	bool     open;       // a path has been recorded
};

static struct channel_load channels[DBG_LOAD_CHANNELS];
static dbg_load_event_t    pending;
static bool                enabled;

// See dbg_load_set_policy(): the debugger can be switched on by the running
// machine, so what the command line asked for is kept separately from what the
// debugger is currently doing, and the two are resolved on every change.
static int  policy = -1;
static bool debugger_on;

static void
apply_policy(void)
{
	enabled = (policy >= 0) ? (policy > 0) : debugger_on;
}

// Configuration rather than state, so a machine reset does not clear it: the
// -prg file is still the -prg file afterwards.
static char prg_path[DBG_LOAD_PATH_MAX];

// Which .dbg files are currently merged into dbg_info, and over what range.
//
// dbg_info_load() is additive and dbg_info_unload_range() prunes only some of
// what it builds. File and equate records are never pruned by address; they are
// instead reused or replaced when the same .dbg is parsed again, so those no
// longer accumulate. Segment, span, line and label records still append, and
// they are pruned only by the address range handed to the unload -- so tracking
// which .dbg is live over which range is what keeps the two in step.
//
// But merging is also destructive: dbg_info_load_for_file() unloads everything
// in the incoming file's address range before reading it. Re-parsing is
// therefore how a module that was displaced gets its records back. So this
// tracks what is *live*, not what has *ever* been merged: when a new file
// evicts a range, the modules it overlaps are forgotten and will merge again
// the next time they load.
//
// Not cleared by dbg_load_reset(): a machine reset does not free dbg_info, so
// those records are still live afterwards.
// The registry grows rather than evicting. Evicting a live module would be
// unsound: dropping its entry does not drop its records, so the next load of
// that path would look like a first sighting and merge a second copy -- the
// exact duplication this exists to prevent. It stays small in practice because
// every merge forgets the modules it displaces, so entries only ever track
// modules that are genuinely resident at the same time.
struct parsed_entry {
	char       path[DBG_LOAD_PATH_MAX];
	dbg_addr_t start;
	dbg_addr_t end;
};

static struct parsed_entry *parsed;
static int                  parsed_count;
static int                  parsed_cap;

static bool
already_parsed(const char *path)
{
	for (int i = 0; i < parsed_count; i++) {
		if (strcmp(parsed[i].path, path) == 0)
			return true;
	}
	return false;
}

// Drop the tracking entries for every module overlapping [start,end], and
// return how many there were. The caller unloads the records, because what
// needs unloading differs between "something else is taking this range" and
// "this range is being overwritten by something we cannot describe at all".
static int
forget_overlapping(dbg_addr_t start, dbg_addr_t end)
{
	int keep    = 0;
	int dropped = 0;

	for (int i = 0; i < parsed_count; i++) {
		bool overlaps = parsed[i].start <= end && start <= parsed[i].end;
		if (overlaps) {
			dropped++;
		} else {
			if (keep != i)
				parsed[keep] = parsed[i];
			keep++;
		}
	}
	parsed_count = keep;
	return dropped;
}

// Make room for one more entry without adding it, so a merge is never done
// unless its result can be tracked. A merged-but-untracked module would be
// merged a second time on its next load, which is the duplication all this
// exists to prevent.
static bool
reserve_parsed_slot(const char *path)
{
	if (strlen(path) >= DBG_LOAD_PATH_MAX)
		return false;

	if (parsed_count < parsed_cap)
		return true;

	int new_cap = parsed_cap ? parsed_cap * 2 : 8;
	struct parsed_entry *grown = realloc(parsed, (size_t)new_cap * sizeof(*grown));
	if (!grown)
		return false;

	parsed     = grown;
	parsed_cap = new_cap;
	return true;
}

// Record a module as live. The caller must have reserved a slot first, so this
// cannot fail.
static void
remember_parsed(const char *path, dbg_addr_t start, dbg_addr_t end)
{
	size_t len = strlen(path);

	memcpy(parsed[parsed_count].path, path, len + 1);
	parsed[parsed_count].start = start;
	parsed[parsed_count].end   = end;
	parsed_count++;
}
static bool
valid_channel(int channel)
{
	return channel >= 0 && channel < DBG_LOAD_CHANNELS;
}

// Something we cannot describe landed at [addr, addr+size). Unload the debug
// info of any module it completely replaced, because that info now describes
// bytes which are gone, and a debugger confidently showing the wrong source is
// worse than one showing none.
//
// Only when the load covers a module *whole*. The envelope recorded for a
// module is the min/max across every segment in its .dbg -- including ZEROPAGE,
// DATA and BSS -- so for a typical cc65 program it spans most of low RAM. A
// partial overlap is therefore far more likely to be that same program loading
// data into one of its own buffers, which invalidates nothing; discarding its
// debug info for that would throw away the mapping for the code that is running.
//
// Modules this registry does not know about are left alone, so debug info the
// user named with -dbgfile survives: they asked for it explicitly, and guessing
// that a guest load invalidated it is a worse failure than leaving it.
static void
discard_range(uint16_t addr, uint32_t size)
{
	if (size == 0)
		return;

	dbg_addr_t start = addr;
	dbg_addr_t end   = addr + (size - 1);
	if (end > 0xFFFF)
		end = 0xFFFF;  // a transfer running off the top wraps; it cannot cover more

	int keep = 0;
	for (int i = 0; i < parsed_count; i++) {
		bool covered = start <= parsed[i].start && parsed[i].end <= end;
		if (covered) {
			dbg_info_unload_range(parsed[i].start, parsed[i].end);
		} else {
			if (keep != i)
				parsed[keep] = parsed[i];
			keep++;
		}
	}
	parsed_count = keep;
}

// Whether a host path names a .dbg file.
bool
dbg_load_path_is_dbg(const char *path)
{
	if (!path)
		return false;

	size_t n = strlen(path);
	if (n < 4)
		return false;

	const char *ext = path + n - 4;
	return ext[0] == '.'
	    && (ext[1] == 'd' || ext[1] == 'D')
	    && (ext[2] == 'b' || ext[2] == 'B')
	    && (ext[3] == 'g' || ext[3] == 'G');
}

void
dbg_load_set_policy(int new_policy)
{
	policy = new_policy;
	apply_policy();
}

void
dbg_load_note_debugger(bool on)
{
	debugger_on = on;
	apply_policy();
}

void
dbg_load_set_enabled(bool on)
{
	dbg_load_set_policy(on ? 1 : 0);
}

bool
dbg_load_is_enabled(void)
{
	return enabled;
}

void
dbg_load_begin(int channel, const char *host_path)
{
	if (!valid_channel(channel))
		return;

	struct channel_load *c = &channels[channel];
	memset(c, 0, sizeof(*c));

	// Nothing is collected at all unless auto-loading was asked for, which
	// leaves the per-byte reporting below doing nothing but testing a flag.
	if (!enabled || !host_path || !host_path[0])
		return;

	// A truncated path names a different file, or none. Rather than parse
	// whatever that turns out to be, ignore the load.
	size_t len = strlen(host_path);
	if (len >= sizeof(c->path))
		return;

	memcpy(c->path, host_path, len + 1);
	c->open = true;
}

// Publish a finished load and clear the channel. Static, and reached only from
// dbg_load_note_byte() below with an open channel whose final byte has already
// been counted -- which is what keeps the byte and the completion in the right
// order, and why there is nothing to check here.
static void
complete(int channel)
{
	struct channel_load *c = &channels[channel];

	memcpy(pending.path, c->path, sizeof(pending.path));
	pending.addr  = c->addr;
	pending.bank  = c->bank;
	pending.size  = c->size;
	pending.valid = true;

	memset(c, 0, sizeof(*c));
}

void
dbg_load_set_prg_path(const char *path)
{
	prg_path[0] = '\0';
	if (!path)
		return;

	size_t len = strlen(path);
	if (len >= sizeof(prg_path))
		return;  // truncation would name a different file; better to say nothing

	memcpy(prg_path, path, len + 1);
}

void
dbg_load_begin_prg(int channel)
{
	dbg_load_begin(channel, prg_path);
}

void
dbg_load_note_byte(int channel, uint16_t addr, uint8_t bank, bool last)
{
	if (!valid_channel(channel))
		return;

	struct channel_load *c = &channels[channel];
	if (!c->open)
		return;

	// The first byte is the one that says where the program starts and which
	// bank it started in. A load crossing $C000 moves the RAM bank on as it
	// goes, so a later byte would name a different segment.
	if (c->size == 0) {
		c->addr = addr;
		c->bank = bank;
	}
	c->size++;

	// Counted above before this runs, which is the whole point of the two being
	// a single call. A channel that never reports a byte -- a data file read
	// through GET#, or a stream-mode transfer into a hardware register -- never
	// reaches here at all, and so is never mistaken for a program load.
	if (last)
		complete(channel);
}

void
dbg_load_abandon(int channel)
{
	if (!valid_channel(channel))
		return;
	memset(&channels[channel], 0, sizeof(channels[channel]));
}

bool
dbg_load_take(dbg_load_event_t *out)
{
	if (!pending.valid)
		return false;
	if (out) {
		*out = pending;
	}
	memset(&pending, 0, sizeof(pending));
	return true;
}

bool
dbg_load_poll(void)
{
	dbg_load_event_t ev;
	if (!dbg_load_take(&ev))
		return false;
	if (!enabled || !ev.path[0])
		return false;

	// A .dbg loaded as if it were a program. A wildcard like LOAD"GAME*" can
	// match the debug info sitting beside GAME.PRG, and the machine will
	// happily read that text file into memory at whatever its first two bytes
	// spell out. Reading it as debug info for itself would then attribute its
	// segments to that nonsense address -- but the bytes did land, so anything
	// they completely replaced is still invalidated.
	if (dbg_load_path_is_dbg(ev.path)) {
		discard_range(ev.addr, ev.size);
		return false;
	}

	// Load first. dbg_info_load_for_file() unloads whatever already describes
	// the address range before reading the new file, so a bank annotation made
	// beforehand would be thrown away with the mappings it was attached to.
	//
	// Only if this file's records are not already live -- see the note on
	// `parsed` above. Note also that dbg_info_load_for_file() ignores the
	// address it is given, so a relocating load -- secondary address 0, which
	// lands at BASIC start rather than the address in the file's own header --
	// still describes the program at its link-time addresses. That is a
	// limitation of the debug info, not of the destination recorded here.
	if (!already_parsed(ev.path)) {
		dbg_addr_t new_start, new_end;
		if (!dbg_info_peek_file_range(ev.path, &new_start, &new_end)) {
			// Nothing describes this file, but it still landed in memory.
			discard_range(ev.addr, ev.size);
			return false;
		}

		// Make room before merging, so the result can definitely be tracked.
		if (!reserve_parsed_slot(ev.path)) {
			discard_range(ev.addr, ev.size);
			return false;
		}

		int rc = dbg_info_load_for_file(ev.path, ev.addr);

		// A DAP client's breakpoints are addresses resolved from debug info
		// that has just been replaced, so anything inside the range it covered
		// is now pointing at whatever used to be there.
		debug_server_invalidate_breakpoints_in_range(new_start, new_end);

		// Forgotten whether or not that succeeded. It unloads the range before
		// reading the file, so a failure part-way through still leaves those
		// modules' records gone -- and a registry claiming they are live would
		// then skip the merge that would bring them back. Forgetting one whose
		// records did survive costs only a re-merge next time, which replaces
		// them rather than duplicating them.
		forget_overlapping(new_start, new_end);

		if (rc != 0)
			return false;
		remember_parsed(ev.path, new_start, new_end);

		// Breakpoints set before the program was loaded could not be resolved
		// then. The debug info describing them has just arrived, so this is the
		// moment they can be armed.
		debug_server_retry_unverified_breakpoints();
	}

	// Now say which bank the program went into, every time, because the same
	// overlay can legitimately be loaded into a different bank than last time.
	// Several segments commonly start at $A000, one per bank, and the .dbg
	// records no bank at all, so this is the only thing that can tell them
	// apart. The bank is the one that was mapped when the first byte landed --
	// a load crossing $C000 moves the bank on as it goes, so the bank at the
	// end belongs to a different segment.
	if (ev.addr >= 0xA000 && ev.addr <= 0xBFFF && ev.size > 0) {
		dbg_info_note_bank_load(ev.addr, ev.size, ev.bank);
	}

	// Both the .dbg's own directory and the loaded file's are host paths worth
	// searching for the sources it names.
	source_view_add_path(dbg_info_get_dbg_dir());
	{
		char dir[DBG_LOAD_PATH_MAX];
		strncpy(dir, ev.path, sizeof(dir) - 1);
		dir[sizeof(dir) - 1] = '\0';
		char *s1  = strrchr(dir, '/');
		char *s2  = strrchr(dir, '\\');
		char *sep = (s2 > s1) ? s2 : s1;
		if (sep) {
			*sep = '\0';
			source_view_add_path(dir);
		}
	}

	// An overlay can reuse a file name at a different address, so anything
	// cached under that name has to be re-read rather than trusted.
	source_view_invalidate();
	return true;
}

void
dbg_load_reset(void)
{
	memset(channels, 0, sizeof(channels));
	memset(&pending, 0, sizeof(pending));
}
