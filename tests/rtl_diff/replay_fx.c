// Replays an op file through src/video.c; one state line per op, as tb_fx.v.
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "video.h"

static uint8_t pattern(uint32_t a) { return (uint8_t)((a * 37) ^ (a >> 5) ^ (a >> 11)); }

static void reinit(void)
{
	video_reset();
	for (uint32_t a = 0; a < 0x20000; a++)
		video_space_write(a, pattern(a));
}

int main(int argc, char **argv)
{
	if (argc != 3) { fprintf(stderr, "usage: emu_fx ops out\n"); return 2; }
	FILE *in = fopen(argv[1], "r"), *out = fopen(argv[2], "w");
	if (!in || !out) { perror("open"); return 2; }
	static char buf[1 << 20];
	setvbuf(out, buf, _IOFBF, sizeof buf);

	reinit();
	int kind; unsigned reg, val;
	while (fscanf(in, "%d %x %x\n", &kind, &reg, &val) == 3) {
		if (kind == 2) { reinit(); fprintf(out, "S\n"); continue; }
		uint32_t pre = (reg == 3 || reg == 4) ? video_get_address(reg - 3) : 0;
		uint8_t rd = 0;
		if (kind == 0) rd = video_read((uint8_t)reg, false);
		else           video_write((uint8_t)reg, (uint8_t)val);

		uint32_t word = 0;
		if (kind == 1 && (reg == 3 || reg == 4)) {
			uint32_t w = pre & 0x1fffc;
			word = video_space_read(w) | video_space_read(w + 1) << 8 |
			       (uint32_t)video_space_read(w + 2) << 16 | (uint32_t)video_space_read(w + 3) << 24;
		}
		struct video_state s;
		video_get_state(&s);
		uint32_t cache = s.fx_cache[0] | s.fx_cache[1] << 8 |
		                 (uint32_t)s.fx_cache[2] << 16 | (uint32_t)s.fx_cache[3] << 24;
		fprintf(out, "%02x %05x %d %05x %d %02x %02x %05x %05x %08x %08x %d %08x\n",
			rd, s.port[0].address, s.port[0].nibble, s.port[1].address, s.port[1].nibble,
			s.port[0].prefetch, s.port[1].prefetch,
			(s.fx_x_position >> 7) & 0xfffff, (s.fx_y_position >> 7) & 0xfffff,
			cache, (uint32_t)s.fx_accumulator, s.fx_2bit_poking, word);
	}
	fclose(out);
	return 0;
}
