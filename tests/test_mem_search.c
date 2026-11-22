// Checks for the memory search (src/mem_search.c).

#include "mem_search.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

#define MEM_SIZE 0x20000u

static uint8_t  mem[MEM_SIZE];
static uint8_t  hits[MEM_SIZE];
static uint32_t reads;

static uint8_t
read_mem(uint32_t offset, void *ctx)
{
	(void)ctx;
	reads++;
	if (hits[offset] < 255) {
		hits[offset]++;
	}
	return mem[offset];
}

static void
reset_counts(void)
{
	reads = 0;
	memset(hits, 0, sizeof hits);
}

static bool
parsed(const char *s, bool strict, const char *want, size_t want_len)
{
	uint8_t out[MEM_SEARCH_MAX_PATTERN];
	size_t  n;
	return mem_search_parse_hex(s, strict, out, &n) && n == want_len && !memcmp(out, want, n);
}

static bool
refused(const char *s, bool strict)
{
	uint8_t out[MEM_SEARCH_MAX_PATTERN];
	size_t  n;
	return !mem_search_parse_hex(s, strict, out, &n);
}

static void
test_parse(void)
{
	check(parsed("A9 00 8D", true, "\xA9\x00\x8D", 3), "hex with spaces");
	check(parsed("a9008d", true, "\xA9\x00\x8D", 3), "hex without spaces, lower case");
	check(parsed("  A9\t00 ", true, "\xA9\x00", 2), "whitespace around bytes");

	check(parsed("A9 0", false, "\xA9\x00", 2), "lenient: a trailing digit is a low nibble");
	check(parsed("A", false, "\x0A", 1), "lenient: one digit");
	check(parsed("A 9", false, "\xA9", 1), "lenient: space inside a byte");
	check(refused("A9 0", true), "strict: odd digits");
	check(refused("A", true), "strict: one digit");
	check(refused("A 9", true), "strict: space inside a byte");

	for (int strict = 0; strict < 2; strict++) {
		check(refused("", strict), "empty hex");
		check(refused("   ", strict), "blank hex");
		check(refused("XY", strict), "not hex");
		check(refused("A9 G0", strict), "not hex after a byte");
		check(refused("0xA9", strict), "a prefix is not hex");
	}

	char long_hex[2 * MEM_SEARCH_MAX_PATTERN + 3];
	memset(long_hex, 'A', sizeof long_hex - 1);
	long_hex[sizeof long_hex - 1] = 0;
	check(refused(long_hex, true), "one byte too many");
	long_hex[2 * MEM_SEARCH_MAX_PATTERN] = 0;
	check(!refused(long_hex, true), "the longest pattern");
	long_hex[2 * MEM_SEARCH_MAX_PATTERN]     = 'A';
	long_hex[2 * MEM_SEARCH_MAX_PATTERN + 1] = 0;
	check(refused(long_hex, false), "lenient: a nibble past the end");

	uint8_t out[MEM_SEARCH_MAX_PATTERN];
	size_t  n;
	check(mem_search_parse_text("READY", out, &n) && n == 5 && !memcmp(out, "READY", 5), "text");
	check(!mem_search_parse_text("", out, &n), "empty text");
	char long_text[MEM_SEARCH_MAX_PATTERN + 2];
	memset(long_text, 'x', sizeof long_text - 1);
	long_text[sizeof long_text - 1] = 0;
	check(!mem_search_parse_text(long_text, out, &n), "text too long");
	long_text[MEM_SEARCH_MAX_PATTERN] = 0;
	check(mem_search_parse_text(long_text, out, &n) && n == MEM_SEARCH_MAX_PATTERN, "longest text");
}

