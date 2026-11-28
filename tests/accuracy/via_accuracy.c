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
	// The testbench's idle inputs.
	via2_drive(VIA_PIN_PA, 0xff);
	via2_drive(VIA_PIN_PB, 0xff);
	// The pins are sampled as the CPU takes data, before the access's own effects.
	long       access_t   = -1; // the cycle of the last access
	unsigned   irq_before = 0;
	via_pins_t pins_before;
	via2_pins(&pins_before);
	// A cycle's input changes land together as its clock samples them: a pin's
	// last level that cycle, the ports before the control pins, so an active
	// transition latches the port as that cycle has it. An O or I line for the
	// cycle before comes after them in the file.
	int      level[6], pending = 0;
	unsigned pending_t = 0;
	int failed = 0, n = 0;
	while (fgets(line, sizeof line, f)) {
		n++;
		unsigned reg, val, t;
		char pin[4];
		const bool is_pin = line[0] == 'P' && sscanf(line + 1, "%3s %x %u", pin, &val, &t) == 3;
		unsigned line_t = 0;
		bool     timed  = false;
		if (is_pin) {
			line_t = t;
			timed  = true;
		} else if (line[0] == 'W' || line[0] == 'R') {
			timed = sscanf(line + 1, "%*x %*x %u", &line_t) == 1;
		} else if (line[0] == 'I') {
			timed = sscanf(line + 1, "%*u %u", &line_t) == 1;
		} else if (line[0] == 'O') {
			timed = sscanf(line + 1, "%*s %*s %*s %*x %*x %*x %*x %u", &line_t) == 1;
		}
		if (pending && timed && line_t >= pending_t && (!is_pin || line_t > pending_t)) {
			run_to(pending_t);
			for (int k = 0; k < 2; k++) {
				for (int p = 0; p < 6; p++) {
					const bool port = p == VIA_PIN_PA || p == VIA_PIN_PB;
					if ((pending & (1 << p)) && port == (k == 0)) via2_drive(p, (uint8_t)level[p]);
				}
			}
			pending = 0;
		}
		if (is_pin) {
			static const char *names[] = { "CA1", "CA2", "CB1", "CB2", "PA", "PB" };
			int p = 0;
			while (p < 6 && strcmp(pin, names[p])) p++;
			if (p == 6) {
				printf("%s:%d: bad pin line\n", name, n);
				failed = 1;
				break;
			}
			level[p]  = (int)val;
			pending  |= 1 << p;
			pending_t = t;
			continue;
		}
		if (line[0] == 'W' && sscanf(line + 1, "%x %x %u", &reg, &val, &t) == 3) {
			run_to(t);
			irq_before = via2_irq();
			via2_pins(&pins_before);
			access_t   = (long)t;
			via2_write((uint8_t)reg, (uint8_t)val);
		} else if (line[0] == 'R' && sscanf(line + 1, "%x %x %u", &reg, &val, &t) == 3) {
			unsigned mask = 0xff; // the bits checked
			sscanf(line + 1, "%*x %*x %*u %x", &mask);
			run_to(t);
			irq_before = via2_irq();
			via2_pins(&pins_before);
			access_t   = (long)t;
			const unsigned got = via2_read((uint8_t)reg, false);
			if ((got ^ val) & mask) {
				printf("%s:%d: %s read of %X at %u gave %02X, expected %02X\n", name, n,
				       dense ? "dense" : "sparse", reg, t, got, val);
				failed = 1;
				break;
			}
		} else if (line[0] == 'I' && sscanf(line + 1, "%u %u", &val, &t) == 2) {
			if ((long)t != access_t) {
				run_to(t);
				irq_before = via2_irq();
			}
			if (irq_before != val) {
				printf("%s:%d: %s IRQ in cycle %u is %u, expected %u\n", name, n,
				       dense ? "dense" : "sparse", t, irq_before, val);
				failed = 1;
				break;
			}
		} else if (line[0] == 'O') {
			char c[3][4];
			unsigned pa, paoe, pb, pboe;
			if (sscanf(line + 1, "%3s %3s %3s %x %x %x %x %u", c[0], c[1], c[2], &pa, &paoe, &pb, &pboe, &t) != 8) {
				printf("%s:%d: bad output line\n", name, n);
				failed = 1;
				break;
			}
			via_pins_t got = pins_before;
			if ((long)t != access_t) {
				run_to(t);
				via2_pins(&got);
			}
			const int8_t lv[3] = { got.ca2, got.cb1, got.cb2 };
			char lc[3];
			for (int k = 0; k < 3; k++) {
				lc[k] = lv[k] < 0 ? 'Z' : (char)('0' + lv[k]);
				// X: a driven level the vector leaves unchecked.
				if (!strcmp(c[k], "X") && lv[k] >= 0) {
					c[k][0] = lc[k];
				}
			}
			char want[64], have[64];
			snprintf(want, sizeof want, "%s %s %s %02X %02X %02X %02X", c[0], c[1], c[2], pa, paoe, pb, pboe);
			snprintf(have, sizeof have, "%c %c %c %02X %02X %02X %02X", lc[0], lc[1], lc[2],
			         got.pa, got.pa_oe, got.pb, got.pb_oe);
			if (strcmp(want, have)) {
				printf("%s:%d: %s outputs in cycle %u are %s, expected %s\n", name, n,
				       dense ? "dense" : "sparse", t, have, want);
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
