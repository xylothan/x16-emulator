// The 65C22's interval timers: T1 ($04-$07) and T2 ($08-$09).
// Both count down at the PHI2 rate and raise an interrupt flag at zero.
// Subject is VIA 2, as in tests/test_via_irq.c. The counter is observed with
// debug reads, since a real read of T1C-L or T2C-L clears the flag (Table 2-11).
//
// ORACLE: C -- WDC W65C22S datasheet, pages 17-19, quoted inline. Fetch it
// with `python tests/refs/fetch_refs.py`.
//
// NOT COVERED: PB7 output (ACR bit 7), PB6 pulse counting: nothing reads them.

#include "support/harness.h"

#include "via.h"

#include <stdint.h>

#define VIA_T1C_L 4
#define VIA_T1C_H 5
#define VIA_T1L_L 6
#define VIA_T1L_H 7
#define VIA_T2C_L 8
#define VIA_T2C_H 9
#define VIA_ACR   11
#define VIA_IFR   13

#define IFR_T2 0x20
#define IFR_T1 0x40

#define ACR_ONE_SHOT 0x00
#define ACR_FREE_RUN 0x40 // ACR bit 6
#define ACR_T2_PULSE 0x20 // ACR bit 5

static uint8_t
ifr(void)
{
	return via2_read(VIA_IFR, true);
}

static uint16_t
t1_count(void)
{
	return (uint16_t)(via2_read(VIA_T1C_L, true) | (via2_read(VIA_T1C_H, true) << 8));
}

static uint16_t
t2_count(void)
{
	return (uint16_t)(via2_read(VIA_T2C_L, true) | (via2_read(VIA_T2C_H, true) << 8));
}

// p17: "the microprocessor does not write directly into the T1 low order
//       counter. Instead, this half of the counter is loaded automatically
//       from the low order register when the microprocessor writes into the
//       high order register and counter."
static void
test_writing_t1_high_loads_the_counter_from_the_latch(void)
{
	via2_init();
	via2_write(VIA_ACR, ACR_ONE_SHOT);

	// via_init() leaves the counters alone; put a known value in first.
	via2_write(VIA_T1L_L, 0xFF);
	via2_write(VIA_T1C_H, 0xFF);
	check_eq(t1_count(), 0xFFFFu, "the counter holds a known value");

	via2_write(VIA_T1L_L, 0x34);
	check_eq(t1_count(), 0xFFFFu, "the low latch alone does not load the counter");

	via2_write(VIA_T1C_H, 0x12);
	check_eq(t1_count(), 0x1234u, "writing the high byte loads both halves at once");
}

static void
test_t1_counts_down_at_the_clock_rate(void)
{
	via2_init();
	via2_write(VIA_ACR, ACR_ONE_SHOT);
	via2_write(VIA_T1L_L, 0x00);
	via2_write(VIA_T1C_H, 0x20);

	via2_step(0x100);
	check_eq(t1_count(), 0x1F00u, "T1 decrements once per clock");

	via2_step(0x0F00);
	check_eq(t1_count(), 0x1000u, "and keeps going");
	check_eq(ifr() & IFR_T1, 0, "without raising its flag before zero");
}

// p17: "Interval Timer T1 may operate in the One-Shot Mode that allows the
//       generation of a single Interrupt Flag each time the Timer is loaded."
//
// p18: "When operating in the Free-Run Mode, the Interrupt Flag is set and the
//       signal on PB7 is inverted each time the counter reaches a count of
//       zero... it is not necessary to reload the timer in order to set the
//       Interrupt Flag on the next count of zero."
static void
test_one_shot_raises_the_flag_once_and_free_run_repeats(void)
{
	via2_init();
	via2_write(VIA_ACR, ACR_ONE_SHOT);
	via2_write(VIA_T1L_L, 0x10);
	via2_write(VIA_T1C_H, 0x00);

	via2_step(0x40);
	check_eq(ifr() & IFR_T1, IFR_T1, "one-shot raises the flag at zero");

	via2_write(VIA_IFR, IFR_T1);
	via2_step(0x4000);
	check_eq(ifr() & IFR_T1, 0, "and never again without another load");

	// The same sequence with only ACR bit 6 changed.
	via2_init();
	via2_write(VIA_ACR, ACR_FREE_RUN);
	via2_write(VIA_T1L_L, 0x10);
	via2_write(VIA_T1C_H, 0x00);

	via2_step(0x40);
	check_eq(ifr() & IFR_T1, IFR_T1, "free-run raises the flag at zero");

	via2_write(VIA_IFR, IFR_T1);
	via2_step(0x40);
	check_eq(ifr() & IFR_T1, IFR_T1, "and raises it again with no reload");
}

