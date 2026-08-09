#ifndef _DBG_INFO_H_
#define _DBG_INFO_H_

#include <stdint.h>
#include <stdbool.h>

// A debug-info address. Flat 24-bit: bits 0-15 are the CPU address, bits 16-23
// the 65C816 program bank. On the 65C02 the high byte is simply zero. Code on a
// GS/Gen2 machine lives above $FFFF, so a 16-bit address cannot describe it.
typedef uint32_t dbg_addr_t;

// How confident a banked lookup is about the answer it gave.
typedef enum {
	DBG_BANK_NO_MATCH = 0, // no debug info covers this address
	DBG_BANK_RESOLVED,     // matched a segment whose RAM bank is known
	DBG_BANK_UNKNOWN,      // matched, but which bank this segment belongs to
	                       // was never established, so the answer may belong to
	                       // a different bank's segment at the same address
} dbg_bank_result_t;

// Load and parse a cc65 .dbg file. Returns 0 on success, -1 on error.
int dbg_info_load(const char *path);

// Look up source location for a given address.
// Returns true if found, fills in file_path and line_num.
// file_path points to internal storage — do not free.
bool dbg_info_addr_to_source(dbg_addr_t addr, const char **file_path, int *line_num);

// Like dbg_info_addr_to_source, but returns the line whose span *encloses* addr
// (nearest line starting at or below addr and covering it). This keeps the
// current-line highlight correct when the PC lands in the middle of a multi-byte
// instruction while stepping.
bool dbg_info_addr_to_source_nearest(dbg_addr_t addr, const char **file_path, int *line_num);

// Directory of the most recently loaded .dbg file (for locating source files).
// Returns an empty string if no .dbg has been loaded or it had no directory part.
const char *dbg_info_get_dbg_dir(void);

// Look up address for a given source location.
// Returns true if found, fills in addr.
bool dbg_info_source_to_addr(const char *file_path, int line_num, dbg_addr_t *addr);

// Check if debug info is loaded
bool dbg_info_is_loaded(void);

// Disassembly alignment helper: returns true if `addr` is the start address of
// a known cc65 .dbg span/line (i.e. a compiler-emitted instruction boundary).
// Used by the anchored disassembler (code_map) as a fallback alignment anchor
// when live execution coverage is unavailable for that address.
bool dbg_info_is_span_start(dbg_addr_t addr);

// Remove all debug mappings in an address range (for module swap)
void dbg_info_unload_range(dbg_addr_t start, dbg_addr_t end);

// Peek at a .dbg file's segment range without loading it.
// Returns true if a matching .dbg file exists and has valid segments.
bool dbg_info_peek_file_range(const char *loaded_path, dbg_addr_t *out_start, dbg_addr_t *out_end);

// Auto-load .dbg file matching a loaded binary. Unloads overlapping range first.
// Returns 0 on success, -1 if no .dbg found.
// NOTE: load_addr is currently ignored -- the debug info always describes the
// program at its link-time addresses. A relocating load (secondary address 0)
// therefore reports source lines for where the program was linked, not where it
// actually landed.
int dbg_info_load_for_file(const char *loaded_path, dbg_addr_t load_addr);

/* Address -> source, disambiguated by the X16 RAM bank currently mapped at
 * $A000-$BFFF. Programs routinely place several segments at $A000 (one per RAM
 * bank); the .dbg records no bank, so a banked address otherwise matches every
 * one of them and the reported file/line is effectively arbitrary. Pass the
 * live RAM bank (memory_get_ram_bank()) to pick the right one; pass -1 to skip
 * bank filtering. dbg_info_addr_to_source_nearest() is the ram_bank = -1 form.
 *
 * Segments whose bank could not be established stay eligible, so a mapping is
 * never lost -- but the answer may then belong to another bank's segment at the
 * same address. Use the _ex form to tell the two cases apart and say so, rather
 * than showing a confident file/line that might be from the wrong bank. */
bool dbg_info_addr_to_source_banked(dbg_addr_t addr, int ram_bank,
                                    const char **file_path, int *line_num);

