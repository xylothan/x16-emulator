// Checks for the save-state chunk format (src/savestate_io.h).
//
// Standalone: the chunk writer and reader are header-only and need nothing from
// the emulator, SDL or a ROM. Returns 0 when every check passes.
//
// Build: configure with -DBUILD_SAVESTATE_TEST=ON, then build target
// savestate_test.
#include "savestate_io.h"

#include <stdio.h>
#include <string.h>

static int g_fails = 0;

static void
check(int cond, const char *msg)
{
	printf("%s: %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond) {
		g_fails++;
	}
}

// Two chunks, the way a module's saver would write them.
static void
write_sample(ss_writer *w, uint32_t a, const uint8_t *blob, size_t blob_len)
{
	ss_begin(w, "AAAA", 1);
	ss_put_u32(w, a);
	ss_put_u16(w, 0x1234);
	ss_put_bool(w, true);
	ss_put_f32(w, -0.5f);
	ss_end(w);
	ss_begin(w, "BBBB", 7);
	ss_put(w, blob, blob_len);
	ss_end(w);
}

int
main(void)
{
	uint8_t blob[300];
	for (int i = 0; i < (int)sizeof blob; i++) {
		blob[i] = (uint8_t)(i * 7 + 3);
	}

	// ── Round trip ──
	ss_writer w;
	ss_writer_init(&w, false, NULL);
	write_sample(&w, 0xDEADBEEF, blob, sizeof blob);
	check(!w.failed, "writing two chunks succeeds");
	check(w.len == 2 * SS_CHUNK_HEADER + 4 + 2 + 1 + 4 + sizeof blob,
	      "the state is the two headers plus exactly the bytes put");

	ss_reader r;
	ss_reader_init(&r, w.data, w.len);
	check(ss_open(&r, "AAAA") == 1, "the first chunk opens with its version");
	check(ss_get_u32(&r) == 0xDEADBEEF, "a u32 reads back");
	check(ss_get_u16(&r) == 0x1234, "a u16 reads back");
	check(ss_get_bool(&r) == true, "a bool reads back");
	const float f = ss_get_f32(&r);
	check(memcmp(&f, &(float){ -0.5f }, 4) == 0, "a float reads back bit for bit");
	ss_close(&r);
	check(!r.failed, "reading exactly what the chunk holds closes cleanly");
	check(ss_open(&r, "BBBB") == 7, "the second chunk opens with its version");
	uint8_t back[sizeof blob];
	ss_get(&r, back, sizeof back);
	ss_close(&r);
	check(!r.failed && memcmp(back, blob, sizeof blob) == 0, "a block reads back");
	check(r.pos == w.len, "the reader ends at the end of the state");

	// ── A loader that disagrees with its saver fails ──
	ss_reader_init(&r, w.data, w.len);
	ss_open(&r, "AAAA");
	(void)ss_get_u32(&r);
	ss_close(&r);
	check(r.failed, "reading less than a chunk holds fails at close");

	ss_reader_init(&r, w.data, w.len);
	ss_open(&r, "AAAA");
	uint8_t too_much[64];
	memset(too_much, 0xAA, sizeof too_much);
	ss_get(&r, too_much, sizeof too_much);
	int zeros = 1;
	for (size_t i = 0; i < sizeof too_much; i++) zeros &= too_much[i] == 0;
	check(r.failed && zeros, "reading past a chunk fails and yields zeros, not the next chunk");

	ss_reader_init(&r, w.data, w.len);
	check(ss_open(&r, "BBBB") < 0 && r.failed, "opening a chunk under the wrong tag fails");

	ss_reader_init(&r, w.data, w.len - 1);
	ss_open(&r, "AAAA");
	ss_close(&r);
	check(ss_open(&r, "BBBB") < 0, "a truncated state's last chunk does not open");

	// ── Layouts: what a check before loading compares ──
	ss_layout counted, written, read;
	ss_writer c;
	ss_writer_init(&c, true, &counted);
	write_sample(&c, 1, blob, sizeof blob);
	check(!c.failed && c.data == NULL && c.len == w.len,
	      "a counting writer stores nothing but measures the same length");

	ss_writer w2;
	ss_writer_init(&w2, false, &written);
	write_sample(&w2, 2, blob, sizeof blob);
	check(ss_read_layout(w.data, w.len, &read), "a state's layout can be read back");
	check(ss_layout_diff(&counted, &read) == -1 && ss_layout_diff(&written, &read) == -1,
	      "counted, recorded and read layouts all agree");
	check(read.count == 2 && memcmp(read.chunks[1].tag, "BBBB", 4) == 0
	      && read.chunks[1].version == 7 && read.chunks[1].length == sizeof blob,
	      "a layout names each chunk's tag, version and length");

	ss_layout shorter;
	ss_writer w3;
	ss_writer_init(&w3, false, &shorter);
	write_sample(&w3, 3, blob, sizeof blob - 1);
	check(ss_layout_diff(&counted, &shorter) == 1, "a chunk of a different length is found by index");
	ss_layout fewer = counted;
	fewer.count = 1;
	check(ss_layout_diff(&counted, &fewer) == 1, "a missing chunk is found");
	ss_layout newer = counted;
	newer.chunks[0].version = 2;
	check(ss_layout_diff(&counted, &newer) == 0, "a chunk of another version is found");

	uint8_t junk[20] = { 'X', 'X', 'X', 'X', 1, 0, 0, 0, 0xFF, 0xFF, 0, 0 };
	ss_layout bad;
	check(!ss_read_layout(junk, sizeof junk, &bad), "a chunk claiming more bytes than there are is refused");

	// ── Misuse is caught rather than written ──
	ss_writer m;
	ss_writer_init(&m, false, NULL);
	ss_begin(&m, "AAAA", 1);
	ss_begin(&m, "BBBB", 1);
	check(m.failed, "chunks do not nest");
	ss_writer_free(&m);
	ss_writer_init(&m, false, NULL);
	ss_end(&m);
	check(m.failed, "ending a chunk that was never begun fails");
	ss_writer_free(&m);

	// ── Reuse: rewinding keeps the buffer ──
	uint8_t *before = w.data;
	ss_writer_rewind(&w);
	write_sample(&w, 0xDEADBEEF, blob, sizeof blob);
	check(w.data == before && !w.failed, "a rewound writer reuses its buffer");

	ss_writer_free(&w);
	ss_writer_free(&w2);
	ss_writer_free(&w3);

	printf("\n%s (%d failure%s)\n", g_fails ? "FAILED" : "PASSED", g_fails, g_fails == 1 ? "" : "s");
	return g_fails ? 1 : 0;
}
