// VERA accuracy vectors. Expected values come from the release named in each file.
//
// usage: vera_accuracy VECTOR_DIR

#include "video.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE (1u << 20)

// Timed ops carry T, clocks since the VSYNC rising edge, which is dot 4 of
// line 492; emulator dot x is RTL dot x - 1. top.v:257-265: a write takes
// effect 3 clocks after its strobe rises.
#define ANCHOR_Y     492
#define ANCHOR_X     5
#define WRITE_DELAY  3

static char line_buf_text[MAX_LINE];
static uint32_t crc_table[256];

static void
crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++) {
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		}
		crc_table[i] = c;
	}
}

static uint32_t
frame_line_crc(int y)
{
	uint32_t c = 0xFFFFFFFFu;
	for (int x = 0; x < SCREEN_WIDTH; x++) {
		const uint8_t *p = &framebuffer[(y * SCREEN_WIDTH + x) * 4];
		const uint16_t v = (uint16_t)(((p[2] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[0] >> 4));
		c = crc_table[(c ^ (v & 0xFF)) & 0xFF] ^ (c >> 8);
		c = crc_table[(c ^ (v >> 8)) & 0xFF] ^ (c >> 8);
	}
	return ~c;
}

static void
fill_vram(uint32_t seed)
{
	uint32_t x = seed ? seed : 1;
	for (uint32_t i = 0; i < sizeof video_ram; i += 4) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		video_ram[i + 0] = (uint8_t)x;
		video_ram[i + 1] = (uint8_t)(x >> 8);
		video_ram[i + 2] = (uint8_t)(x >> 16);
		video_ram[i + 3] = (uint8_t)(x >> 24);
	}
}

static void
step_frame(void)
{
	while (!video_step(25.0f, 1.0f, false)) {
	}
}

static unsigned
hex_byte(const char *s)
{
	char t[3] = { s[0], s[1], 0 };
	return (unsigned)strtoul(t, NULL, 16);
}

// Mid-frame writes: T as in bus vectors, applied WRITE_DELAY clocks after the strobe.
#define MAX_MID 4096
static long mid_t[MAX_MID];
static uint8_t mid_r[MAX_MID], mid_v[MAX_MID];
static int mid_n;
static bool mid_line;

static bool
run_scene(FILE *f, const char *name, uint32_t seed, int frame, size_t final_ops)
{
	if (!fgets(line_buf_text, MAX_LINE, f) || strncmp(line_buf_text, "ops ", 4) != 0) {
		printf("FAIL %s: missing ops\n", name);
		return false;
	}
	const char *ops = line_buf_text + 4;
	size_t n = strcspn(ops, "\r\n") / 4;

	video_reset();
	fill_vram(seed);
	for (size_t i = 0; i + final_ops < n; i++) {
		video_write((uint8_t)hex_byte(ops + i * 4), (uint8_t)hex_byte(ops + i * 4 + 2));
	}
	step_frame();
	if (mid_n || mid_line) {
		while (vga_scan_pos_y != ANCHOR_Y || vga_scan_pos_x != ANCHOR_X) {
			video_step(25.0f, 1.0f, false);
		}
		for (size_t i = n - final_ops; i < n; i++) {
			video_write((uint8_t)hex_byte(ops + i * 4), (uint8_t)hex_byte(ops + i * 4 + 2));
		}
		long now = 0;
		int frames = 0, k = 0;
		while (frames <= frame) {
			if (k < mid_n && now == mid_t[k] + WRITE_DELAY) {
				video_write(mid_r[k], mid_v[k]);
				k++;
				continue;
			}
			frames += video_step(25.0f, 1.0f, mid_line);
			now++;
		}
	} else {
		// The final writes land just after VSYNC, in blanking.
		while (vga_scan_pos_y != 493) {
			video_step(25.0f, 1.0f, false);
		}
		for (size_t i = n - final_ops; i < n; i++) {
			video_write((uint8_t)hex_byte(ops + i * 4), (uint8_t)hex_byte(ops + i * 4 + 2));
		}
		for (int i = 0; i <= frame; i++) {
			step_frame();
		}
	}

	if (!fgets(line_buf_text, MAX_LINE, f) || strncmp(line_buf_text, "crc ", 4) != 0) {
		printf("FAIL %s: missing crc\n", name);
		return false;
	}
	int bad = 0, first = -1;
	const char *p = line_buf_text + 4;
	for (int y = 0; y < SCREEN_HEIGHT; y++) {
		char *end;
		const uint32_t want = (uint32_t)strtoul(p, &end, 16);
		p = end;
		if (frame_line_crc(y) != want) {
			if (first < 0) {
				first = y;
			}
			bad++;
		}
	}
	if (bad) {
		printf("FAIL %s: %d of %d lines differ, first %d\n", name, bad, SCREEN_HEIGHT, first);
		return false;
	}
	return true;
}


static long bus_now;

static void
step_dot(void)
{
	video_step(25.0f, 1.0f, false);
	bus_now = (vga_scan_pos_y == ANCHOR_Y && vga_scan_pos_x == ANCHOR_X) ? 0 : bus_now + 1;
}

// T restarts at every VSYNC rising edge, as tb_bus.v counts it.
static bool
step_to(long t)
{
	for (long n = 0; bus_now != t; n++) {
		if (n > 800L * 525) {
			return false;
		}
		step_dot();
	}
	return true;
}

static void
step_to_anchor(void)
{
	do {
		step_dot();
	} while (bus_now != 0);
}

