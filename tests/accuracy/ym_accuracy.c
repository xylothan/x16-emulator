// YM2151 bus vectors: tests/accuracy/README.md.
//
//     ym_accuracy [DIR]
//
// T counts interface cycles (two phiM clocks, 32 per sample) since reset. The
// chip is driven as audio.c drives it: whole samples, then YM_sync() to the
// cycle within the next one.

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ymglue.h"

#ifndef YM_VECTOR_DIR
#define YM_VECTOR_DIR "."
#endif

static uint64_t samples;
static uint64_t now;

#define MAX_SAMPLES 65536
static int16_t out[2][MAX_SAMPLES];

static void
run_to(uint64_t t)
{
	static int16_t buf[2 * 64], host[2 * 64];
	while (samples < t / 32) {
		const uint64_t left = t / 32 - samples;
		const uint32_t n = left > 64 ? 64 : (uint32_t)left;
		YM_stream_update((uint16_t *)buf, (uint16_t *)host, n);
		for (uint32_t k = 0; k < n && samples + k < MAX_SAMPLES; k++) {
			out[0][samples + k] = buf[2 * k];
			out[1][samples + k] = buf[2 * k + 1];
		}
		samples += n;
	}
	YM_sync((uint8_t)(t % 32));
	now = t;
}

// CRC-32 over values of 4 bytes each, LSB first, as tb_opm prints it.
static uint32_t
crc(uint32_t c, uint32_t v)
{
	static uint32_t table[256];
	if (table[1] == 0) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t k = i;
			for (int j = 0; j < 8; j++) {
				k = (k & 1) ? 0xedb88320u ^ (k >> 1) : k >> 1;
			}
			table[i] = k;
		}
	}
	c = ~c;
	for (int i = 0; i < 4; i++, v >>= 8) {
		c = table[(c ^ v) & 0xff] ^ (c >> 8);
	}
	return ~c;
}

static uint32_t
phase_step(unsigned kc, unsigned kf, unsigned dt1, unsigned dt2, unsigned mul, unsigned pms, unsigned pm)
{
	const struct ym_debug_phase in = { (uint8_t)kc, (uint8_t)kf, (uint8_t)dt1, (uint8_t)dt2, (uint8_t)mul, (uint8_t)pms, (uint8_t)pm };
	return YM_debug_phase_step(&in) & 0xfffff;
}

// F: static phase steps over KF, DT1, DT2 and MUL.
static uint32_t
crc_static(unsigned kc)
{
	static const unsigned muls[4] = { 0, 1, 7, 15 };
	uint32_t c = 0;
	for (unsigned kf = 0; kf < 64; kf++)
		for (unsigned dt1 = 0; dt1 < 8; dt1++)
			for (unsigned dt2 = 0; dt2 < 4; dt2++)
				for (unsigned m = 0; m < 4; m++)
					c = crc(c, phase_step(kc, kf, dt1, dt2, muls[m], 0, 0));
	return c;
}

// M: phase steps under every PM value.
static uint32_t
crc_pm(unsigned pms, unsigned kc)
{
	static const unsigned kfs[3] = { 0, 33, 63 }, dt1s[2] = { 0, 6 };
	uint32_t c = 0;
	for (unsigned pm = 0; pm < 256; pm++)
		for (unsigned k = 0; k < 3; k++)
			for (unsigned dt2 = 0; dt2 < 4; dt2++)
				for (unsigned d = 0; d < 2; d++)
					c = crc(c, phase_step(kc, kfs[k], dt1s[d], dt2, 1, pms, pm));
	return c;
}

struct watch {
	bool     open, known;
	uint64_t end;
	unsigned s, i;
};

static int
check_now(const struct watch *w, const char *name, int line)
{
	const unsigned s = YM_read_status(), i = YM_irq();
	if (w->known && (s != w->s || i != w->i)) {
		printf("%s:%d: at %" PRIu64 " status %02x irq %u, expected %02x %u\n", name, line, now, s, i, w->s, w->i);
		return 1;
	}
	return 0;
}

// Dense: every cycle before t is checked against the watch. Sparse: the chip
// jumps to t in batches of up to 64 samples, so expiries must not depend on
// how the samples are grouped.
static bool dense;

static int
advance(struct watch *w, uint64_t t, const char *name, int line)
{
	while (dense && w->open && now + 1 < t && now + 1 <= w->end) {
		run_to(now + 1);
		if (check_now(w, name, line)) {
			return 1;
		}
	}
	if (w->open && t > w->end) {
		w->open = false;
	}
	run_to(t);
	return 0;
}

