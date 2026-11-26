// VIA bus vectors: tests/accuracy/README.md.
//
//     via_accuracy [DIR]
//
// T counts PHI2 cycles from the first access. Each file runs twice: a cycle at
// a time, then as the main loop drives it, in instructions of up to seven
// cycles whose accesses catch the VIA up to their own cycle.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "via.h"

#ifndef VIA_VECTOR_DIR
#define VIA_VECTOR_DIR "."
#endif

static bool     dense;
static unsigned now;    // the cycle in progress
static unsigned base;   // where the current instruction began

// Into cycle t, ending every cycle before it.
static void
run_to(unsigned t)
{
	if (dense) {
		while (now < t) {
			via2_step(1);
			now++;
		}
		base = now;
		return;
	}
	if (t > now) {
		// The instruction holding the last access ends with that access.
		via2_step(now + 1 - base);
		base = now + 1;
		while (t - base >= 7) {
			via2_step(7);
			base += 7;
		}
	}
	via2_catch_up(t - base);
	now = t;
}

static int
run_file(const char *dir, const char *name)
{
	char path[1024], line[256];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (!f) {
		printf("%s: cannot open\n", path);
		return 1;
	}
	via2_init();
	now = base = 0;
	// The pin is sampled as the CPU takes data, before the access's own effects.
	bool     accessed   = false;
	unsigned irq_before = 0;
	int failed = 0, n = 0;
	while (fgets(line, sizeof line, f)) {
		n++;
		unsigned reg, val, t;
		if (line[0] == 'W' && sscanf(line + 1, "%x %x %u", &reg, &val, &t) == 3) {
			run_to(t);
			irq_before = via2_irq();
			accessed   = true;
			via2_write((uint8_t)reg, (uint8_t)val);
		} else if (line[0] == 'R' && sscanf(line + 1, "%x %x %u", &reg, &val, &t) == 3) {
			run_to(t);
			irq_before = via2_irq();
			accessed   = true;
			const unsigned got = via2_read((uint8_t)reg, false);
			if (got != val) {
				printf("%s:%d: %s read of %X at %u gave %02X, expected %02X\n", name, n,
				       dense ? "dense" : "sparse", reg, t, got, val);
				failed = 1;
				break;
			}
		} else if (line[0] == 'I' && sscanf(line + 1, "%u %u", &val, &t) == 2) {
			if (!accessed || t != now) {
				run_to(t);
				irq_before = via2_irq();
			}
			if (irq_before != val) {
				printf("%s:%d: %s IRQ in cycle %u is %u, expected %u\n", name, n,
				       dense ? "dense" : "sparse", t, irq_before, val);
				failed = 1;
				break;
			}
		}
	}
	fclose(f);
	return failed;
}

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : VIA_VECTOR_DIR;
	char path[1024], name[256];
	snprintf(path, sizeof path, "%s/MANIFEST", dir);
	FILE *m = fopen(path, "r");
	if (!m) {
		printf("%s: cannot open\n", path);
		return 1;
	}
	int files = 0, failed = 0;
	while (fgets(name, sizeof name, m)) {
		name[strcspn(name, "\r\n")] = 0;
		if (!name[0] || name[0] == '#') {
			continue;
		}
		files++;
		dense = true;
		int bad = run_file(dir, name);
		dense = false;
		bad |= run_file(dir, name);
		failed += bad;
	}
	fclose(m);
	printf("via accuracy: %d files, %d failed\n", files, failed);
	return failed != 0;
}
