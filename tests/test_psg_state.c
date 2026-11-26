// PSG save-state chunk (src/vera_psg.c): version 2 and the older, shorter
// version 1, which has no DAC sum.
#include "vera_psg.h"
#include "savestate.h"

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

static void
put_le(uint8_t *p, uint32_t v, int n)
{
	for (int i = 0; i < n; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint32_t
get_le(const uint8_t *p, int n)
{
	uint32_t v = 0;
	for (int i = 0; i < n; i++) {
		v |= (uint32_t)p[i] << (8 * i);
	}
	return v;
}

// v2 rewritten as v1: version 1, the 4 DAC bytes at the end dropped.
static size_t
as_older(const ss_writer *v2, uint8_t *out, size_t drop, uint16_t version)
{
	const size_t len = v2->len - drop;
	memcpy(out, v2->data, len);
	put_le(out + 4, version, 2);
	put_le(out + 8, get_le(v2->data + 8, 4) - (uint32_t)drop, 4);
	return len;
}

static bool
load(const uint8_t *data, size_t len)
{
	ss_reader r;
	ss_reader_init(&r, data, len);
	psg_state_load(&r);
	return !r.failed && r.pos == len;
}

int
main(void)
{
	psg_reset();
	for (int ch = 0; ch < 3; ch++) {
		psg_writereg((uint8_t)(ch * 4 + 0), (uint8_t)(0x34 + ch));
		psg_writereg((uint8_t)(ch * 4 + 1), 0x12);
		psg_writereg((uint8_t)(ch * 4 + 2), 0xFF);
		psg_writereg((uint8_t)(ch * 4 + 3), (uint8_t)(0x20 + ch * 0x40));
	}
	int16_t buf[2 * 256];
	psg_render(buf, 256);

	ss_writer v2;
	ss_writer_init(&v2, false, NULL);
	psg_state_save(&v2);
	check(!v2.failed && get_le(v2.data + 4, 2) == 2, "the PSG saves as chunk version 2");
	const uint8_t *dac = v2.data + v2.len - 4;
	check(get_le(dac, 4) != 0, "the saved DAC sum is not zero");

	uint8_t v1[4096];
	check(v2.len <= sizeof v1, "the chunk fits the buffer");
	const size_t v1_len = as_older(&v2, v1, 4, 1);

	psg_reset();
	check(load(v1, v1_len), "a version 1 chunk, 4 bytes shorter, loads and closes cleanly");
	ss_writer again;
	ss_writer_init(&again, false, NULL);
	psg_state_save(&again);
	check(!again.failed && again.len == v2.len
	      && memcmp(again.data + SS_CHUNK_HEADER, v2.data + SS_CHUNK_HEADER, v2.len - SS_CHUNK_HEADER - 4) == 0,
	      "version 1 restores every channel, register and the noise state");
	check(get_le(again.data + again.len - 4, 4) == 0, "version 1 leaves the DAC sum at zero");

	psg_reset();
	check(load(v2.data, v2.len), "the version 2 chunk loads");
	ss_writer_rewind(&again);
	psg_state_save(&again);
	check(again.len == v2.len && memcmp(again.data, v2.data, v2.len) == 0, "version 2 round-trips byte for byte");

	uint8_t bad[4096];
	size_t  bad_len = as_older(&v2, bad, 2, 1);
	check(!load(bad, bad_len), "a version 1 chunk of the wrong length fails");
	bad_len = as_older(&v2, bad, 4, 2);
	check(!load(bad, bad_len), "a version 2 chunk 4 bytes short fails");

	ss_writer_free(&v2);
	ss_writer_free(&again);
	printf("%s\n", g_fails ? "SOME CHECKS FAILED" : "all checks passed");
	return g_fails ? 1 : 0;
}