static int
run_file(const char *dir, const char *name)
{
	char path[1024], line[256];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		printf("%s: cannot open\n", path);
		return 1;
	}
	YM_reset();
	samples = 0;
	now = 0;
	struct watch w = { 0 };
	int n = 0, bad = 0;
	while (!bad && fgets(line, sizeof line, f)) {
		unsigned p, v, m, a, s, i, c;
		uint64_t t, t1;
		char side;
		n++;
		if (sscanf(line, "W %u %x %" SCNu64, &p, &v, &t) == 3) {
			bad = advance(&w, t, name, n);
			YM_write((uint8_t)p, (uint8_t)v);
		} else if (sscanf(line, "R %x %" SCNu64, &v, &t) == 2) {
			bad = advance(&w, t, name, n);
			if (!bad && YM_read_status() != v) {
				printf("%s:%d: at %" PRIu64 " status %02x, expected %02x\n", name, n, t, YM_read_status(), v);
				bad = 1;
			}
		} else if (sscanf(line, "G %x %x %x %" SCNu64, &a, &v, &m, &t) == 4) {
			bad = advance(&w, t, name, n);
			if (!bad && (YM_debug_get_reg((uint8_t)a) & m) != v) {
				printf("%s:%d: at %" PRIu64 " register %02x holds %02x, expected %02x\n",
				       name, n, t, YM_debug_get_reg((uint8_t)a) & m, v, a);
				bad = 1;
			}
		} else if (sscanf(line, "X %" SCNu64 " %" SCNu64, &t, &t1) == 2) {
			bad = advance(&w, t, name, n);
			w.open = true;
			w.known = false;
			w.end = t1;
		} else if (sscanf(line, "C %x %u %" SCNu64, &s, &i, &t) == 3) {
			bad = advance(&w, t, name, n);
			w.known = true;
			w.s = s;
			w.i = i;
			bad = bad || check_now(&w, name, n);
		} else if (sscanf(line, "F %x %x", &a, &c) == 2) {
			if (crc_static(a) != c) {
				printf("%s:%d: phase steps at KC %02x: crc %08x, expected %08x\n", name, n, a, crc_static(a), c);
				bad = 1;
			}
		} else if (sscanf(line, "M %u %x %x", &p, &a, &c) == 3) {
			if (crc_pm(p, a) != c) {
				printf("%s:%d: PM phase steps at PMS %u KC %02x: crc %08x, expected %08x\n", name, n, p, a, crc_pm(p, a), c);
				bad = 1;
			}
		} else if (sscanf(line, "O %" SCNu64 " %c %x", &t, &side, &c) == 3) {
			if (t + 64 > MAX_SAMPLES) {
				printf("%s:%d: sample %" PRIu64 " past the end of the buffer\n", name, n, t);
				bad = 1;
				break;
			}
			if (32 * (t + 64) > now) {
				bad = advance(&w, 32 * (t + 64), name, n);
			}
			const int sd = side == 'L' ? 0 : 1;
			uint32_t got = 0;
			for (uint64_t k = t; k < t + 64; k++) {
				got = crc(got, (uint16_t)out[sd][k]);
			}
			if (!bad && got != c) {
				printf("%s:%d: samples %" PRIu64 "..%" PRIu64 " %c: crc %08x, expected %08x\n",
				       name, n, t, t + 63, side, got, c);
				bad = 1;
			}
		} else if (line[0] == 'e') {
			if (w.open) {
				bad = advance(&w, w.end + 1, name, n);
			}
			break;
		}
	}
	fclose(f);
	return bad;
}

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : YM_VECTOR_DIR;
	char path[1024], name[256];
	snprintf(path, sizeof path, "%s/MANIFEST", dir);
	FILE *m = fopen(path, "r");
	if (m == NULL) {
		printf("%s: cannot open\n", path);
		return 1;
	}
	YM_Create(3579545);
	YM_init(3579545 / 64, 60);
	int files = 0, failed = 0;
	while (fscanf(m, "%255s", name) == 1) {
		files++;
		dense = true;
		int bad = run_file(dir, name);
		dense = false;
		bad |= run_file(dir, name);
		failed += bad;
	}
	fclose(m);
	printf("ym accuracy: %d files, %d failed\n", files, failed);
	return failed != 0;
}
