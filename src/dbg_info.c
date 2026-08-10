#include "dbg_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "dirent_win32.h" // portable opendir/readdir (native <dirent.h> elsewhere)

#ifdef _WIN32
#define strcasecmp _stricmp
#else
// The project builds as strict C11, under which glibc leaves strcasecmp out of
// <string.h>; it lives here.
#include <strings.h>
#endif

/* ------------------------------------------------------------------ */
/*  Raw parsed records                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
	int    id;
	char  *name;
	int    owner;     /* which .dbg declared it; see dbg_owner_intern() */
} dbg_file_t;

typedef struct {
	int      id;
	uint32_t start;   /* base address */
	uint32_t size;
	char    *name;    /* segment name, e.g. "BANKCODE" */
	int      bank;    /* X16 RAM bank this segment lives in, -1 = unknown */
	int      owner;   /* which .dbg declared it; see dbg_owner_intern() */
	bool     bank_from_equ; /* bank was inferred from an equate, not observed */
} dbg_seg_t;

typedef struct {
	int      id;
	int      seg;     /* segment id */
	uint32_t start;   /* offset within segment */
	uint32_t size;
} dbg_span_t;

typedef struct {
	int  id;
	int  file;        /* file id */
	int  line;        /* source line number */
	int *spans;       /* array of span ids */
	int  span_count;
} dbg_line_t;

/* ------------------------------------------------------------------ */
/*  Resolved address map entry (built after parsing)                   */
/* ------------------------------------------------------------------ */

typedef struct {
	dbg_addr_t addr;
	dbg_addr_t end;     /* last address covered by this line's span (inclusive) */
	int      file_id;
	int      line_num;
	int      seg_id;  /* owning segment — disambiguates banked ($A000-$BFFF) code */
} addr_map_entry_t;

/* ------------------------------------------------------------------ */
/*  Module state                                                       */
/* ------------------------------------------------------------------ */

static dbg_file_t      *files;
static int              file_count;
static int              file_cap;

static dbg_seg_t       *segs;
static int              seg_count;
static int              seg_cap;

static dbg_span_t      *spans;
static int              span_count;
static int              span_cap;

static dbg_line_t      *lines;
static int              line_count;
static int              line_cap;

/* cc65 `sym` records: name → address, for address-typed labels (type=lab).
 * Kept sorted by address after each load for fast addr→label lookup. */
typedef struct {
	char    *name;
	dbg_addr_t addr;
} dbg_sym_t;

static dbg_sym_t       *syms;
static int              sym_count;
static int              sym_cap;

/* `RAM_BANK_x` / `x_BANK` equates (type=equ, small value) harvested while
 * parsing. Used to associate banked segments ($A000-$BFFF) with a RAM bank —
 * see seed_banks_from_equates(). */
typedef struct {
	char *name;
	int   bank;
	int   owner;      /* which .dbg declared it; see dbg_owner_intern() */
	uint64_t gen;     /* which parse of that .dbg produced it */
} dbg_bank_equ_t;

static dbg_bank_equ_t  *bank_equs;
static int              bank_equ_count;
static int              bank_equ_cap;

/* cc65 `equ` records: name → value. These are constants rather than program
 * addresses (KERNAL entry points such as JOYGET, hardware register names,
 * zero-page variable addresses, plain numeric constants), so they are kept out
 * of `syms` to leave that a clean address→name map for the disassembler. They
 * are still the thing a user hovers most often in source, so keep them here and
 * consult them for name→value lookups. */
typedef struct {
	char    *name;
	dbg_addr_t val;
	int      owner;   /* which .dbg declared it; see dbg_owner_intern() */
	uint64_t gen;     /* which parse of that .dbg produced it */
} dbg_equ_t;

static dbg_equ_t       *equs;
static int              equ_count;
static int              equ_cap;

static addr_map_entry_t *addr_map;
static int               addr_map_count;
static int               addr_map_cap;

static bool              loaded;

// Directory of the most recently loaded .dbg file (for source discovery).
static char              dbg_dir[1024];

// ID offsets for merging multiple .dbg files without ID collisions
static int              id_base_file;
static int              id_base_seg;
static int              id_base_span;
static int              id_base_line;

// Highest ID handed out so far, plus one. IDs must never be reused: unloading a
// module compacts segs[]/spans[] but the survivors keep their IDs, so basing
// the next module's offset on the array count would hand out IDs that collide
// with them.
static int              next_id_file;
static int              next_id_seg;
static int              next_id_span;
static int              next_id_line;

static void note_id(int *high_water, int id)
{
	if (id >= *high_water) {
		*high_water = id + 1;
	}
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void seed_banks_from_equates(void); /* defined with the bank helpers below */

#define INIT_CAP 64

// Grow a dynamic array in place. Returns false and leaves the array untouched
// if the allocation fails, so callers can decline to append rather than write
// through a NULL that realloc left behind.
static bool grow_array(void **arr, int *cap, size_t elem_size)
{
	int new_cap = *cap ? *cap * 2 : INIT_CAP;
	void *grown = realloc(*arr, (size_t)new_cap * elem_size);
	if (!grown) {
		return false;
	}
	*arr = grown;
	*cap = new_cap;
	return true;
}

#define GROW_ARRAY(arr, count, cap, type) \
    ((count) < (cap) || grow_array((void **)&(arr), &(cap), sizeof(type)))

/* Skip leading whitespace / tabs. */
static const char *skip_ws(const char *p)
{
	while (*p && (*p == ' ' || *p == '\t'))
		p++;
	return p;
}

/* Extract the value string for a key inside a key=value list.
 * `p` points just past the record-type keyword.
 * If the key is found, *out receives a pointer to the start of the value
 * (inside the original buffer) and the function returns the length.
 * For quoted values the quotes are stripped.
 * Returns -1 if the key is not found. */
static int find_value(const char *p, const char *key, const char **out)
{
	size_t klen = strlen(key);

	while (*p) {
		p = skip_ws(p);
		if (*p == '\0')
			break;

		/* Match key name */
		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			const char *v = p + klen + 1;
			if (*v == '"') {
				/* Quoted string — find closing quote */
				v++;
				const char *end = strchr(v, '"');
				if (!end)
					return -1;
				*out = v;
				return (int)(end - v);
			} else {
				const char *end = v;
				while (*end && *end != ',' && *end != '\n' && *end != '\r')
					end++;
				*out = v;
				return (int)(end - v);
			}
		}

		/* Advance to next key=value pair */
		while (*p && *p != ',')
			p++;
		if (*p == ',')
			p++;
	}
	return -1;
}

/* Parse an integer value for a given key.  Returns -1 if not found. */
static long parse_int_key(const char *p, const char *key)
{
	const char *v;
	int len = find_value(p, key, &v);
	if (len <= 0)
		return -1;

	char buf[32];
	if (len >= (int)sizeof(buf))
		len = (int)sizeof(buf) - 1;
	memcpy(buf, v, (size_t)len);
	buf[len] = '\0';

	if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
		return (long)strtoul(buf, NULL, 16);
	return strtol(buf, NULL, 10);
}

/* Parse a quoted-string value for a given key.  Caller must free(). */
static char *parse_str_key(const char *p, const char *key)
{
	const char *v;
	int len = find_value(p, key, &v);
	if (len < 0)
		return NULL;
	char *s = (char *)malloc((size_t)len + 1);
	if (!s)
		return NULL;
	memcpy(s, v, (size_t)len);
	s[len] = '\0';
	return s;
}

/* Parse a span list like "9" or "9+10+11".
 * Allocates and returns an int array; sets *out_count. */
static int *parse_span_list(const char *p, int *out_count)
{
	const char *v;
	int len = find_value(p, "span", &v);
	if (len <= 0) {
		*out_count = 0;
		return NULL;
	}

	/* Count '+' separators to size the array */
	int cap = 1;
	for (int i = 0; i < len; i++)
		if (v[i] == '+')
			cap++;

	int *arr = (int *)malloc((size_t)cap * sizeof(int));
	if (!arr) {
		*out_count = 0;
		return NULL;
	}

	int n = 0;
	const char *end = v + len;
	while (v < end && n < cap) {
		arr[n++] = (int)strtol(v, NULL, 10);
		/* Advance past the number and the '+' */
		while (v < end && *v != '+')
			v++;
		if (v < end)
			v++;  /* skip '+' */
	}

	*out_count = n;
	return arr;
}

