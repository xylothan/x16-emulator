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

static void
run_to(uint64_t t)
{
	static int16_t buf[2 * 64], host[2 * 64];
	while (samples < t / 32) {
		const uint64_t left = t / 32 - samples;
		const uint32_t n = left > 64 ? 64 : (uint32_t)left;
		YM_stream_update((uint16_t *)buf, (uint16_t *)host, n);
		samples += n;
	}
	YM_sync((uint8_t)(t % 32));
	now = t;
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
		unsigned p, v, m, a, s, i;
		uint64_t t, t1;
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