dbg_bank_result_t dbg_info_addr_to_source_banked_ex(dbg_addr_t addr, int ram_bank,
                                                    const char **file_path, int *line_num);

/* Tell the debug info that a runtime LOAD placed `size` bytes at `load_addr`
 * while RAM bank `ram_bank` was mapped. When that uniquely identifies a banked
 * segment, the segment is associated with that bank. No-op outside
 * $A000-$BFFF. (Banks are also seeded from RAM_BANK_* equates in the .dbg.) */
void dbg_info_note_bank_load(dbg_addr_t load_addr, uint32_t size, uint8_t ram_bank);

// Free all loaded debug info
void dbg_info_free(void);

// ---- Symbol / label lookups (from cc65 .dbg `sym` records) ------------------
// Symbol names point to internal storage owned by the loaded debug info. Unlike
// file paths, they do NOT survive a module swap: dbg_info_unload_range() frees
// the names of the symbols it drops. Copy anything that has to outlive a load
// or unload.
// Address label at exactly `addr` (type=lab). `name` points to internal storage
// — do not free. Returns false if none.
bool dbg_info_addr_to_label(dbg_addr_t addr, const char **name);

// Label that `addr` falls inside: the nearest one at or below it, with its own
// address returned in `label_addr` so callers can render "name+offset".
//
// A return address on the stack points just past a JSR, which is almost never
// exactly a label, so an exact lookup leaves every caller frame anonymous. This
// is what the Call Stack needs to name its frames.
//
// It will happily match a far-away label for an address the debug info does not
// really cover, so only trust it where the address is known to be in mapped
// code (e.g. when dbg_info_addr_to_source_banked() also succeeds).
bool dbg_info_addr_to_label_nearest(dbg_addr_t addr, const char **name, dbg_addr_t *label_addr);

// Address of a label by name (case-insensitive, exact match). Returns false if
// no such label.
bool dbg_info_label_to_addr(const char *name, dbg_addr_t *addr);

// Value of a cc65 `equ` record by name (case-insensitive). Equates are the
// KERNAL entry points, hardware register names, zero-page variable addresses
// and plain constants a program defines; they are deliberately kept out of the
// label map (which stays a clean address→name map for the disassembler) but are
// exactly what a user hovers in source. Returns false if no such equate.
bool dbg_info_equate_to_value(const char *name, dbg_addr_t *val);

// What kind of name dbg_info_name_to_value() resolved.
#define DBG_NAME_LABEL  0
#define DBG_NAME_EQUATE 1

// Resolve any name the debug info knows: program labels first, then equates.
// `kind` (optional) receives DBG_NAME_LABEL or DBG_NAME_EQUATE.
bool dbg_info_name_to_value(const char *name, dbg_addr_t *val, int *kind);

// Enumerate all address labels (sorted by address). dbg_info_symbol_at fills
// `name` (internal storage) and `addr` for 0 <= index < dbg_info_symbol_count().
int  dbg_info_symbol_count(void);
bool dbg_info_symbol_at(int index, const char **name, dbg_addr_t *addr);

// ---- Source-file enumeration (for the debugger's file picker) ---------------
// Maximum path length used by dbg_info_scan_dbg_files().
#define DBG_INFO_PATH_MAX 260

// Enumerate the source files referenced by the loaded .dbg (by the name as
// recorded — usually a basename). Lets the debugger pre-open any known source
// file to set breakpoints / run-to before the PC reaches it. `name` points to
// internal storage — do not free. Valid for 0 <= index < dbg_info_file_count().
int  dbg_info_file_count(void);
bool dbg_info_file_at(int index, const char **name);

// Scan a directory (non-recursive) for cc65 ".dbg" files, writing up to `max`
// full paths into `out`. Portable (uses dirent). Safe if dir is NULL/empty.
// Returns the number of paths written. Lets the debugger surface other .dbg
// modules that the running code may LOAD later, so their sources/line
// breakpoints can be queued up ahead of time.
int  dbg_info_scan_dbg_files(const char *dir, char out[][DBG_INFO_PATH_MAX], int max);

#endif