/* Return a pointer to the basename portion of a path. */
static const char *basename_ptr(const char *path)
{
	const char *p = path;
	const char *last = path;
	while (*p) {
		if (*p == '/' || *p == '\\')
			last = p + 1;
		p++;
	}
	return last;
}

/* ------------------------------------------------------------------ */
/*  Record parsers                                                     */
/* ------------------------------------------------------------------ */

/* Records are deduplicated so that reloading a module does not grow the tables
 * without end. That is only correct WITHIN a module: two linked modules may
 * legitimately both compile a file called "main.c", and may both define an
 * equate called LIMIT with different values. Deduplicating those against each
 * other would make one module's debug info describe the other's code. So every
 * deduplicated record remembers which .dbg declared it, and a reused record has
 * to come from the same one.
 *
 * The owner is the .dbg path, interned. Comparison is case-insensitive on
 * Windows, where the same file reached by two spellings is still one file. */
static char **owners      = NULL;
static int    owner_count = 0;
static int    owner_cap   = 0;
static int    cur_owner   = -1;
/* Bumped per parse, so records written by the parse in progress can be told
 * from ones the same module left behind last time. */
static uint64_t cur_gen   = 0;

static int dbg_owner_intern(const char *path)
{
	if (!path)
		return -1;
	for (int i = 0; i < owner_count; i++) {
#ifdef _WIN32
		if (owners[i] && _stricmp(owners[i], path) == 0)
#else
		if (owners[i] && strcmp(owners[i], path) == 0)
#endif
			return i;
	}
	if (!GROW_ARRAY(owners, owner_count, owner_cap, char *))
		return -1;
	size_t n = strlen(path);
	char  *copy = (char *)malloc(n + 1);
	if (!copy)
		return -1;
	memcpy(copy, path, n + 1);
	owners[owner_count] = copy;
	return owner_count++;
}

/* Drop the equates a module declared in an earlier generation. Called once the
 * file has been read, so a file that turned out to be unreadable or unparseable
 * leaves the previous generation intact. Replacing them one by one as they are
 * parsed is not enough on its own: an equate the rebuilt file no longer
 * declares would never be visited, and would go on seeding banks and answering
 * lookups on behalf of a module that has stopped defining it. */
static void drop_stale_equates(int owner, uint64_t gen)
{
	if (owner < 0)
		return;
	int w = 0;
	for (int i = 0; i < equ_count; i++) {
		if (equs[i].owner == owner && equs[i].gen != gen) {
			free(equs[i].name);
			continue;
		}
		if (w != i)
			equs[w] = equs[i];
		w++;
	}
	equ_count = w;

	w = 0;
	for (int i = 0; i < bank_equ_count; i++) {
		if (bank_equs[i].owner == owner && bank_equs[i].gen != gen) {
			free(bank_equs[i].name);
			continue;
		}
		if (w != i)
			bank_equs[w] = bank_equs[i];
		w++;
	}
	bank_equ_count = w;
}

/* Maps this load's `file` IDs onto the record that ends up describing them.
 * Needed because a file already known from an earlier load is reused rather than
 * appended, so its ID is not this load's ID plus the usual base. Reset per load.
 */
static int *file_alias      = NULL;
static int  file_alias_count = 0;
static int  file_alias_cap   = 0;

static void file_alias_set(long incoming, int resolved)
{
	if (incoming < 0 || incoming > 0xFFFFF)
		return;                     /* absurd ID; the record is unusable anyway */
	while (file_alias_count <= (int)incoming) {
		if (!GROW_ARRAY(file_alias, file_alias_count, file_alias_cap, int))
			return;
		file_alias[file_alias_count++] = -1;
	}
	file_alias[incoming] = resolved;
}

static int file_alias_get(long incoming)
{
	if (incoming >= 0 && incoming < file_alias_count && file_alias[incoming] >= 0)
		return file_alias[incoming];
	return (int)incoming + id_base_file;    /* no alias recorded; the usual rule */
}

static void parse_file_record(const char *p)
{
	long id = parse_int_key(p, "id");
	char *name = parse_str_key(p, "name");
	if (id < 0 || !name) {
		free(name);
		return;
	}

	/* Reuse a file this same .dbg already gave us. Unloading a range
	 * deliberately leaves file records alone -- callers keep the path pointers
	 * we handed them -- so appending here would add a copy of every file on
	 * every reload, and an overlay swapped in and out repeatedly would grow the
	 * table without end. Restricted to one owner because two modules that both
	 * build a "main.c" have two different main.c's. Consumers currently resolve
	 * a file by its name string, so merging them is not yet observable beyond
	 * the record count; keeping them apart is what lets the ownership work
	 * noted at dbg_info_load_for_file() tell them apart later. */
	for (int i = 0; i < file_count; i++) {
		if (files[i].owner == cur_owner && files[i].name &&
		    strcmp(files[i].name, name) == 0) {
			file_alias_set(id, files[i].id);
			free(name);
			return;
		}
	}

	if (!GROW_ARRAY(files, file_count, file_cap, dbg_file_t)) { free(name); return; }
	files[file_count].id    = (int)id + id_base_file;
	note_id(&next_id_file, files[file_count].id);
	files[file_count].name  = name;
	files[file_count].owner = cur_owner;
	file_alias_set(id, files[file_count].id);
	file_count++;
}

static void parse_seg_record(const char *p)
{
	long id    = parse_int_key(p, "id");
	long start = parse_int_key(p, "start");
	long size  = parse_int_key(p, "size");
	if (id < 0 || start < 0)
		return;
	if (size < 0)
		size = 0;

	if (!GROW_ARRAY(segs, seg_count, seg_cap, dbg_seg_t)) return;
	segs[seg_count].id    = (int)id + id_base_seg;
	note_id(&next_id_seg, segs[seg_count].id);
	segs[seg_count].start = (uint32_t)start;
	segs[seg_count].size  = (uint32_t)size;
	segs[seg_count].name  = parse_str_key(p, "name"); /* may be NULL */
	segs[seg_count].bank  = -1;                       /* learned later */
	segs[seg_count].owner = cur_owner;
	segs[seg_count].bank_from_equ = false;
	seg_count++;
}

static void parse_span_record(const char *p)
{
	long id    = parse_int_key(p, "id");
	long seg   = parse_int_key(p, "seg");
	long start = parse_int_key(p, "start");
	long size  = parse_int_key(p, "size");
	if (id < 0 || seg < 0 || start < 0)
		return;
	if (size < 0)
		size = 0;

	if (!GROW_ARRAY(spans, span_count, span_cap, dbg_span_t)) return;
	spans[span_count].id    = (int)id + id_base_span;
	note_id(&next_id_span, spans[span_count].id);
	spans[span_count].seg   = (int)seg + id_base_seg;
	spans[span_count].start = (uint32_t)start;
	spans[span_count].size  = (uint32_t)size;
	span_count++;
}

static void parse_line_record(const char *p)
{
	long id   = parse_int_key(p, "id");
	long file = parse_int_key(p, "file");
	long line = parse_int_key(p, "line");
	if (id < 0 || file < 0 || line < 0)
		return;

	int  span_n = 0;
	int *span_ids = parse_span_list(p, &span_n);
	if (span_n == 0) {
		free(span_ids);
		return;
	}

	// Offset span IDs for multi-file merge
	for (int i = 0; i < span_n; i++)
		span_ids[i] += id_base_span;

	if (!GROW_ARRAY(lines, line_count, line_cap, dbg_line_t)) { free(span_ids); return; }
	lines[line_count].id         = (int)id + id_base_line;
	note_id(&next_id_line, lines[line_count].id);
	lines[line_count].file       = (int)file;   /* raw; resolved after the load */
	lines[line_count].line       = (int)line;
	lines[line_count].spans      = span_ids;
	lines[line_count].span_count = span_n;
	line_count++;
}

