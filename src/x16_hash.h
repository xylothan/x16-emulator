// Commander X16 Emulator
// Small seeded PRNG and hash, header-only so the modules that use them need no
// new link dependency (several are linked into tests against fakes).
// All rights reserved. License: 2-clause BSD

#ifndef X16_HASH_H
#define X16_HASH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// splitmix64: one 64-bit state, good spread, trivially seekable by stream.
static inline uint64_t
x16_splitmix64(uint64_t *state)
{
	uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

// Fill a buffer from (seed, stream). Each stream is independent, so RAM, VRAM
// and cartridge banks each get the same bytes for a seed whatever order they
// are filled in, and whatever else in the process calls rand().
static inline void
x16_fill_random(uint8_t *buf, size_t n, uint64_t seed, uint64_t stream)
{
	uint64_t state = seed ^ (stream * 0xD1B54A32D192ED03ULL);
	size_t i = 0;
	while (i < n) {
		uint64_t r = x16_splitmix64(&state);
		for (int b = 0; b < 8 && i < n; b++, i++) {
			buf[i] = (uint8_t)(r >> (8 * b));
		}
	}
}

// A fast 64-bit hash for comparing machine state between runs. Not
// cryptographic; it only has to make an accidental match vanishingly unlikely.
#define X16_HASH_INIT 0x84222325CBF29CE4ULL

static inline uint64_t
x16_hash_mix(uint64_t h, uint64_t w)
{
	h ^= w * 0x87C37B91114253D5ULL;
	h = (h << 31) | (h >> 33);
	return h * 0x4CF5AD432745937FULL + 0x52DCE729;
}

static inline uint64_t
x16_hash_bytes(uint64_t h, const void *data, size_t n)
{
	const uint8_t *p = (const uint8_t *)data;
	while (n >= 8) {
		uint64_t w;
		memcpy(&w, p, 8);
		h = x16_hash_mix(h, w);
		p += 8;
		n -= 8;
	}
	uint64_t tail = 0;
	for (size_t i = 0; i < n; i++) {
		tail |= (uint64_t)p[i] << (8 * i);
	}
	// The length goes in too, so a run of zeros of one length differs from another.
	return x16_hash_mix(x16_hash_mix(h, tail), (uint64_t)n);
}

static inline uint64_t
x16_hash_u64(uint64_t h, uint64_t v)
{
	return x16_hash_mix(h, v);
}

#endif
