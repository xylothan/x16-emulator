// Replays a YM2151 bus script on Nuked-OPM and prints what the chip shows.
//
//     cc -O2 -I<nuked-opm> -o tb_opm tb_opm.c <nuked-opm>/opm.c
//     tb_opm < script
//
// T counts OPM_Clock calls (2 phiM, 32 per sample) since IC was released.
// Script lines, in T order:
//   w P V T    write V to port P before clock T  -> W P V T
//   r T        status after T clocks       -> R V T
//   g A T      register A as held          -> G A V M T
//   x T0 T1    status and IRQ over T0..T1  -> X T0 T1, then C S I T per change,
//                                             interleaved with later lines
//   o N0 N S   DAC side S (L or R), samples N0..N0+N-1
//                                          -> O N0 S C per 64, before e
//   f KC       phase steps of operator 0 for KF 0-63, DT1 0-7, DT2 0-3,
//              MUL 0, 1, 7, 15             -> F KC C
//   m PMS KC   phase steps under a PM latch of 0-255 (sign and magnitude),
//              KF 0, 33, 63,
//              DT2 0-3, DT1 0, 6, MUL 1    -> M PMS KC C
//   e          end                         -> e
//
// C is the CRC-32 of the values in loop order, 4 bytes each, LSB first.
// Sample n is the one computed in clocks 32n..32n+31; the DAC shows it
// 5 samples later.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "opm.h"

static opm_t chip;
static long now;
static long watch_end = -1;
static int watch_s, watch_i;

static void watch(void)
{
	if (now > watch_end) {
		return;
	}
	const int s = OPM_Read(&chip, 1), i = OPM_ReadIRQ(&chip);
	if (s != watch_s || i != watch_i) {
		printf("C %02x %d %ld\n", s, i, now);
		watch_s = s;
		watch_i = i;
	}
}

#define OUT_LAG 5
#define MAX_SAMPLES 65536
#define MAX_BLOCKS 1024

static int16_t out[2][MAX_SAMPLES];
static struct { long n0; int side; } blocks[MAX_BLOCKS];
static int nblocks;

static void run_to(long t)
{
	int32_t dac[2];
	while (now < t) {
		OPM_Clock(&chip, dac, NULL, NULL, NULL);
		const long n = now / 32 - OUT_LAG;
		if ((now & 31) == 31 && n >= 0 && n < MAX_SAMPLES) {
			out[0][n] = (int16_t)dac[0];
			out[1][n] = (int16_t)dac[1];
		}
		now++;
		watch();
	}
}

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, uint32_t v)
{
	if (crc_table[1] == 0) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t k = i;
			for (int j = 0; j < 8; j++) {
				k = (k & 1) ? 0xedb88320u ^ (k >> 1) : k >> 1;
			}
			crc_table[i] = k;
		}
	}
	c = ~c;
	for (int i = 0; i < 4; i++, v >>= 8) {
		c = crc_table[(c ^ v) & 0xff] ^ (c >> 8);
	}
	return ~c;
}

// Operator 0's phase step, on a chip of its own.
static opm_t pg;

static uint32_t phase_step(int kc, int kf, int dt1, int dt2, int mul, int pms, int pm)
{
	pg.ch_kc[0] = kc;
	pg.ch_kf[0] = kf;
	pg.sl_dt1[0] = dt1;
	pg.sl_dt2[0] = dt2;
	pg.sl_mul[0] = mul;
	pg.ch_pms[0] = pms;
	for (int i = 0; i < 64; i++) {
		pg.lfo_pmd = 1;
		pg.lfo_pm_lock = pm;
		OPM_Clock(&pg, NULL, NULL, NULL, NULL);
	}
	return pg.pg_inc[0] & 0xfffff;
}

