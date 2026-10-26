// Commander X16 Emulator — little-endian reading and writing for the saved
// profile run (profile_run.c, waterfall.c). Fields are written one at a time,
// never as raw structs, so padding and bool sizes never reach the file.
#ifndef BYTES_IO_H
#define BYTES_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline void
bio_w8(FILE *f, uint8_t v)
{
	fputc(v, f);
}

static inline void
bio_w16(FILE *f, uint16_t v)
{
	const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	fwrite(b, 1, 2, f);
}

static inline void
bio_w32(FILE *f, uint32_t v)
{
	const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	fwrite(b, 1, 4, f);
}

static inline void
bio_w64(FILE *f, uint64_t v)
{
	bio_w32(f, (uint32_t)v);
	bio_w32(f, (uint32_t)(v >> 32));
}

// A string as a 16-bit length and its bytes (no terminator).
static inline void
bio_wstr(FILE *f, const char *s)
{
	size_t n = s ? strlen(s) : 0;
	if (n > 0xFFFF)
		n = 0xFFFF;
	bio_w16(f, (uint16_t)n);
	if (n)
		fwrite(s, 1, n, f);
}

// Reading from a buffer that is known to hold the whole section. Any read past
// its end marks the reader bad and returns zeros, so a caller checks once at
// the end rather than after every field.
typedef struct {
	const uint8_t *p, *end;
	bool           bad;
} bio_reader_t;

static inline bool
bio_need(bio_reader_t *r, size_t n)
{
	if (r->bad || (size_t)(r->end - r->p) < n) {
		r->bad = true;
		return false;
	}
	return true;
}

static inline uint8_t
bio_r8(bio_reader_t *r)
{
	if (!bio_need(r, 1))
		return 0;
	return *r->p++;
}

static inline uint16_t
bio_r16(bio_reader_t *r)
{
	if (!bio_need(r, 2))
		return 0;
	const uint16_t v = (uint16_t)(r->p[0] | (r->p[1] << 8));
	r->p += 2;
	return v;
}

static inline uint32_t
bio_r32(bio_reader_t *r)
{
	if (!bio_need(r, 4))
		return 0;
	const uint32_t v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) | ((uint32_t)r->p[2] << 16) |
	                   ((uint32_t)r->p[3] << 24);
	r->p += 4;
	return v;
}

static inline uint64_t
bio_r64(bio_reader_t *r)
{
	const uint64_t lo = bio_r32(r);
	return lo | ((uint64_t)bio_r32(r) << 32);
}

// A string written by bio_wstr, into `out` (cut to fit, always terminated).
static inline void
bio_rstr(bio_reader_t *r, char *out, size_t size)
{
	const size_t n = bio_r16(r);
	if (!bio_need(r, n)) {
		if (size)
			out[0] = 0;
		return;
	}
	const size_t k = n < size ? n : (size ? size - 1 : 0);
	if (size) {
		memcpy(out, r->p, k);
		out[k] = 0;
	}
	r->p += n;
}

// Whether `count` items of at least `each` bytes could still follow: checked
// before allocating for them, so a corrupt count cannot ask for gigabytes.
static inline bool
bio_room_for(bio_reader_t *r, uint64_t count, size_t each)
{
	if (r->bad)
		return false;
	if (each && count > (uint64_t)(r->end - r->p) / each) {
		r->bad = true;
		return false;
	}
	return true;
}

#endif