// p18: "By loading the latches only, the microprocessor can access the timer
//       during each countdown operation without affecting the time out in
//       progress. In this way, data loaded into the latches will determine the
//       length of the next subsequent time out period."
static void
test_loading_the_latches_does_not_disturb_a_count_in_progress(void)
{
	via2_init();
	via2_write(VIA_ACR, ACR_FREE_RUN);
	via2_write(VIA_T1L_L, 0x00);
	via2_write(VIA_T1C_H, 0x20);
	via2_step(0x100);

	const uint16_t before = t1_count();
	check_eq(before, 0x1F00u, "a count is in progress");

	via2_write(VIA_T1L_L, 0x55);
	check_eq(t1_count(), before, "writing the low latch leaves the counter alone");

	via2_write(VIA_T1L_H, 0x66);
	check_eq(t1_count(), before, "and so does writing the high latch");
}

// p19: "Timer 2 (T2) operates in the One-Shot Mode only (as an interval
//       timer), or as a pulse counter for counting negative pulses on PB6."
//
// Table 2-9: "T2H WRITE - 8 bits loaded into T2 high order counter. Also, low
//       order latches are transferred to low order counter."
static void
test_t2_loads_counts_and_fires_once(void)
{
	via2_init();
	via2_write(VIA_ACR, ACR_ONE_SHOT);

	via2_write(VIA_T2C_L, 0xFF);
	via2_write(VIA_T2C_H, 0xFF);
	check_eq(t2_count(), 0xFFFFu, "the counter holds a known value");

	via2_write(VIA_T2C_L, 0x78);
	check_eq(t2_count(), 0xFFFFu, "the low latch alone does not load T2");

	via2_write(VIA_T2C_H, 0x56);
	check_eq(t2_count(), 0x5678u, "writing the high byte loads both halves");

	via2_step(0x78);
	check_eq(t2_count(), 0x5600u, "T2 decrements once per clock");
	check_eq(ifr() & IFR_T2, 0, "with no flag before zero");

	via2_step(0x5700);
	check_eq(ifr() & IFR_T2, IFR_T2, "and raises its flag at zero");

	// One-shot only: there is no free-run bit for T2.
	via2_write(VIA_IFR, IFR_T2);
	via2_step(0x10000);
	check_eq(ifr() & IFR_T2, 0, "T2 has no free-run mode to raise it again");
}

// p19: "A single control bit within ACR5 is used to select between these two
//       modes." Nothing drives PB6 on VIA 2, so T2 does not count in pulse mode.
static void
test_acr_bit_5_takes_t2_off_the_clock(void)
{
	via2_init();
	via2_write(VIA_ACR, ACR_T2_PULSE);
	via2_write(VIA_T2C_L, 0x00);
	via2_write(VIA_T2C_H, 0x10);

	via2_step(0x4000);
	check_eq(t2_count(), 0x1000u, "T2 in pulse mode ignores the clock");
	check_eq(ifr() & IFR_T2, 0, "and never times out on it");

	// The same count with the mode bit clear.
	via2_init();
	via2_write(VIA_ACR, ACR_ONE_SHOT);
	via2_write(VIA_T2C_L, 0x00);
	via2_write(VIA_T2C_H, 0x10);

	via2_step(0x4000);
	check_eq(ifr() & IFR_T2, IFR_T2, "while on the clock the same count times out");
}

int
main(void)
{
	test_writing_t1_high_loads_the_counter_from_the_latch();
	test_t1_counts_down_at_the_clock_rate();
	test_one_shot_raises_the_flag_once_and_free_run_repeats();
	test_loading_the_latches_does_not_disturb_a_count_in_progress();
	test_t2_loads_counts_and_fires_once();
	test_acr_bit_5_takes_t2_off_the_clock();
	return x16_test_summary("via_timers");
}