// The byte register A holds, and which of its bits the chip keeps.
static int held(int a, int *mask)
{
	const int c = a & 7, s = a & 0x1f;
	*mask = 0xff;
	switch (a) {
	case 0x0f: return chip.noise_en << 7 | chip.noise_freq;
	case 0x10: return chip.timer_a_reg >> 2;
	case 0x11: *mask = 0x03; return chip.timer_a_reg & 3;
	case 0x12: return chip.timer_b_reg;
	case 0x14: *mask = 0x8f; return chip.mode_csm << 7 | chip.timer_irqb << 3 | chip.timer_irqa << 2 | chip.timer_loadb << 1 | chip.timer_loada;
	case 0x18: return chip.lfo_freq_hi << 4 | chip.lfo_freq_lo;
	case 0x1b: *mask = 0xc3; return chip.io_ct2 << 7 | chip.io_ct1 << 6 | chip.lfo_wave;
	}
	switch (a & 0xf8) {
	case 0x20: return chip.ch_rl[c] << 6 | chip.ch_fb[c] << 3 | chip.ch_connect[c];
	case 0x28: *mask = 0x7f; return chip.ch_kc[c];
	case 0x30: *mask = 0xfc; return chip.ch_kf[c] << 2;
	case 0x38: *mask = 0x73; return chip.ch_pms[c] << 4 | chip.ch_ams[c];
	}
	switch (a & 0xe0) {
	case 0x40: *mask = 0x7f; return chip.sl_dt1[s] << 4 | chip.sl_mul[s];
	case 0x60: *mask = 0x7f; return chip.sl_tl[s];
	case 0x80: *mask = 0xdf; return chip.sl_ks[s] << 6 | chip.sl_ar[s];
	case 0xa0: *mask = 0x9f; return chip.sl_am_e[s] << 7 | chip.sl_d1r[s];
	case 0xc0: *mask = 0xdf; return chip.sl_dt2[s] << 6 | chip.sl_d2r[s];
	case 0xe0: return chip.sl_d1l[s] << 4 | chip.sl_rr[s];
	}
	*mask = 0;
	return 0;
}

int main(void)
{
	char line[128];
	OPM_Reset(&chip, 0);
	OPM_Reset(&pg, 0);
	now = 0;
	while (fgets(line, sizeof line, stdin)) {
		unsigned p, v, a;
		long t, t1;
		char side;
		if (sscanf(line, "w %u %x %ld", &p, &v, &t) == 3) {
			run_to(t);
			OPM_Write(&chip, p, (uint8_t)v);
			printf("W %u %02x %ld\n", p, v, t);
		} else if (sscanf(line, "r %ld", &t) == 1) {
			run_to(t);
			printf("R %02x %ld\n", OPM_Read(&chip, 1), t);
		} else if (sscanf(line, "g %x %ld", &a, &t) == 2) {
			int m, h;
			run_to(t);
			h = held((int)a, &m);
			printf("G %02x %02x %02x %ld\n", a, h & m, m, t);
		} else if (sscanf(line, "x %ld %ld", &t, &t1) == 2) {
			run_to(t);
			printf("X %ld %ld\n", t, t1);
			watch_end = t1;
			watch_s = watch_i = -1;
			watch();
		} else if (sscanf(line, "o %ld %ld %c", &t, &t1, &side) == 3) {
			for (long n0 = t; n0 < t + t1 && nblocks < MAX_BLOCKS; n0 += 64) {
				blocks[nblocks].n0 = n0;
				blocks[nblocks++].side = side == 'L' ? 0 : 1;
			}
		} else if (sscanf(line, "m %u %x", &p, &a) == 2) {
			static const int kfs[3] = { 0, 33, 63 }, dt1s[2] = { 0, 6 };
			uint32_t c = 0;
			for (int pm = 0; pm < 256; pm++)
				for (int k = 0; k < 3; k++)
					for (int dt2 = 0; dt2 < 4; dt2++)
						for (int d = 0; d < 2; d++)
							c = crc(c, phase_step((int)a, kfs[k], dt1s[d], dt2, 1, (int)p, pm));
			printf("M %u %02x %08x\n", p, a, c);
		} else if (sscanf(line, "f %x", &a) == 1) {
			static const int muls[4] = { 0, 1, 7, 15 };
			uint32_t c = 0;
			for (int kf = 0; kf < 64; kf++)
				for (int dt1 = 0; dt1 < 8; dt1++)
					for (int dt2 = 0; dt2 < 4; dt2++)
						for (int m = 0; m < 4; m++)
							c = crc(c, phase_step((int)a, kf, dt1, dt2, muls[m], 0, 0));
			printf("F %02x %08x\n", a, c);
		} else if (line[0] == 'e') {
			long last = watch_end;
			for (int b = 0; b < nblocks; b++) {
				const long need = 32 * (blocks[b].n0 + 64 + OUT_LAG + 1);
				if (need > last) {
					last = need;
				}
			}
			run_to(last);
			for (int b = 0; b < nblocks; b++) {
				uint32_t c = 0;
				for (long n = blocks[b].n0; n < blocks[b].n0 + 64; n++) {
					c = crc(c, (uint16_t)out[blocks[b].side][n]);
				}
				printf("O %ld %c %08x\n", blocks[b].n0, blocks[b].side ? 'R' : 'L', c);
			}
			printf("e\n");
			break;
		}
	}
	return 0;
}