/* Parse a cc65 `sym` record. We keep address-typed labels (type=lab, and
 * records with no explicit type) that carry a 16-bit `val` — i.e. named program
 * addresses. Equates/imports/constants (type=equ/imp/…) are skipped so the
 * label map stays a clean address→name mapping for the disassembler. */
/* Remove a name this parse already filed in the ordinary equate table. Used
 * when the same name turns up again with a value that belongs in the other
 * table, so that the later definition is the only one left. */
static void forget_current_equate(dbg_equ_t *arr, int *count, const char *name)
{
	int w = 0;
	for (int i = 0; i < *count; i++) {
		if (arr[i].owner == cur_owner && arr[i].gen == cur_gen && arr[i].name &&
		    !strcasecmp(arr[i].name, name)) {
			free(arr[i].name);
			continue;
		}
		if (w != i)
			arr[w] = arr[i];
		w++;
	}
	*count = w;
}

static void forget_current_bank_equate(const char *name)
{
	int w = 0;
	for (int i = 0; i < bank_equ_count; i++) {
		if (bank_equs[i].owner == cur_owner && bank_equs[i].gen == cur_gen &&
		    bank_equs[i].name && !strcasecmp(bank_equs[i].name, name)) {
			free(bank_equs[i].name);
			continue;
		}
		if (w != i)
			bank_equs[w] = bank_equs[i];
		w++;
	}
	bank_equ_count = w;
}

/* Returns true if the record was structurally complete -- it had a name and a
 * usable value -- whether or not we ended up keeping it. The caller counts
 * those to tell a file that was written to the end from one still being
 * written; it is not a measure of how many records we stored. */
static bool parse_sym_record(const char *p)
{
	char *name = parse_str_key(p, "name");
	if (!name)
		return false;

	long val = parse_int_key(p, "val");
	if (val < 0 || val > 0xFFFFFF) {   /* no usable address value */
		free(name);
		return false;
	}

	/* Keep only labels. If a `type` field is present it must be "lab"; records
	 * without a type field are accepted (older/leaner emitters). */
	const char *tv;
	int tlen = find_value(p, "type", &tv);
	if (tlen > 0 && !(tlen == 3 && strncmp(tv, "lab", 3) == 0)) {
		/* Not a label. Equates that look like a RAM-bank constant
		 * (RAM_BANK_x / BANK_x / x_BANK with a 0..255 value) are kept aside to
		 * associate banked segments with their RAM bank. */
		if (tlen == 3 && strncmp(tv, "equ", 3) == 0 && val >= 0 && val <= 255) {
			size_t l = strlen(name);
			bool looks_bank = !strncmp(name, "RAM_BANK", 8) ||
			                  !strncmp(name, "BANK_", 5) ||
			                  (l > 5 && !strcmp(name + l - 5, "_BANK"));
			if (looks_bank) {
				/* Same-owner only. Pooling these lets a second module's value
				 * overwrite the first's, and since equate-derived banks are now
				 * re-derived on every load, the first module's own segments
				 * would then be re-seeded from a value it never declared. */
				for (int i = 0; i < bank_equ_count; i++) {
					if (bank_equs[i].owner == cur_owner &&
					    bank_equs[i].gen == cur_gen &&
					    bank_equs[i].name &&
					    !strcasecmp(bank_equs[i].name, name)) {
						free(bank_equs[i].name);
						bank_equs[i].name = name;   /* takes ownership */
						bank_equs[i].bank = (int)val;
						return true;
					}
				}
				/* The same name can land in either table depending on its
				 * value -- RAM_BANK_CODE=3 is a bank constant, RAM_BANK_CODE
				 * =$1234 is not -- so a redefinition can cross tables. Drop any
				 * twin on the other side, or the superseded one goes on
				 * answering for whichever question it was filed under. */
				forget_current_equate(equs, &equ_count, name);
				if (!GROW_ARRAY(bank_equs, bank_equ_count, bank_equ_cap, dbg_bank_equ_t)) { free(name); return true; }
				bank_equs[bank_equ_count].name  = name;   /* takes ownership */
				bank_equs[bank_equ_count].bank  = (int)val;
				bank_equs[bank_equ_count].owner = cur_owner;
				bank_equs[bank_equ_count].gen   = cur_gen;
				bank_equ_count++;
				return true;
			}
		}
		/* Every other equate is still a name the user can hover in source
		 * (KERNAL vectors, hardware registers, constants), so record it for
		 * name→value lookups without polluting the label map. */
		if (tlen == 3 && strncmp(tv, "equ", 3) == 0) {
			/* Replace rather than append, for the same reason as bank equates:
			 * unloading a range never prunes these, and the lookup returns the
			 * first match, so a stale value would win forever. Same-owner only:
			 * two modules may each define LIMIT with different values, and
			 * replacing across them would destroy one of the two outright,
			 * since nothing restores it when the other module unloads. */
			for (int i = 0; i < equ_count; i++) {
				if (equs[i].owner == cur_owner && equs[i].gen == cur_gen &&
				    equs[i].name &&
				    !strcasecmp(equs[i].name, name)) {
					free(equs[i].name);
					equs[i].name = name;   /* takes ownership */
					equs[i].val  = (dbg_addr_t)val;
					return true;
				}
			}
			forget_current_bank_equate(name);
			if (!GROW_ARRAY(equs, equ_count, equ_cap, dbg_equ_t)) { free(name); return true; }
			equs[equ_count].name  = name;   /* takes ownership */
			equs[equ_count].val   = (dbg_addr_t)val;
			equs[equ_count].owner = cur_owner;
			equs[equ_count].gen   = cur_gen;
			equ_count++;
			return true;
		}
		free(name);
		return true;
	}

	if (!GROW_ARRAY(syms, sym_count, sym_cap, dbg_sym_t)) { free(name); return true; }
	syms[sym_count].name = name;
	syms[sym_count].addr = (dbg_addr_t)val;
	sym_count++;
	return true;
}

/* ------------------------------------------------------------------ */
/*  Segment / span / file lookups by id.                                */
/* ------------------------------------------------------------------ */

/* Records are normally appended with increasing IDs, and compaction preserves
 * their order, so a binary search finds them in log time. The file supplies the
 * IDs, though, and nothing obliges it to list them in order -- so a miss falls
 * back to a linear scan, which is correct whatever order they arrive in.
 *
 * The index shortcut this replaces -- `arr[id].id == id` -- only held on the
 * very first load. Unloading a range compacts the array without resetting the ID
 * counter, so afterwards element 0 carries a nonzero ID and the shortcut missed
 * on every lookup, leaving the linear scan alone. build_addr_map() does one span
 * lookup per line, so a rebuild went quadratic exactly where it mattered: the
 * auto-load path, which runs from the emulator's main loop. */
#define DBG_FIND_BY_ID(arr, count, id)                                    \
	do {                                                                  \
		int lo_ = 0, hi_ = (count) - 1;                                   \
		while (lo_ <= hi_) {                                              \
			const int mid_ = lo_ + (hi_ - lo_) / 2;                       \
			if ((arr)[mid_].id == (id))                                   \
				return &(arr)[mid_];                                      \
			if ((arr)[mid_].id < (id))                                    \
				lo_ = mid_ + 1;                                           \
			else                                                          \
				hi_ = mid_ - 1;                                           \
		}                                                                 \
		for (int i_ = 0; i_ < (count); i_++)                              \
			if ((arr)[i_].id == (id))                                     \
				return &(arr)[i_];                                        \
		return NULL;                                                      \
	} while (0)

static const dbg_seg_t *find_seg(int id)
{
	DBG_FIND_BY_ID(segs, seg_count, id);
}

static const dbg_span_t *find_span(int id)
{
	DBG_FIND_BY_ID(spans, span_count, id);
}

