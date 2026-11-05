// Commander X16 Emulator
// Save states: the chunk writer and reader every module serializes through.
//
// Header-only on purpose. Each module's xxx_state_save()/xxx_state_load() lives
// in that module's own file, and several of those files are linked into unit
// tests on their own (memory.c against tests/support/fake_devices.c, video.c,
// the CPU core). Inline functions add no link dependency, so a module gaining a
// serializer does not change what its tests need.
//
// A state is a sequence of chunks, each one module's data:
//
//     tag[4]  version (u16)  reserved (u16, 0)  length (u32)  bytes[length]
//
// Values are stored in the host's own byte order and layout: a state belongs to
// the build that wrote it, which the header checks (savestate.c).
//
// A writer can also just count. Run the savers through a counting writer and
// the result is the chunk list a state from this machine must have, which is how
// a load is checked in full before anything is overwritten.

#ifndef SAVESTATE_IO_H
#define SAVESTATE_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SS_CHUNK_HEADER 12

#define SS_LAYOUT_MAX 32

typedef struct {
	char     tag[4];
	uint16_t version;
	uint32_t length;
} ss_chunk_info;

typedef struct {
	int           count;
	ss_chunk_info chunks[SS_LAYOUT_MAX];
} ss_layout;

typedef struct {
	uint8_t   *data;        // NULL while counting
	size_t     len;
	size_t     cap;
	bool       counting;    // measure only: nothing is stored
	bool       failed;      // out of memory, or chunks misnested
	size_t     chunk_start; // offset of the open chunk's header, or SIZE_MAX
	ss_layout *layout;      // when set, every chunk is recorded here
} ss_writer;

typedef struct {
	const uint8_t *data;
	size_t         len;
	size_t         pos;
	size_t         chunk_end;
	bool           failed;  // a read ran past its chunk, or a chunk was missing
} ss_reader;

// ─── Writer ──────────────────────────────────────────────────────────────────

static inline void
ss_writer_init(ss_writer *w, bool counting, ss_layout *layout)
{
	w->data        = NULL;
	w->len         = 0;
	w->cap         = 0;
	w->counting    = counting;
	w->failed      = false;
	w->chunk_start = SIZE_MAX;
	w->layout      = layout;
	if (layout) {
		layout->count = 0;
	}
}

// Keep the buffer, forget the contents: a writer reused for every hash does
// not reallocate each time.
static inline void
ss_writer_rewind(ss_writer *w)
{
	w->len         = 0;
	w->failed      = false;
	w->chunk_start = SIZE_MAX;
	if (w->layout) {
		w->layout->count = 0;
	}
}

static inline void
ss_writer_free(ss_writer *w)
{
	free(w->data);
	w->data = NULL;
	w->len = w->cap = 0;
}

static inline bool
ss_reserve(ss_writer *w, size_t n)
{
	if (w->counting) {
		return true;
	}
	if (w->len + n <= w->cap) {
		return true;
	}
	size_t cap = w->cap ? w->cap : 4096;
	while (cap < w->len + n) {
		cap *= 2;
	}
	uint8_t *p = (uint8_t *)realloc(w->data, cap);
	if (!p) {
		w->failed = true;
		return false;
	}
	w->data = p;
	w->cap  = cap;
	return true;
}

static inline void
ss_put(ss_writer *w, const void *src, size_t n)
{
	if (w->failed || !ss_reserve(w, n)) {
		return;
	}
	if (!w->counting) {
		memcpy(w->data + w->len, src, n);
	}
	w->len += n;
}

static inline void ss_put_u8(ss_writer *w, uint8_t v)   { ss_put(w, &v, 1); }
static inline void ss_put_u16(ss_writer *w, uint16_t v) { ss_put(w, &v, 2); }
static inline void ss_put_u32(ss_writer *w, uint32_t v) { ss_put(w, &v, 4); }
static inline void ss_put_u64(ss_writer *w, uint64_t v) { ss_put(w, &v, 8); }
static inline void ss_put_bool(ss_writer *w, bool v)    { ss_put_u8(w, v ? 1 : 0); }
// Floats travel as their bits, so a restored value is the identical value.
static inline void ss_put_f32(ss_writer *w, float v)    { ss_put(w, &v, 4); }

static inline void
ss_begin(ss_writer *w, const char tag[4], uint16_t version)
{
	if (w->chunk_start != SIZE_MAX) {
		w->failed = true;  // chunks do not nest
		return;
	}
	w->chunk_start = w->len;
	uint16_t reserved = 0;
	uint32_t length   = 0;  // patched by ss_end()
	ss_put(w, tag, 4);
	ss_put_u16(w, version);
	ss_put_u16(w, reserved);
	ss_put_u32(w, length);
	if (w->layout && !w->failed) {
		if (w->layout->count >= SS_LAYOUT_MAX) {
			w->failed = true;
			return;
		}
		ss_chunk_info *c = &w->layout->chunks[w->layout->count];
		memcpy(c->tag, tag, 4);
		c->version = version;
		c->length  = 0;
	}
}

