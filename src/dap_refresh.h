// Commander X16 Emulator — what an attached editor has yet to be told.
//
// Changes are recorded wherever they happen, including inside the CPU loop, and
// sent later from the server's poll, because sending can drop the client. This
// holds the record. It knows nothing of sockets or of the debug info, so it links
// alone in a unit test.

#ifndef _DAP_REFRESH_H_
#define _DAP_REFRESH_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Memory ranges ----------------------------------------------------------
// However many writes land between two polls, at most this many ranges come out.
#define REFRESH_MAX_RANGES 8

typedef struct {
	uint32_t start;
	uint32_t count;
} refresh_range_t;

typedef struct {
	refresh_range_t r[REFRESH_MAX_RANGES + 1]; // sorted; the spare is for an insert
	int             n;
} refresh_ranges_t;

void refresh_ranges_clear(refresh_ranges_t *s);

// `count` bytes from `start` in an address space of `space` bytes, wrapping at
// its end. Ranges that overlap or touch merge. Past the limit the two closest
// merge, so a range can grow to cover bytes that did not change but never
// fails to cover one that did.
void refresh_ranges_add(refresh_ranges_t *s, uint32_t start, uint32_t count, uint32_t space);

// The lowest range, removed. False when none is left.
bool refresh_ranges_take(refresh_ranges_t *s, refresh_range_t *out);

// ---- Loaded sources ---------------------------------------------------------
// One entry of the set a client can list. `revision` moves when the file is read
// again. `path` is borrowed for the length of the call.
typedef struct {
	const char *path;
	unsigned    revision;
	bool        label;     // a label file rather than a source named by a .dbg
} refresh_source_t;

typedef enum {
	REFRESH_SOURCE_NEW,
	REFRESH_SOURCE_CHANGED,
	REFRESH_SOURCE_REMOVED,
} refresh_source_reason_t;

typedef struct {
	refresh_source_reason_t reason;
	char                   *path;  // owned; see refresh_source_changes_free
	bool                    label;
} refresh_source_change_t;

typedef struct refresh_seen refresh_seen_t;

// The set the client was last told about. Zero-initialised is empty.
typedef struct {
	refresh_seen_t *v;
	int             n;
} refresh_sources_t;

void refresh_sources_free(refresh_sources_t *s);

// Take `cur` as already known, reporting nothing. Sorts `cur`.
void refresh_sources_baseline(refresh_sources_t *s, refresh_source_t *cur, int n);

// What differs between the known set and `cur`, up to `max` changes, each then
// counted as known. *more is set when some were left for another call. Sorts
// `cur`. Returns how many were written to `out`.
int refresh_sources_diff(refresh_sources_t *s, refresh_source_t *cur, int n,
                         refresh_source_change_t *out, int max, bool *more);

void refresh_source_changes_free(refresh_source_change_t *c, int n);

#ifdef __cplusplus
}
#endif

#endif // _DAP_REFRESH_H_