static const dbg_file_t *find_file(int id)
{
	if (id >= 0 && id < file_count && files[id].id == id)
		return &files[id];
	for (int i = 0; i < file_count; i++)
		if (files[i].id == id)
			return &files[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/*  Build the sorted address map                                       */
/* ------------------------------------------------------------------ */

static int addr_map_cmp(const void *a, const void *b)
{
	const addr_map_entry_t *ea = (const addr_map_entry_t *)a;
	const addr_map_entry_t *eb = (const addr_map_entry_t *)b;
	if (ea->addr != eb->addr)
		return (ea->addr < eb->addr) ? -1 : 1;
	/* Same start: order by span size ascending so the tightest (innermost)
	 * span comes first. qsort is not stable, so without this tie-break the
	 * order of equal-start spans was arbitrary and the line reported for a PC
	 * could vary between runs. */
	if (ea->end != eb->end)
		return (ea->end < eb->end) ? -1 : 1;
	if (ea->line_num != eb->line_num)
		return (ea->line_num < eb->line_num) ? -1 : 1;
	return 0;
}

static int sym_cmp(const void *a, const void *b)
{
	const dbg_sym_t *sa = (const dbg_sym_t *)a;
	const dbg_sym_t *sb = (const dbg_sym_t *)b;
	if (sa->addr != sb->addr)
		return (sa->addr < sb->addr) ? -1 : 1;
	return 0;
}

static void build_addr_map(void)
{
	/* Full rebuild from the raw line records. This must reset, not append:
	 * every load regenerates entries for ALL retained lines, so appending
	 * duplicated every previously-loaded module's entries. */
	addr_map_count = 0;

	for (int i = 0; i < line_count; i++) {
		const dbg_line_t *ln = &lines[i];
		for (int j = 0; j < ln->span_count; j++) {
			const dbg_span_t *sp = find_span(ln->spans[j]);
			if (!sp)
				continue;
			const dbg_seg_t *sg = find_seg(sp->seg);
			if (!sg)
				continue;

			uint32_t abs_addr = sg->start + sp->start;
			if (abs_addr > 0xFFFFFF)
				continue;

			uint32_t abs_end = abs_addr + (sp->size ? sp->size - 1 : 0);
			if (abs_end > 0xFFFFFF)
				abs_end = 0xFFFFFF;

			// Out of memory: stop adding entries, but still sort what we have
			// below -- the lookups binary-search these arrays.
			if (!GROW_ARRAY(addr_map, addr_map_count, addr_map_cap, addr_map_entry_t)) {
				goto sort;
			}
			addr_map[addr_map_count].addr     = (dbg_addr_t)abs_addr;
			addr_map[addr_map_count].end      = (dbg_addr_t)abs_end;
			addr_map[addr_map_count].file_id  = ln->file;
			addr_map[addr_map_count].line_num = ln->line;
			addr_map[addr_map_count].seg_id   = sp->seg;
			addr_map_count++;
		}
	}

sort:
	if (addr_map_count > 0)
		qsort(addr_map, (size_t)addr_map_count, sizeof(addr_map_entry_t), addr_map_cmp);

	if (sym_count > 0)
		qsort(syms, (size_t)sym_count, sizeof(dbg_sym_t), sym_cmp);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

// Read one whole line, however long, into a caller-owned growable buffer.
// Returns NULL at end of file or on allocation failure.
//
// A fixed buffer is not good enough here: cc65 records every reference to a
// symbol in its `sym` line, so for a heavily used label that line runs well
// past any reasonable fixed size -- and the fields that matter (val, type)
// come after the reference list, so a truncated line silently loses the symbol
// rather than failing loudly.
static char *read_line(FILE *f, char **buf, size_t *cap)
{
	size_t len = 0;
	for (;;) {
		if (*cap - len < 2) {
			size_t ncap = *cap ? *cap * 2 : 1024;
			char  *nb   = (char *)realloc(*buf, ncap);
			if (!nb) {
				return NULL;
			}
			*buf = nb;
			*cap = ncap;
		}
		if (!fgets(*buf + len, (int)(*cap - len), f)) {
			return len ? *buf : NULL; // EOF; return a final unterminated line
		}
		len += strlen(*buf + len);
		if (len > 0 && (*buf)[len - 1] == '\n') {
			return *buf;
		}
	}
}

int dbg_info_load(const char *path)
{
	FILE  *f;
	char  *buf = NULL;
	size_t cap = 0;
#ifdef _WIN32
	if (fopen_s(&f, path, "r") != 0 || !f) {
#else
	f = fopen(path, "r");
	if (!f) {
#endif
		fprintf(stderr, "dbg_info: cannot open '%s'\n", path);
		return -1;
	}

	// Only now that the file is definitely readable: don't free existing data,
	// so .dbg files load additively, but offset this file's IDs so they cannot
	// collide with those already loaded. Doing any of this before the open
	// would leave a failed load having quietly discarded the address map of
	// whatever was already loaded.
	id_base_file = next_id_file;
	id_base_seg = next_id_seg;
	id_base_span = next_id_span;
	id_base_line = next_id_line;

	// Per load: this file's `file` IDs mean nothing to the next one.
	file_alias_count = 0;

	// Records deduplicated below are pooled only with others from this same
	// .dbg; see dbg_owner_intern(). Interning can only fail on allocation
	// failure, and carrying on with -1 would pool this module with every other
	// load that failed the same way -- and, worse, hand that shared identity to
	// the generation sweep below, which would then discard all of them. Refuse
	// the load instead; nothing here is destructive yet.
	cur_owner = dbg_owner_intern(path);
	if (cur_owner < 0) {
		fclose(f);
		fprintf(stderr, "dbg_info: out of memory loading '%s'\n", path);
		return -1;
	}

	// Equates this module declared before are left in place while the file is
	// read, and dropped only once it has been read to the end. Sweeping them
	// first meant an empty or corrupt file -- one that opens fine and then
	// parses to nothing -- silently deleted the previous generation and still
	// reported success. Records from this parse are tagged with a new
	// generation so the two can be told apart while both are present.
	cur_gen++;

	// `line` records are resolved through the alias only after the whole file
	// is read, so a `line` that precedes its `file` still lands on the right
	// record. cc65 emits them in the other order today, but nothing promises
	// that, and getting it wrong silently drops source lines.
	int line_first = line_count;
	int records    = 0;
	int syms       = 0;
	long expect_syms = -1;   /* from the `info` record, -1 if it had none */

	// Clear addr_map — it is rebuilt below from all accumulated records.
	addr_map_count = 0;

	while (read_line(f, &buf, &cap)) {
		/* Strip trailing newline / carriage-return */
		size_t len = strlen(buf);
		while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
			buf[--len] = '\0';

		const char *p = skip_ws(buf);
		if (*p == '\0')
			continue;

		if (strncmp(p, "file", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
			parse_file_record(p + 4);
			records++;
		} else if (strncmp(p, "seg", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) {
			parse_seg_record(p + 3);
			records++;
		} else if (strncmp(p, "span", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
			parse_span_record(p + 4);
			records++;
		} else if (strncmp(p, "line", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
			parse_line_record(p + 4);
			records++;
		} else if (strncmp(p, "sym", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) {
			if (parse_sym_record(p + 3))
				syms++;
			records++;
		} else if (strncmp(p, "info", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
			/* ld65 states its own record counts up front. `sym=N` is what makes
			 * a complete file distinguishable from one still being written. */
			expect_syms = parse_int_key(p + 4, "sym");
		}
		/* Other record types (version, info, scope, …) are silently skipped. */
	}

	fclose(f);
	free(buf);

	// Decide what the file's own record counts say about it. ld65 states them
	// in the `info` line, so a file that promised N symbols and delivered N
	// structurally complete ones was written to the end; one that delivered
	// fewer was not.
	//
	// Both directions require positive evidence, and a file with no `info` line
	// supplies neither. It is merged, because refusing every .dbg that does not
	// come from ld65 would be a far bigger change than the problem warrants,
	// and nothing of its previous generation is dropped, because there is no
	// reason to believe this reading is the whole of it.
	//
	// An incomplete file is a failed load, not a partial success. It must not
	// report 0: dbg_load records a successful merge in its registry and then
	// skips the whole peek/unload/parse path on every later LOAD of the same
	// program, so a half-written .dbg latched there would stop the finished one
	// from ever being read. Failing sends it back for another go.
	//
	// The address map is still rebuilt, because it was cleared on the way in
	// and everything else that is loaded still needs it.
	bool short_read = (expect_syms >= 0 && syms != (int)expect_syms);
	if (records == 0 || short_read) {
		if (short_read)
			fprintf(stderr, "dbg_info: '%s' declares %ld symbols but has %d; not merging\n",
			        path, expect_syms, syms);
		else
			fprintf(stderr, "dbg_info: no usable records in '%s'\n", path);
		build_addr_map();
		return -1;
	}

	// Read to the end as far as anything here can tell, so this generation is
	// what the module declares now. Anything it declared last time and did not
	// declare again goes. Without an `info` line the best available signal is
	// whether the symbol block was reached at all -- weaker, but it has to be
	// something: the per-name replacement above only matches within a
	// generation, so without this a reload would leave the old value in front
	// of the new one for every lookup.
	if (expect_syms >= 0 || syms > 0)
		drop_stale_equates(cur_owner, cur_gen);

	// Now that every `file` record has been seen, turn this load's raw file IDs
	// into the records that actually describe them.
	for (int i = line_first; i < line_count; i++)
		lines[i].file = file_alias_get(lines[i].file);

	// Remember the directory this .dbg came from, for source-file discovery.
	{
		size_t plen = strlen(path);
		if (plen >= sizeof(dbg_dir))
			plen = sizeof(dbg_dir) - 1;
		memcpy(dbg_dir, path, plen);
		dbg_dir[plen] = '\0';
		char *s1 = strrchr(dbg_dir, '/');
		char *s2 = strrchr(dbg_dir, '\\');
		char *sep = (s2 > s1) ? s2 : s1;
		if (sep)
			*sep = '\0';       /* keep directory portion only */
		else
			dbg_dir[0] = '\0'; /* no directory component */
	}

	seed_banks_from_equates();
	build_addr_map();
	loaded = true;

	fprintf(stderr, "dbg_info: loaded %d files, %d segs, %d spans, %d lines, %d addr entries, %d symbols, %d equates\n",
	        file_count, seg_count, span_count, line_count, addr_map_count, sym_count, equ_count);

	return 0;
}

/* ---- Symbol (label) lookups ------------------------------------------------ */

bool dbg_info_addr_to_label(dbg_addr_t addr, const char **name)
{
	if (!loaded || sym_count == 0)
		return false;

	/* syms are sorted by address (build_addr_map). */
	int lo = 0, hi = sym_count - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		if (syms[mid].addr == addr) {
			if (name)
				*name = syms[mid].name;
			return true;
		} else if (syms[mid].addr < addr) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return false;
}

bool dbg_info_addr_to_label_nearest(dbg_addr_t addr, const char **name, dbg_addr_t *label_addr)
{
	if (!loaded || sym_count == 0)
		return false;

	/* syms are sorted by address (build_addr_map): find the LAST symbol at or
	 * below addr, i.e. the label the address falls inside. Callers that want an
	 * exact hit should use dbg_info_addr_to_label(). */
	int lo = 0, hi = sym_count - 1, best = -1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		if (syms[mid].addr <= addr) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	if (best < 0)
		return false;
	if (name)
		*name = syms[best].name;
	if (label_addr)
		*label_addr = syms[best].addr;
	return true;
}

bool dbg_info_label_to_addr(const char *name, dbg_addr_t *addr)
{
	if (!loaded || !name || !name[0])
		return false;

	for (int i = 0; i < sym_count; i++) {
		if (strcasecmp(syms[i].name, name) == 0) {
			if (addr)
				*addr = syms[i].addr;
			return true;
		}
	}
	return false;
}

bool dbg_info_equate_to_value(const char *name, dbg_addr_t *val)
{
	if (!loaded || !name || !name[0])
		return false;

	for (int i = 0; i < equ_count; i++) {
		if (strcasecmp(equs[i].name, name) == 0) {
			if (val)
				*val = equs[i].val;
			return true;
		}
	}
	return false;
}

bool dbg_info_name_to_value(const char *name, dbg_addr_t *val, int *kind)
{
	if (dbg_info_label_to_addr(name, val)) {
		if (kind)
			*kind = DBG_NAME_LABEL;
		return true;
	}
	if (dbg_info_equate_to_value(name, val)) {
		if (kind)
			*kind = DBG_NAME_EQUATE;
		return true;
	}
	return false;
}

int dbg_info_symbol_count(void)
{
	return loaded ? sym_count : 0;
}

bool dbg_info_symbol_at(int index, const char **name, dbg_addr_t *addr)
{
	if (!loaded || index < 0 || index >= sym_count)
		return false;
	if (name)
		*name = syms[index].name;
	if (addr)
		*addr = syms[index].addr;
	return true;
}

bool dbg_info_addr_to_source(dbg_addr_t addr, const char **file_path, int *line_num)
{
	if (!loaded || addr_map_count == 0)
		return false;

	/* Binary search for exact address match */
	int lo = 0, hi = addr_map_count - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		if (addr_map[mid].addr == addr) {
			const dbg_file_t *fi = find_file(addr_map[mid].file_id);
			if (!fi)
				return false;
			if (file_path)
				*file_path = fi->name;
			if (line_num)
				*line_num = addr_map[mid].line_num;
			return true;
		} else if (addr_map[mid].addr < addr) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return false;
}

/* True when entry `i` is eligible for `addr` given the RAM bank currently
 * mapped. Only banked addresses ($A000-$BFFF) are filtered, and only against
 * segments whose bank we actually learned — an unknown-bank segment always
 * stays eligible so we never lose a mapping we used to have. */
static bool entry_bank_ok(int i, dbg_addr_t addr, int ram_bank)
{
	if (ram_bank < 0 || addr < 0xA000 || addr > 0xBFFF)
		return true;
	const dbg_seg_t *sg = find_seg(addr_map[i].seg_id);
	if (!sg || sg->bank < 0)
		return true;
	return sg->bank == ram_bank;
}

/* How well an entry's segment matches the bank we are asking about. A confirmed
 * match beats an unknown one, which beats a mismatch -- entry_bank_ok() treats
 * unknown as eligible so a mapping is never lost, but eligible is not the same
 * as equally good, and picking between two eligible entries on span size alone
 * can swap a confirmed answer for a guess about a different segment. */
static int entry_bank_rank(int i, dbg_addr_t addr, int ram_bank)
{
	if (ram_bank < 0 || addr < 0xA000 || addr > 0xBFFF)
		return 2;
	const dbg_seg_t *sg = find_seg(addr_map[i].seg_id);
	if (!sg || sg->bank < 0)
		return 1;               /* eligible, but only because we do not know */
	return sg->bank == ram_bank ? 2 : 0;
}

bool dbg_info_addr_to_source_banked(dbg_addr_t addr, int ram_bank,
                                    const char **file_path, int *line_num)
{
	return dbg_info_addr_to_source_banked_ex(addr, ram_bank, file_path, line_num)
	       != DBG_BANK_NO_MATCH;
}

dbg_bank_result_t dbg_info_addr_to_source_banked_ex(dbg_addr_t addr, int ram_bank,
                                                    const char **file_path, int *line_num)
{
	if (!loaded || addr_map_count == 0)
		return DBG_BANK_NO_MATCH;

	/* Find the greatest entry with entry.addr <= addr. */
	int lo = 0, hi = addr_map_count - 1, best = -1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		if (addr_map[mid].addr <= addr) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	if (best < 0)
		return DBG_BANK_NO_MATCH;

	/* Walk backwards from the nearest-below entry to find the one whose span
	 * actually covers addr (spans are small, so this is cheap). Entries that
	 * belong to a different RAM bank are skipped. */
	int found = -1;
	for (int i = best; i >= 0; i--) {
		if (addr <= addr_map[i].end && entry_bank_ok(i, addr, ram_bank)) {
			found = i;
			break;
		}
		/* Stop once we are clearly past any reasonable span size. */
		if (addr_map[i].addr + 256 < addr)
			break;
	}
	if (found < 0) {
		/* Nothing in this bank — fall back to a bank-agnostic match rather than
		 * showing no source at all, but say that is what happened: the answer
		 * may belong to a different bank's segment at the same address. */
		if (ram_bank < 0)
			return DBG_BANK_NO_MATCH;
		return dbg_info_addr_to_source_banked_ex(addr, -1, file_path, line_num) == DBG_BANK_NO_MATCH
		           ? DBG_BANK_NO_MATCH
		           : DBG_BANK_UNKNOWN;
	}

	/* Several spans commonly START at the same address (the statement, its
	 * enclosing block/function, macro and include spans). Among those that
	 * cover addr, pick the SMALLEST — the innermost, most specific line — so
	 * stepping highlights the actual statement rather than an arbitrary
	 * enclosing span. Equal-start entries are contiguous after the sort.
	 *
	 * Bank confidence outranks span size. Two segments commonly share $A000,
	 * one per bank, and only the one that has been loaded has a known bank; if
	 * the smaller happens to be the unknown one, preferring it reports the
	 * OTHER segment's file and line and demotes a confirmed answer to a guess.
	 * So narrow only within the same confidence. */
	{
		dbg_addr_t start = addr_map[found].addr;
		int lo2 = found, hi2 = found;
		while (lo2 > 0 && addr_map[lo2 - 1].addr == start)
			lo2--;
		while (hi2 + 1 < addr_map_count && addr_map[hi2 + 1].addr == start)
			hi2++;
		int best_rank = entry_bank_rank(found, addr, ram_bank);
		for (int i = lo2; i <= hi2; i++) {
			if (addr > addr_map[i].end)
				continue;
			const int rank = entry_bank_rank(i, addr, ram_bank);
			if (rank == 0)
				continue;                       /* a bank we know it is not */
			if (rank > best_rank
			    || (rank == best_rank && addr_map[i].end < addr_map[found].end)) {
				found     = i;
				best_rank = rank;
			}
		}
	}

	{
		const dbg_file_t *fi = find_file(addr_map[found].file_id);
		if (!fi)
			return DBG_BANK_NO_MATCH;
		if (file_path)
			*file_path = fi->name;
		if (line_num)
			*line_num = addr_map[found].line_num;

		/* Resolved only if the segment we matched actually knows its bank; a
		 * banked address matched against an unknown-bank segment is a guess. */
		if (ram_bank >= 0 && addr >= 0xA000 && addr <= 0xBFFF) {
			const dbg_seg_t *sg = find_seg(addr_map[found].seg_id);
			if (!sg || sg->bank < 0)
				return DBG_BANK_UNKNOWN;
		}
		return DBG_BANK_RESOLVED;
	}
}

bool dbg_info_addr_to_source_nearest(dbg_addr_t addr, const char **file_path, int *line_num)
{
	return dbg_info_addr_to_source_banked(addr, -1, file_path, line_num);
}

const char *dbg_info_get_dbg_dir(void)
{
	return dbg_dir;
}

int dbg_info_file_count(void)
{
	return file_count;
}

bool dbg_info_file_at(int index, const char **name)
{
	if (index < 0 || index >= file_count)
		return false;
	if (name)
		*name = files[index].name;
	return true;
}

int dbg_info_scan_dbg_files(const char *dir, char out[][DBG_INFO_PATH_MAX], int max)
{
	if (!dir || !dir[0] || !out || max <= 0)
		return 0;

	DIR *dp = opendir(dir);
	if (!dp)
		return 0;

	size_t dlen    = strlen(dir);
	bool   need_sep = dlen > 0 && dir[dlen - 1] != '/' && dir[dlen - 1] != '\\';

	int            n = 0;
	struct dirent *de;
	while (n < max && (de = readdir(dp)) != NULL) {
		const char *nm = de->d_name;
		size_t      l  = strlen(nm);
		if (l < 5 || strcasecmp(nm + l - 4, ".dbg") != 0)
			continue; // want "<something>.dbg"
		int w = snprintf(out[n], DBG_INFO_PATH_MAX, "%s%s%s", dir, need_sep ? "/" : "", nm);
		if (w > 0 && w < DBG_INFO_PATH_MAX)
			n++;
	}
	closedir(dp);
	return n;
}

bool dbg_info_source_to_addr(const char *file_path, int line_num, dbg_addr_t *addr)
{
	if (!loaded || !file_path)
		return false;

	const char *query_base = basename_ptr(file_path);

	for (int i = 0; i < addr_map_count; i++) {
		if (addr_map[i].line_num != line_num)
			continue;

		const dbg_file_t *fi = find_file(addr_map[i].file_id);
		if (!fi)
			continue;

		const char *entry_base = basename_ptr(fi->name);
		if (strcasecmp(query_base, entry_base) == 0) {
			if (addr)
				*addr = addr_map[i].addr;
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------ */
/*  Banked-segment -> RAM bank association                             */
/*                                                                     */
/*  Several segments typically all start at $A000 — they are different */
/*  X16 RAM banks. The .dbg records no bank number, so an address in   */
/*  $A000-$BFFF is ambiguous across every banked segment. We learn the  */
/*  mapping two ways:                                                  */
/*    1. `RAM_BANK_<SEG>` / `<SEG>_BANK` equate symbols emitted by the  */
/*       program's own source (covers BSS segments that are never       */
/*       loaded from a file), and                                       */
/*    2. runtime LOADs: a blob of size N landing at the segment's start */
/*       while RAM bank B is mapped means that segment lives in bank B. */
/* ------------------------------------------------------------------ */

/* Squash a name for fuzzy comparison: uppercase, drop '_'. Returns false if the
 * name did not fit, since a truncated name compares equal to anything sharing
 * its head and would invent matches that the full names do not support. */
static bool squash_name(const char *src, char *dst, size_t dstsz)
{
	size_t o = 0;
	for (; src && *src; src++) {
		if (*src == '_')
			continue;
		if (o + 1 >= dstsz) {
			dst[0] = '\0';
			return false;
		}
		char c = *src;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		dst[o++] = c;
	}
	dst[o] = '\0';
	return true;
}

/* Reduce a `RAM_BANK_x` / `BANK_x` / `x_BANK` equate name to the squashed
 * segment name it is talking about. Returns false if it is not one of those. */
static bool bank_equ_target(const char *nm, char *out, size_t outsz)
{
	if (!nm)
		return false;
	const char *body;
	size_t      blen;
	if (!strncmp(nm, "RAM_BANK_", 9)) {
		body = nm + 9;
		blen = strlen(body);
	} else if (!strncmp(nm, "BANK_", 5)) {
		body = nm + 5;
		blen = strlen(body);
	} else {
		size_t l = strlen(nm);
		if (l > 5 && !strcmp(nm + l - 5, "_BANK")) {
			body = nm;
			blen = l - 5;
		} else {
			return false;
		}
	}

	/* Truncating here would silently invent matches: two names sharing a long
	 * enough head would reduce to the same target and compare exactly equal.
	 * A name this long is not one of these constants, so refuse it instead. */
	char stripped[128];
	if (blen >= sizeof stripped)
		return false;
	memcpy(stripped, body, blen);
	stripped[blen] = '\0';

	if (!squash_name(stripped, out, outsz))
		return false;
	return out[0] != '\0';
}
/* Seed segment banks from `RAM_BANK_x` / `x_BANK` style equates parsed out of
 * the .dbg. Matching is on the squashed names.
 *
 * Driven from the segment rather than the equate. Walking the equates and
 * claiming every segment that matched put the answer at the mercy of the order
 * the equates happened to be parsed in: a prefix match is accepted in either
 * direction, so RAM_BANK_CODE matches segment CODE2 as readily as CODE, and
 * because a segment that already has a bank is skipped, whichever equate was
 * seen first won -- leaving the exact RAM_BANK_CODE2 to be discarded and CODE2's
 * code attributed to the wrong bank.
 *
 * Per segment, every equate is sorted into one of four ranks: own-exact,
 * other-exact, own-prefix, other-prefix. The best non-empty rank decides, and
 * decides alone -- if its candidates disagree about the bank the segment is
 * left unknown rather than falling through to a weaker rank. Callers report
 * unknown honestly and a runtime observation can still resolve it, whereas a
 * confident wrong answer is not recoverable. */
static void seed_banks_from_equates(void)
{
	/* Equate-derived banks are recomputed from scratch, because the equates
	 * they came from may have changed. A module that reloads with a different
	 * RAM_BANK_x value can have seeded segments belonging to OTHER modules
	 * through the cross-module ranks, and those segments are not unloaded --
	 * so without this they would keep a bank nobody declares any more. Banks
	 * that were observed at runtime are left alone: an observation is evidence,
	 * an equate is only an inference, and the observation must keep winning. */
	for (int i = 0; i < seg_count; i++) {
		if (segs[i].bank_from_equ) {
			segs[i].bank          = -1;
			segs[i].bank_from_equ = false;
		}
	}

	/* Squash each equate's target once rather than once per segment. Matching
	 * is O(segments x equates) either way, but this keeps the string work out
	 * of the inner loop -- seeding runs from the emulator's main loop via the
	 * auto-load path, so a large .dbg must not stall emulation. */
	char *targets = NULL;
	if (bank_equ_count > 0) {
		if ((size_t)bank_equ_count > SIZE_MAX / 128)
			return;   /* not reachable with real input; the product must not wrap */
		targets = (char *)malloc((size_t)bank_equ_count * 128);
		if (!targets)
			return;   /* leaves banks unknown, which callers report honestly */
		for (int b = 0; b < bank_equ_count; b++) {
			if (!bank_equ_target(bank_equs[b].name, targets + (size_t)b * 128, 128))
				targets[(size_t)b * 128] = '\0';
		}
	}

	for (int i = 0; i < seg_count; i++) {
		if (segs[i].bank >= 0 || !segs[i].name || !segs[i].size)
			continue;
		if (segs[i].start < 0xA000 || segs[i].start > 0xBFFF)
			continue;

		char have[128];
		if (!squash_name(segs[i].name, have, sizeof have) || !have[0])
			continue;   /* truncated names would match things they do not equal */
		size_t lh = strlen(have);

		/* Candidates are ranked by how good the name match is first and by who
		 * declared the equate second: own-exact, other-exact, own-prefix,
		 * other-prefix. Gating the whole search on the segment's own module
		 * instead was wrong -- an own-module PREFIX match would end the search
		 * before another module's EXACT match was ever looked at, which is the
		 * same "claimed by a name it merely resembles" failure this ranking
		 * exists to prevent, just across a module boundary.
		 *
		 * Ownership still matters, because two overlays that each define
		 * RAM_BANK_CODE for their own CODE segment -- an ordinary X16
		 * arrangement -- would otherwise see two equally good candidates and
		 * both end up unknown. */
		enum { OWN_EXACT, ANY_EXACT, OWN_PREFIX, ANY_PREFIX, RANK_COUNT };
		int  cand[RANK_COUNT];
		bool ambiguous[RANK_COUNT];
		for (int r = 0; r < RANK_COUNT; r++) {
			cand[r]      = -1;
			ambiguous[r] = false;
		}

		for (int b = 0; b < bank_equ_count; b++) {
			const char *want = targets + (size_t)b * 128;
			if (!want[0])
				continue;
			bool own = (bank_equs[b].owner == segs[i].owner);
			int  rank;
			if (!strcmp(have, want)) {
				rank = own ? OWN_EXACT : ANY_EXACT;
			} else {
				/* Either direction, so RAM_BANK_STORE_TILEMAP pairs with
				 * segment STORETILE, which is what these conventions produce.
				 * Four characters is the shortest overlap worth trusting. */
				size_t lw = strlen(want);
				size_t n  = lh < lw ? lh : lw;
				if (n < 4 || strncmp(have, want, n) != 0)
					continue;
				rank = own ? OWN_PREFIX : ANY_PREFIX;
			}
			if (cand[rank] < 0)
				cand[rank] = b;
			else if (bank_equs[cand[rank]].bank != bank_equs[b].bank)
				ambiguous[rank] = true;
		}

		/* The best kind of match available decides, and it decides alone: if
		 * the candidates of that kind disagree there is no basis for choosing
		 * between them, and falling through to a weaker kind would answer a
		 * question the better evidence just said was undecidable. Unknown is
		 * reported honestly and a runtime observation can still resolve it. */
		for (int r = 0; r < RANK_COUNT; r++) {
			if (cand[r] < 0)
				continue;
			if (!ambiguous[r]) {
				segs[i].bank          = bank_equs[cand[r]].bank;
				segs[i].bank_from_equ = true;
			}
			break;
		}
	}

	free(targets);
}

void dbg_info_note_bank_load(dbg_addr_t load_addr, uint32_t size, uint8_t ram_bank)
{
	if (!loaded || load_addr < 0xA000 || load_addr > 0xBFFF || size == 0)
		return;

	/* Match the loaded blob to exactly one segment by (start,size). A PRG-style
	 * 2-byte load-address header may or may not be counted, so accept both. */
	int hit = -1;
	for (int i = 0; i < seg_count; i++) {
		if (!segs[i].size || segs[i].start != load_addr)
			continue;
		if (segs[i].size == size || segs[i].size + 2 == size) {
			if (hit >= 0)
				return;   /* ambiguous (two segments same start+size) — skip */
			hit = i;
		}
	}
	if (hit >= 0) {
		segs[hit].bank = ram_bank;
		/* Observed, not inferred: re-seeding must not clear this. */
		segs[hit].bank_from_equ = false;
	}
}

bool dbg_info_is_loaded(void)
{
	return loaded;
}

bool dbg_info_is_span_start(dbg_addr_t addr)
{
	if (!loaded || addr_map_count == 0)
		return false;

	/* Binary search the sorted address map for an exact span-start match. */
	int lo = 0, hi = addr_map_count - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		if (addr_map[mid].addr == addr)
			return true;
		else if (addr_map[mid].addr < addr)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

void dbg_info_unload_range(dbg_addr_t start, dbg_addr_t end)
{
	if (!loaded) return;

	// Drop the RAW line records that map into this range FIRST. build_addr_map()
	// regenerates addr_map from lines[] on every load, so pruning only the
	// derived map below would be silently undone by the next dbg_info_load():
	// the replaced module's lines would be re-added and could then win the
	// address lookup (a swapped-out module's file/line shown for the new
	// module's code). Freeing the line's span-id array here also stops the
	// swap from leaking it.
	{
		int ldst = 0;
		for (int i = 0; i < line_count; i++) {
			bool in_range = false;
			for (int j = 0; j < lines[i].span_count && !in_range; j++) {
				const dbg_span_t *sp = find_span(lines[i].spans[j]);
				if (!sp)
					continue;
				const dbg_seg_t *sg = find_seg(sp->seg);
				if (!sg)
					continue;
				uint32_t a = sg->start + sp->start;
				if (a <= 0xFFFFFF && a >= start && a <= end)
					in_range = true;
			}
			if (in_range)
				free(lines[i].spans);
			else
				lines[ldst++] = lines[i];
		}
		line_count = ldst;
	}

	// NOTE: do not early-out on addr_map_count==0 — a symbol-only / line-less
	// .dbg can have zero addr-map entries but non-zero symbols, and the symbol
	// cleanup below must still run (otherwise repeated module swaps leak/stale
	// labels). The addr-map compaction loop is a no-op when the map is empty.

	// Remove all addr_map entries in [start, end]
	int dst = 0;
	for (int src = 0; src < addr_map_count; src++) {
		if (addr_map[src].addr < start || addr_map[src].addr > end) {
			addr_map[dst++] = addr_map[src];
		}
	}
	addr_map_count = dst;

	// Remove symbols in [start, end] too, so a module swap drops its labels
	// (freeing their names). The array stays sorted by address.
	int sdst = 0;
	for (int src = 0; src < sym_count; src++) {
		if (syms[src].addr < start || syms[src].addr > end) {
			syms[sdst++] = syms[src];
		} else {
			free(syms[src].name);
		}
	}
	sym_count = sdst;

	// Finally drop the spans and segments that lived in this range. Without
	// this, reloading a module appends a second copy of its segments, and
	// dbg_info_note_bank_load() -- which needs a unique (start, size) match to
	// decide which RAM bank a segment lives in -- sees two candidates and gives
	// up. From the first module swap onwards no segment would ever be assigned
	// a bank again, and banked $A000-$BFFF addresses would resolve to whichever
	// module happened to match first.
	//
	// Entries keep their IDs; only the arrays are compacted. Lookups fall back
	// to a scan by ID, and new IDs come from a high-water mark, so nothing is
	// invalidated by the holes this leaves. files[] is deliberately left alone:
	// callers hold the file_path pointers it owns.
	{
		int pdst = 0;
		for (int i = 0; i < span_count; i++) {
			const dbg_seg_t *sg = find_seg(spans[i].seg);
			uint32_t a = sg ? (uint32_t)sg->start + spans[i].start : 0xFFFFFFFFu;
			if (sg && a <= 0xFFFFFF && a >= start && a <= end) {
				continue; // in range: drop
			}
			spans[pdst++] = spans[i];
		}
		span_count = pdst;

		int gdst = 0;
		for (int i = 0; i < seg_count; i++) {
			uint32_t s = segs[i].start;
			uint32_t e = segs[i].size ? s + segs[i].size - 1 : s;
			if (s >= start && e <= end) {
				free(segs[i].name); // owned by this module; nothing else refers to it
				continue;           // wholly inside the unloaded range: drop
			}
			segs[gdst++] = segs[i];
		}
		seg_count = gdst;
	}
}

// Helper: build .dbg path from a loaded file path. Returns false if the result
// would not fit, rather than writing past the caller's buffer -- loaded_path
// can come from an emulated LOAD, so its length is not ours to trust.
static bool build_dbg_path(const char *loaded_path, char *dbg_path, size_t dbg_path_size) {
	static const char ext[] = ".dbg";
	const size_t ext_len = sizeof(ext) - 1;

	size_t len = strlen(loaded_path);
	if (dbg_path_size <= ext_len || len >= dbg_path_size) {
		return false;
	}
	memcpy(dbg_path, loaded_path, len);
	dbg_path[len] = '\0';

	char *last_sep = strrchr(dbg_path, '/');
	char *last_sep2 = strrchr(dbg_path, '\\');
	if (last_sep2 > last_sep) last_sep = last_sep2;
	char *dot = strrchr(last_sep ? last_sep : dbg_path, '.');

	// Where the extension will start: replacing an existing one, or appended.
	size_t at = dot ? (size_t)(dot - dbg_path) : len;
	if (at + ext_len >= dbg_path_size) {
		return false;
	}
	memcpy(dbg_path + at, ext, ext_len + 1);
	return true;
}

// Helper: scan .dbg file for min/max segment addresses
static bool scan_dbg_seg_range(const char *dbg_path, dbg_addr_t *out_min, dbg_addr_t *out_max) {
	FILE *f;
#ifdef _WIN32
	if (fopen_s(&f, dbg_path, "r") != 0 || !f) return false;
#else
	f = fopen(dbg_path, "r");
	if (!f) return false;
#endif
	dbg_addr_t seg_min = 0xFFFFFF, seg_max = 0;
	char    *buf = NULL;
	size_t   cap = 0;
	while (read_line(f, &buf, &cap)) {
		if (strncmp(buf, "seg", 3) != 0) continue;

		const char *sp = strstr(buf, "start=0x");
		const char *sz = strstr(buf, "size=0x");
		if (!sp || !sz) continue;
		uint32_t start = (uint32_t)strtol(sp + 8, NULL, 16);
		uint32_t size = (uint32_t)strtol(sz + 7, NULL, 16);
		if (size == 0 || start > 0xFFFFFF) continue;
		if (start < seg_min) seg_min = (dbg_addr_t)start;
		uint32_t end = start + size - 1;
		if (end > 0xFFFFFF) end = 0xFFFFFF;
		if (end > seg_max) seg_max = (dbg_addr_t)end;
	}
	free(buf);
	fclose(f);
	if (seg_min > seg_max) return false;
	*out_min = seg_min;
	*out_max = seg_max;
	return true;
}



bool dbg_info_peek_file_range(const char *loaded_path, dbg_addr_t *out_start, dbg_addr_t *out_end) {
	if (!loaded_path) return false;
	char dbg_path[1024];
	if (!build_dbg_path(loaded_path, dbg_path, sizeof(dbg_path))) return false;
	return scan_dbg_seg_range(dbg_path, out_start, out_end);
}

int dbg_info_load_for_file(const char *loaded_path, dbg_addr_t load_addr)
{
	if (!loaded_path) return -1;

	char dbg_path[1024];
	if (!build_dbg_path(loaded_path, dbg_path, sizeof(dbg_path))) return -1;

	dbg_addr_t seg_min, seg_max;
	if (!scan_dbg_seg_range(dbg_path, &seg_min, &seg_max))
		return -1;

	// Unload existing entries in this range, then load new .dbg.
	//
	// This range is the min/max across EVERY segment the file declares,
	// including BSS and the zero page, which for a normal cc65 program runs from
	// low RAM to the top of the program. That is wider than what the load
	// actually overwrote, so it takes other modules in that window with it --
	// including debug info the user named with -dbgfile, which nothing
	// re-merges.
	//
	// Narrowing it to the stored segments alone was tried and is worse:
	// segment, span, line and label records are still appended unconditionally,
	// so this module's own BSS and zero-page records would no longer be pruned
	// but would still be re-added, piling up on every overlay swap until an
	// address could be described by a module that is no longer resident. The
	// two jobs -- evicting other modules, and replacing this one's own records
	// -- need different ranges. Files, equates and segments now carry an owner;
	// finishing this means extending it to spans/lines/syms and having unload
	// consult it. See dbg_info.h.
	dbg_info_unload_range(seg_min, seg_max);
	int result = dbg_info_load(dbg_path);
	if (result == 0) {
		printf("[dap] Auto-loaded debug info: %s ($%04X-$%04X)\n",
		       dbg_path, seg_min, seg_max);
	}
	return result;
}

void dbg_info_free(void)
{
	for (int i = 0; i < owner_count; i++)
		free(owners[i]);
	free(owners);
	owners      = NULL;
	owner_count = 0;
	owner_cap   = 0;   /* GROW_ARRAY keys off this; leaving it set would append
	                    * into a dangling pointer on the next load. */
	cur_owner   = -1;

	free(file_alias);
	file_alias       = NULL;
	file_alias_count = 0;
	file_alias_cap   = 0;

	for (int i = 0; i < file_count; i++)
		free(files[i].name);
	free(files);
	files = NULL;
	file_count = 0;
	file_cap = 0;

	for (int i = 0; i < seg_count; i++)
		free(segs[i].name);
	free(segs);
	segs = NULL;
	seg_count = 0;
	seg_cap = 0;

	for (int i = 0; i < bank_equ_count; i++)
		free(bank_equs[i].name);
	free(bank_equs);
	bank_equs = NULL;
	bank_equ_count = 0;
	bank_equ_cap = 0;

	free(spans);
	spans = NULL;
	span_count = 0;
	span_cap = 0;

	for (int i = 0; i < line_count; i++)
		free(lines[i].spans);
	free(lines);
	lines = NULL;
	line_count = 0;
	line_cap = 0;

	free(addr_map);
	addr_map = NULL;
	addr_map_count = 0;
	addr_map_cap = 0;

	// Nothing is loaded any more, so IDs can start from zero again.
	id_base_file = id_base_seg = id_base_span = id_base_line = 0;
	next_id_file = next_id_seg = next_id_span = next_id_line = 0;

	for (int i = 0; i < sym_count; i++)
		free(syms[i].name);
	free(syms);
	syms = NULL;
	sym_count = 0;
	sym_cap = 0;

	for (int i = 0; i < equ_count; i++)
		free(equs[i].name);
	free(equs);
	equs = NULL;
	equ_count = 0;
	equ_cap = 0;

	dbg_dir[0] = '\0';
	loaded = false;
}