static inline void
ss_end(ss_writer *w)
{
	if (w->chunk_start == SIZE_MAX) {
		w->failed = true;
		return;
	}
	const uint32_t length = (uint32_t)(w->len - w->chunk_start - SS_CHUNK_HEADER);
	if (!w->failed && !w->counting) {
		memcpy(w->data + w->chunk_start + 8, &length, 4);
	}
	if (w->layout && !w->failed) {
		w->layout->chunks[w->layout->count++].length = length;
	}
	w->chunk_start = SIZE_MAX;
}

// ─── Reader ──────────────────────────────────────────────────────────────────

static inline void
ss_reader_init(ss_reader *r, const void *data, size_t len)
{
	r->data      = (const uint8_t *)data;
	r->len       = len;
	r->pos       = 0;
	r->chunk_end = 0;
	r->failed    = false;
}

// Reads past the open chunk fail and yield zeros, so a loader never reads
// outside the state and never needs to check each field.
static inline void
ss_get(ss_reader *r, void *dst, size_t n)
{
	if (r->failed || r->pos + n > r->chunk_end) {
		r->failed = true;
		memset(dst, 0, n);
		return;
	}
	memcpy(dst, r->data + r->pos, n);
	r->pos += n;
}

static inline uint8_t  ss_get_u8(ss_reader *r)  { uint8_t v;  ss_get(r, &v, 1); return v; }
static inline uint16_t ss_get_u16(ss_reader *r) { uint16_t v; ss_get(r, &v, 2); return v; }
static inline uint32_t ss_get_u32(ss_reader *r) { uint32_t v; ss_get(r, &v, 4); return v; }
static inline uint64_t ss_get_u64(ss_reader *r) { uint64_t v; ss_get(r, &v, 8); return v; }
static inline bool     ss_get_bool(ss_reader *r) { return ss_get_u8(r) != 0; }
static inline float    ss_get_f32(ss_reader *r) { float v;    ss_get(r, &v, 4); return v; }

// Read the next chunk's header without entering it. False at the end of the
// data, or when what is left is too short to be a chunk.
static inline bool
ss_peek_chunk(const ss_reader *r, ss_chunk_info *out)
{
	if (r->pos + SS_CHUNK_HEADER > r->len) {
		return false;
	}
	const uint8_t *h = r->data + r->pos;
	memcpy(out->tag, h, 4);
	memcpy(&out->version, h + 4, 2);
	memcpy(&out->length, h + 8, 4);
	return out->length <= r->len - r->pos - SS_CHUNK_HEADER;
}

// Enter the next chunk, which must carry this tag. Returns its version, or -1
// (and marks the reader failed) when the next chunk is anything else.
static inline int
ss_open(ss_reader *r, const char tag[4])
{
	ss_chunk_info c;
	if (r->failed || !ss_peek_chunk(r, &c) || memcmp(c.tag, tag, 4) != 0) {
		r->failed = true;
		return -1;
	}
	r->pos      += SS_CHUNK_HEADER;
	r->chunk_end = r->pos + c.length;
	return c.version;
}

// Leave the chunk. A loader that read less than the chunk holds is as wrong as
// one that read more, so both fail.
static inline void
ss_close(ss_reader *r)
{
	if (r->pos != r->chunk_end) {
		r->failed = true;
	}
	r->pos = r->chunk_end;
}

// Walk a whole state into a layout. False when it is not a well-formed chunk
// sequence, or has more chunks than a layout can hold.
static inline bool
ss_read_layout(const void *data, size_t len, ss_layout *out)
{
	ss_reader r;
	ss_reader_init(&r, data, len);
	out->count = 0;
	while (r.pos < r.len) {
		ss_chunk_info c;
		if (!ss_peek_chunk(&r, &c) || out->count >= SS_LAYOUT_MAX) {
			return false;
		}
		out->chunks[out->count++] = c;
		r.pos += SS_CHUNK_HEADER + c.length;
	}
	return true;
}

// The first chunk where two layouts disagree, or -1 when they match.
static inline int
ss_layout_diff(const ss_layout *a, const ss_layout *b)
{
	const int n = a->count < b->count ? a->count : b->count;
	for (int i = 0; i < n; i++) {
		const ss_chunk_info *x = &a->chunks[i], *y = &b->chunks[i];
		if (memcmp(x->tag, y->tag, 4) != 0 || x->version != y->version || x->length != y->length) {
			return i;
		}
	}
	return a->count == b->count ? -1 : n;
}

#endif