static void
test_find(void)
{
	const uint8_t pat[2] = {0x12, 0x34};
	memset(mem, 0, 64);
	mem[10] = 0x12, mem[11] = 0x34;
	mem[30] = 0x12, mem[31] = 0x34;

	check(mem_search_find(read_mem, NULL, 64, pat, 2, 0, 1) == 10, "find: first");
	check(mem_search_find(read_mem, NULL, 64, pat, 2, 11, 1) == 30, "find: next");
	check(mem_search_find(read_mem, NULL, 64, pat, 2, 31, 1) == 10, "find: wraps forward");
	check(mem_search_find(read_mem, NULL, 64, pat, 2, 29, -1) == 10, "find: previous");
	check(mem_search_find(read_mem, NULL, 64, pat, 2, 9, -1) == 30, "find: wraps backward");
	check(mem_search_find(read_mem, NULL, 64, pat, 2, 64, -1) == 30, "find: from the end");
	check(mem_search_find(read_mem, NULL, 64, pat, 2, -1, 1) == 10, "find: a negative start wraps");

	const uint8_t none[1] = {0x77};
	check(mem_search_find(read_mem, NULL, 64, none, 1, 0, 1) == -1, "find: no match");
	check(mem_search_find(read_mem, NULL, 1, pat, 2, 0, 1) == -1, "find: longer than the space");
	check(mem_search_find(read_mem, NULL, 64, pat, 0, 0, 1) == -1, "find: empty pattern");

	mem[62] = 0x12, mem[63] = 0x34;
	check(mem_search_find(read_mem, NULL, 64, pat, 2, 31, 1) == 62, "find: at the very end");
	mem[62] = 0, mem[63] = 0x12;
	check(mem_search_find(read_mem, NULL, 64, pat, 2, 31, 1) == 10, "find: never past the end");
}

static size_t
scan_all(uint32_t size, const uint8_t *pat, size_t m, uint32_t budget, size_t limit,
         uint32_t *out, size_t cap, int *calls)
{
	uint32_t from = 0;
	size_t   got  = 0;
	*calls        = 0;
	for (;;) {
		uint32_t page[1024];
		size_t   n;
		uint32_t next;
		const size_t ask = limit < 1024 ? limit : 1024;
		mem_search_stop_t st =
		    mem_search_scan(read_mem, NULL, size, pat, m, from, budget, page, ask, &n, &next);
		(*calls)++;
		for (size_t i = 0; i < n && got < cap; i++) {
			out[got++] = page[i];
		}
		if (st == MEM_SEARCH_DONE) {
			return got;
		}
		from = next;
	}
}

