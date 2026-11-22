// Commander X16 Emulator — memory search.
//
// Finds a byte pattern in any flat run of memory. Memory is read through a
// callback, so this has no dependency on the machine and the debug panel and
// the debug server share one matcher.

#ifndef _MEM_SEARCH_H_
#define _MEM_SEARCH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEM_SEARCH_MAX_PATTERN 64

// Bytes one call to mem_search_scan() may read from a server request. Below the
// default banked RAM (512K), so that a long search yields to the main loop.
#define MEM_SEARCH_BUDGET 0x40000u

// Must read without side effects.
typedef uint8_t (*mem_search_read_t)(uint32_t offset, void *ctx);

// Hex digits into `out` (MEM_SEARCH_MAX_PATTERN bytes). False for an empty or
// over-long pattern or a character that is not a digit or whitespace.
//
// Lenient: whitespace is ignored and a trailing lone digit is the last byte's low
// nibble. Strict: whitespace may only separate bytes, and the digits must pair up.
bool mem_search_parse_hex(const char *s, bool strict, uint8_t *out, size_t *len);

// The string's bytes into `out`. False if it is empty or over-long.
bool mem_search_parse_text(const char *s, uint8_t *out, size_t *len);

// The nearest match at or after `from` (dir >= 0) or at or before it (dir < 0),
// wrapping over the candidate starts [0, size - m]. Returns its offset, or -1.
long mem_search_find(mem_search_read_t read, void *ctx, size_t size,
                     const uint8_t *pat, size_t m, long from, int dir);

typedef enum {
	MEM_SEARCH_DONE,   // every candidate was examined
	MEM_SEARCH_LIMIT,  // `limit` matches were found
	MEM_SEARCH_SPENT,  // the read budget ran out
} mem_search_stop_t;

// Scans forward from `from`, without wrapping, and stores the offset of each
// match in `matches` (`limit` entries, at least 1). Every start position counts,
// so matches may overlap. A match lies wholly inside [0, size).
//
// Each byte is read at most once, and a call reads at most `budget` + m bytes.
// Unless the result is MEM_SEARCH_DONE, `next` is where to continue, and a scan
// resumed there finds exactly what one uninterrupted scan would have.
mem_search_stop_t mem_search_scan(mem_search_read_t read, void *ctx, uint32_t size,
                                  const uint8_t *pat, size_t m, uint32_t from,
                                  uint32_t budget, uint32_t *matches, size_t limit,
                                  size_t *found, uint32_t *next);

#ifdef __cplusplus
}
#endif

#endif
