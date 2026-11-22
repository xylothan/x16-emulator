// Commander X16 Emulator — memory search.

#include "mem_search.h"

#include <ctype.h>
#include <string.h>

// Large enough that the m - 1 bytes carried between reads are a small part of it.
#define SCAN_CHUNK 4096

bool
mem_search_parse_hex(const char *s, bool strict, uint8_t *out, size_t *len)
{
	size_t n  = 0;
	int    hi = -1;
	*len      = 0;
	for (const char *p = s; *p; ++p) {
		const unsigned char c = (unsigned char)*p;
		if (isspace(c)) {
			if (strict && hi >= 0) {
				return false;
			}
			continue;
		}
		if (!isxdigit(c)) {
			return false;
		}
		const int v = (c <= '9') ? (c - '0') : (tolower(c) - 'a' + 10);
		if (hi < 0) {
			hi = v;
		} else {
			if (n == MEM_SEARCH_MAX_PATTERN) {
				return false;
			}
			out[n++] = (uint8_t)((hi << 4) | v);
			hi       = -1;
		}
	}
	if (hi >= 0) {
		if (strict || n == MEM_SEARCH_MAX_PATTERN) {
			return false;
		}
		out[n++] = (uint8_t)hi;
	}
	*len = n;
	return n > 0;
}

bool
mem_search_parse_text(const char *s, uint8_t *out, size_t *len)
{
	const size_t n = strlen(s);
	*len           = 0;
	if (n == 0 || n > MEM_SEARCH_MAX_PATTERN) {
		return false;
	}
	memcpy(out, s, n);
	*len = n;
	return true;
}

long
mem_search_find(mem_search_read_t read, void *ctx, size_t size,
                const uint8_t *pat, size_t m, long from, int dir)
{
	if (m == 0 || size < m) {
		return -1;
	}
	const long span = (long)(size - m) + 1;
	from            = ((from % span) + span) % span;
	for (long k = 0; k < span; k++) {
		const long i = (dir >= 0) ? ((from + k) % span) : (((from - k) % span + span) % span);
		bool       ok = true;
		for (size_t j = 0; j < m; j++) {
			if (read((uint32_t)i + (uint32_t)j, ctx) != pat[j]) {
				ok = false;
				break;
			}
		}
		if (ok) {
			return i;
		}
	}
	return -1;
}

mem_search_stop_t
mem_search_scan(mem_search_read_t read, void *ctx, uint32_t size,
                const uint8_t *pat, size_t m, uint32_t from,
                uint32_t budget, uint32_t *matches, size_t limit,
                size_t *found, uint32_t *next)
{
	uint8_t  buf[SCAN_CHUNK];
	uint32_t base = from;
	uint32_t used = 0;
	size_t   have = 0;

	*found = 0;
	*next  = size;
	if (m == 0 || m > MEM_SEARCH_MAX_PATTERN || limit == 0 || size < m || from > size - m) {
		return MEM_SEARCH_DONE;
	}
	const uint32_t last = size - (uint32_t)m;

	for (;;) {
		const size_t   room      = sizeof buf - have;
		const uint32_t left      = size - (base + (uint32_t)have);
		const uint32_t allowance = budget > used ? budget - used : 0;
		size_t         want      = room < left ? room : left;
		if (want > allowance) {
			want = allowance;
		}
		// One whole candidate, so that even a tiny budget makes progress.
		if (have + want < m) {
			want = m - have;
		}
		for (size_t i = 0; i < want; i++) {
			buf[have + i] = read(base + (uint32_t)(have + i), ctx);
		}
		have += want;
		used += (uint32_t)want;

		const size_t candidates = have - m + 1;
		for (size_t i = 0; i < candidates; i++) {
			if (memcmp(buf + i, pat, m) != 0) {
				continue;
			}
			matches[(*found)++] = base + (uint32_t)i;
			if (*found == limit) {
				const uint32_t after = base + (uint32_t)i + 1;
				if (after > last) {
					return MEM_SEARCH_DONE;
				}
				*next = after;
				return MEM_SEARCH_LIMIT;
			}
		}

		const uint32_t pos = base + (uint32_t)candidates;
		if (pos > last) {
			return MEM_SEARCH_DONE;
		}
		if (used >= budget) {
			*next = pos;
			return MEM_SEARCH_SPENT;
		}
		memmove(buf, buf + candidates, m - 1);
		base = pos;
		have = m - 1;
	}
}