static void
test_scan(void)
{
	uint32_t found[1024];
	size_t   n;
	uint32_t next;

	memset(mem, 0, MEM_SIZE);
	mem[4] = mem[5] = mem[6] = mem[7] = 0xAA;
	const uint8_t aa[2]               = {0xAA, 0xAA};
	mem_search_stop_t st =
	    mem_search_scan(read_mem, NULL, 16, aa, 2, 0, 1000, found, 16, &n, &next);
	check(st == MEM_SEARCH_DONE && n == 3 && found[0] == 4 && found[1] == 5 && found[2] == 6,
	      "overlapping matches are all reported");

	st = mem_search_scan(read_mem, NULL, 16, aa, 2, 0, 1000, found, 2, &n, &next);
	check(st == MEM_SEARCH_LIMIT && n == 2 && next == 6, "the limit stops just past the last match");
	st = mem_search_scan(read_mem, NULL, 16, aa, 2, next, 1000, found, 2, &n, &next);
	check(st == MEM_SEARCH_DONE && n == 1 && found[0] == 6, "continuing keeps the overlapping match");

	memset(mem, 0, 32);
	mem[14] = mem[15] = 0xAA;
	st = mem_search_scan(read_mem, NULL, 16, aa, 2, 0, 1000, found, 1, &n, &next);
	check(st == MEM_SEARCH_DONE && n == 1 && found[0] == 14,
	      "a limit reached on the last candidate is done");

	memset(mem, 0, 32);
	mem[15] = 0xAA, mem[16] = 0xAA;
	st = mem_search_scan(read_mem, NULL, 16, aa, 2, 0, 1000, found, 4, &n, &next);
	check(st == MEM_SEARCH_DONE && n == 0, "a match may not run past the end");
	st = mem_search_scan(read_mem, NULL, 17, aa, 2, 0, 1000, found, 4, &n, &next);
	check(st == MEM_SEARCH_DONE && n == 1 && found[0] == 15, "and is found once it fits");

	memset(mem, 0, 32);
	mem[0] = 0xAA, mem[15] = 0xAA;
	st = mem_search_scan(read_mem, NULL, 16, aa, 2, 0, 1000, found, 4, &n, &next);
	check(st == MEM_SEARCH_DONE && n == 0, "no wrap from the end to the start");

	st = mem_search_scan(read_mem, NULL, 16, aa, 2, 15, 1000, found, 4, &n, &next);
	check(st == MEM_SEARCH_DONE && n == 0, "a start with no room is done");
	st = mem_search_scan(read_mem, NULL, 1, aa, 2, 0, 1000, found, 4, &n, &next);
	check(st == MEM_SEARCH_DONE && n == 0, "a space shorter than the pattern");

	// Either side of each internal read boundary.
	const uint8_t abc[3] = {0xA1, 0xB2, 0xC3};
	for (uint32_t at = 4090; at < 4100; at++) {
		memset(mem, 0, 8200);
		memcpy(mem + at, abc, 3);
		st = mem_search_scan(read_mem, NULL, 8200, abc, 3, 0, 100000, found, 4, &n, &next);
		check(st == MEM_SEARCH_DONE && n == 1 && found[0] == at, "a match across a read boundary");
	}

	memset(mem, 0, 0x8000);
	mem[0x5000] = 0xA1, mem[0x5001] = 0xB2, mem[0x5002] = 0xC3;
	reset_counts();
	st = mem_search_scan(read_mem, NULL, 0x8000, abc, 3, 0, 0x1000, found, 4, &n, &next);
	check(st == MEM_SEARCH_SPENT && n == 0 && next < 0x5000, "the budget stops a long scan");
	check(reads <= 0x1000 + 3, "and bounds the bytes read");

	// A pattern that matches nearly everywhere is the costly case.
	const uint8_t zeros[MEM_SEARCH_MAX_PATTERN] = {0};
	memset(mem, 0, 0x8000);
	reset_counts();
	st = mem_search_scan(read_mem, NULL, 0x8000, zeros, sizeof zeros, 0, 0x1000, found, 1024,
	                     &n, &next);
	check(reads <= 0x1000 + sizeof zeros, "the budget holds for a self-similar pattern");
	bool once = true;
	for (uint32_t i = 0; i < 0x8000; i++) {
		once = once && hits[i] <= 1;
	}
	check(once, "each byte is read at most once");

	mem[0] = 0xAA, mem[1] = 0xAA;
	st = mem_search_scan(read_mem, NULL, 0x8000, aa, 2, 0, 1, found, 4, &n, &next);
	check(st == MEM_SEARCH_SPENT && n == 1 && found[0] == 0 && next == 1,
	      "a budget below the pattern length still makes progress");

	// Paged by budget and by limit against one unbounded scan.
	srand(1234);
	for (uint32_t i = 0; i < 0x10000; i++) {
		mem[i] = (uint8_t)(rand() & 3);
	}
	const uint8_t pat[2] = {1, 1};
	static uint32_t whole[0x10000], paged[0x10000];
	int    calls;
	size_t w = scan_all(0x10000, pat, 2, 0xFFFFFFFFu, 1024, whole, 0x10000, &calls);
	check(w > 1024, "the workload has more matches than one page");

	const struct { uint32_t budget; size_t limit; } plans[] = {
		{0xFFFFFFFFu, 1}, {0xFFFFFFFFu, 7}, {1, 1024}, {3, 3}, {4095, 1024}, {4096, 1024},
		{4097, 13}, {0x4000, 64},
	};
	for (size_t p = 0; p < sizeof plans / sizeof plans[0]; p++) {
		size_t g = scan_all(0x10000, pat, 2, plans[p].budget, plans[p].limit, paged, 0x10000, &calls);
		check(g == w && !memcmp(whole, paged, w * sizeof whole[0]),
		      "pages add up to one scan, with no gap and no repeat");
	}
}

int
main(void)
{
	test_parse();
	test_find();
	test_scan();
	if (failures) {
		printf("%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