static bool
run_bus(FILE *f, const char *name)
{
	int bad = 0, step = 0;
	video_reset();
	while (fgets(line_buf_text, MAX_LINE, f)) {
		unsigned r, v, m = 0xFF;
		long t;
		step++;
		if (line_buf_text[0] == 'e') {
			break;
		} else if (line_buf_text[0] == 'f') {
			step_frame();
		} else if (line_buf_text[0] == 'v') {
			step_to_anchor();
		} else if (sscanf(line_buf_text, "W %x %x %ld", &r, &v, &t) == 3) {
			if (!step_to(t + WRITE_DELAY)) {
				printf("FAIL %s: step %d is out of order\n", name, step);
				return false;
			}
			video_write((uint8_t)r, (uint8_t)v);
		} else if (sscanf(line_buf_text, "R %x %x %x %ld", &r, &v, &m, &t) == 4) {
			if (!step_to(t)) {
				printf("FAIL %s: step %d is out of order\n", name, step);
				return false;
			}
			const uint8_t got = video_read((uint8_t)r, false);
			if ((got & m) != (v & m)) {
				if (!bad) {
					printf("FAIL %s: step %d T %ld reg $%02X read $%02X, expected $%02X (mask $%02X)\n",
					       name, step, t, r, got, v, m);
				}
				bad++;
			}
		} else if (sscanf(line_buf_text, "I %u %ld", &v, &t) == 2) {
			if (!step_to(t)) {
				printf("FAIL %s: step %d is out of order\n", name, step);
				return false;
			}
			const unsigned pin = video_get_irq_out() ? 0 : 1;
			if (pin != v) {
				if (!bad) {
					printf("FAIL %s: step %d T %ld IRQ pin %u, expected %u\n", name, step, t, pin, v);
				}
				bad++;
			}
		} else if (sscanf(line_buf_text, "w %x %x", &r, &v) == 2) {
			video_write((uint8_t)r, (uint8_t)v);
		} else if (sscanf(line_buf_text, "r %x %x %x", &r, &v, &m) >= 2) {
			const uint8_t got = video_read((uint8_t)r, false);
			if ((got & m) != (v & m)) {
				if (!bad) {
					printf("FAIL %s: step %d reg $%02X read $%02X, expected $%02X (mask $%02X)\n",
					       name, step, r, got, v, m);
				}
				bad++;
			}
		}
	}
	if (bad > 1) {
		printf("FAIL %s: %d reads differ\n", name, bad);
	}
	return bad == 0;
}

static bool
run_file(const char *path, int *cases)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		printf("FAIL cannot open %s\n", path);
		return false;
	}
	bool ok = true;
	char name[128] = "";
	uint32_t seed = 0;
	int frame = 1;
	unsigned final_ops = 1;
	while (fgets(line_buf_text, MAX_LINE, f)) {
		if (sscanf(line_buf_text, "scene %127s", name) == 1) {
			(*cases)++;
		} else if (sscanf(line_buf_text, "seed %x", &seed) == 1) {
		} else if (sscanf(line_buf_text, "final %u", &final_ops) == 1) {
		} else if (strncmp(line_buf_text, "midline 1", 9) == 0) {
			mid_line = true;
		} else if (strncmp(line_buf_text, "mid ", 4) == 0) {
			unsigned r, v;
			if (mid_n < MAX_MID && sscanf(line_buf_text, "mid %ld %x %x", &mid_t[mid_n], &r, &v) == 3) {
				mid_r[mid_n] = (uint8_t)r;
				mid_v[mid_n] = (uint8_t)v;
				mid_n++;
			}
		} else if (sscanf(line_buf_text, "frame %d", &frame) == 1) {
			ok &= run_scene(f, name, seed, frame, final_ops);
			mid_n = 0;
			mid_line = false;
		} else if (sscanf(line_buf_text, "bus %127s", name) == 1) {
			(*cases)++;
			ok &= run_bus(f, name);
		}
	}
	fclose(f);
	return ok;
}

int
main(int argc, char **argv)
{
#ifdef VERA_VECTOR_DIR
	const char *dir = argc > 1 ? argv[1] : VERA_VECTOR_DIR;
#else
	if (argc != 2) {
		fprintf(stderr, "usage: vera_accuracy VECTOR_DIR\n");
		return 2;
	}
	const char *dir = argv[1];
#endif
	crc_init();

	char path[1024];
	snprintf(path, sizeof path, "%s/MANIFEST", dir);
	FILE *m = fopen(path, "r");
	if (!m) {
		printf("FAIL cannot open %s\n", path);
		return 1;
	}
	int cases = 0, failed_files = 0, files = 0;
	char name[256];
	while (fscanf(m, "%255s", name) == 1) {
		char vec[1300];
		snprintf(vec, sizeof vec, "%s/%s", dir, name);
		files++;
		if (!run_file(vec, &cases)) {
			failed_files++;
		}
	}
	fclose(m);

	// KNOWN_GAPS: captured from hardware, not yet matched. A pass means the entry is stale.
	int gaps = 0, stale = 0;
	snprintf(path, sizeof path, "%s/KNOWN_GAPS", dir);
	m = fopen(path, "r");
	if (m) {
		while (fscanf(m, "%255s", name) == 1) {
			char vec[1300];
			int gap_cases = 0;
			snprintf(vec, sizeof vec, "%s/%s", dir, name);
			gaps++;
			if (run_file(vec, &gap_cases)) {
				printf("FAIL %s: listed in KNOWN_GAPS but matches\n", name);
				stale++;
			} else {
				printf("DIVERGE %s\n", name);
			}
		}
		fclose(m);
	}
	printf("vera accuracy: %d cases in %d files, %d files failed, %d known gaps\n",
	       cases, files, failed_files, gaps);
	return (failed_files || stale || !files) ? 1 : 0;
}
