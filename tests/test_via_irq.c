// The 65C22's interrupt flags and enables: IFR ($0D) and IER ($0E).
//
// Two VIAs sit at $9F00 and $9F10. The interrupt logic is shared between them
// and is entirely internal to the chip -- no timing model, no peripherals --
// which makes it the part of via.c that can be pinned exactly.
//
// The tests drive VIA 2. via2_read() and via2_write() pass straight through to
// the generic implementation both VIAs use (via.c:345-355), while VIA 1's
// ports are wired to I2C, the IEC bus and the NES controllers. Driving the
// user port exercises the same registers with none of that attached. VIA 1 is
// used only to show the two do not share state.
//
// ORACLE: C -- WDC W65C22S datasheet, pages 26-27, quoted inline. Fetch it
// with `python tests/refs/fetch_refs.py`; the text extraction carries
// `===== page N =====` markers so a citation can be found again.
//
// NOT COVERED, because via.c cannot produce these flags at all: IFR bits 0, 1,
// 3 and 4 (CA2, CA1, CB2, CB1) and bit 2 (shift register). via.c:206 leaves
// "Cxx pin and shift register handling" as a TODO, so nothing sets them and no
// test can arrange for them to be set. The code that clears them, including
// the PCR "independent input" exception at via.c:47 and via.c:56, is therefore
// unreachable from outside. That is a gap in the emulator, not in this file,
// and it is recorded rather than asserted around.

#include "support/harness.h"

#include "via.h"

#include <stdint.h>

#define VIA_ORB   0
#define VIA_ORA   1
#define VIA_T1C_L 4
#define VIA_T1C_H 5
#define VIA_T1L_L 6
#define VIA_T1L_H 7
#define VIA_T2C_L 8
#define VIA_T2C_H 9
#define VIA_SR    10
#define VIA_ACR   11
#define VIA_PCR   12
#define VIA_IFR   13
#define VIA_IER   14

#define IFR_CA2  0x01
#define IFR_CA1  0x02
#define IFR_SR   0x04
#define IFR_CB2  0x08
#define IFR_CB1  0x10
#define IFR_T2   0x20
#define IFR_T1   0x40
#define IFR_IRQ  0x80

// IER bit 7 selects between setting and clearing, so it is written rather than
// stored. Named so the intent of a write is legible.
#define IER_SET   0x80
#define IER_CLEAR 0x00

static uint8_t
ifr(void)
{
	return via2_read(VIA_IFR, true);
}

static uint8_t
ier(void)
{
	return via2_read(VIA_IER, true);
}

// The only flags via.c can raise are the two timers. T1 in one-shot mode times
// out once and sets its flag; counting past it is enough.
static void
raise_t1(void)
{
	via2_write(VIA_ACR, 0x00);      // one-shot, not free-running
	via2_write(VIA_T1L_L, 0x10);
	via2_write(VIA_T1C_H, 0x00);    // starts T1 at 0x0010
	via2_step(0x40);
}

static void
raise_t2(void)
{
	via2_write(VIA_ACR, 0x00);      // count clocks, not PB6 pulses
	via2_write(VIA_T2C_L, 0x10);
	via2_write(VIA_T2C_H, 0x00);    // starts T2 at 0x0010
	via2_step(0x40);
}

// p26: "The microprocessor can set or clear selected bits within the IER. This
//       allows the control of individual interrupts without affecting others.
//       To set or clear a particular Interrupt Enable bit, the microprocessor
//       must write to the IER address. During this write operation, if IER7 is
//       a Logic 0, each Logic 1 in IER6 thru IER0 will clear the corresponding
//       bit in the IER. For each Logic 0 in IER6 thru IER0, the corresponding
//       bit in the IER will be unaffected."
//
// Table 2-12 note 3: "If a read of this register is done, bit 7 will be Logic 1
//       and all other bits will reflect their enable/disable state."
static void
test_ier_sets_and_clears_selected_bits(void)
{
	via2_init();
	check_eq(ier(), 0x80, "IER starts with nothing enabled and bit 7 reading one");

	via2_write(VIA_IER, IER_SET | IFR_T1 | IFR_CA1);
	check_eq(ier(), (uint8_t)(0x80 | IFR_T1 | IFR_CA1), "a write with bit 7 set enables those bits");

	// A zero in a position leaves that bit alone, which is the whole point of
	// the convention: one interrupt can be changed without reading first.
	via2_write(VIA_IER, IER_SET | IFR_T2);
	check_eq(ier(), (uint8_t)(0x80 | IFR_T1 | IFR_T2 | IFR_CA1), "and leaves the others alone");

	via2_write(VIA_IER, IER_CLEAR | IFR_CA1);
	check_eq(ier(), (uint8_t)(0x80 | IFR_T1 | IFR_T2), "a write with bit 7 clear disables those bits");

	via2_write(VIA_IER, IER_CLEAR | 0x00);
	check_eq(ier(), (uint8_t)(0x80 | IFR_T1 | IFR_T2), "a write of all zeroes changes nothing");

	via2_write(VIA_IER, IER_SET | 0x00);
	check_eq(ier(), (uint8_t)(0x80 | IFR_T1 | IFR_T2), "and neither does bit 7 alone");

	// Bit 7 is the set/clear selector, not storage, so it cannot be enabled.
	via2_write(VIA_IER, IER_SET | 0x7F);
	check_eq(ier(), 0xFF, "every enable can be set at once");
	via2_write(VIA_IER, IER_CLEAR | 0x7F);
	check_eq(ier(), 0x80, "and cleared again, with bit 7 still reading one");
}

// p26: "individual flag bits may be cleared by writing a Logic 1 into the
//       appropriate bit of the IFR."
static void
test_ifr_flags_clear_on_writing_one(void)
{
	via2_init();
	raise_t1();
	check_eq(ifr() & IFR_T1, IFR_T1, "T1 timing out raises its flag");

	via2_write(VIA_IFR, 0x00);
	check_eq(ifr() & IFR_T1, IFR_T1, "writing zero clears nothing");

	via2_write(VIA_IFR, IFR_T2);
	check_eq(ifr() & IFR_T1, IFR_T1, "and writing a one elsewhere leaves it set");

	via2_write(VIA_IFR, IFR_T1);
	check_eq(ifr() & IFR_T1, 0, "writing its own bit clears it");
}

// p26: "Bit 7 of the IFR indicates the status of the IRQB output. Bit 7
//       corresponds to the following logic function:
//       IRQ = IFR6 ∧ IER6 ∨ IFR5 ∧ IER5 ∨ IFR4 ∧ IER4 ∨ IFR3 ∧ IER3 ∨
//             IFR2 ∧ IER2 ∨ IFR1 ∧ IER1 ∨ IFR0 ∧ IER0."
static void
test_ifr_bit_7_is_the_irq_output(void)
{
	via2_init();
	raise_t1();

	// A flag with no enable behind it does not interrupt.
	check_eq(ifr() & IFR_IRQ, 0, "a raised flag alone does not assert IRQ");
	check(!via2_irq(), "and the interrupt line agrees");

	// An enable whose flag is clear does not either.
	via2_write(VIA_IER, IER_SET | IFR_T2);
	check_eq(ifr() & IFR_IRQ, 0, "nor does an enable with no flag behind it");
	check(!via2_irq(), "with the line still released");

	via2_write(VIA_IER, IER_SET | IFR_T1);
	check_eq(ifr() & IFR_IRQ, IFR_IRQ, "a flag against its own enable asserts IRQ");
	check(via2_irq(), "and raises the line");
}

// p26: "IFR7 is not a flag. Therefore, IFR7 is not directly cleared by writing
//       a Logic 1 into its bit position. It can be cleared, however, by
//       clearing all the flags within the register, or by disabling all active
//       interrupts as presented in the next section."
static void
test_ifr_bit_7_is_not_a_flag(void)
{
	via2_init();
	raise_t1();
	via2_write(VIA_IER, IER_SET | IFR_T1);
	check_eq(ifr() & IFR_IRQ, IFR_IRQ, "IRQ is asserted");

	via2_write(VIA_IFR, IFR_IRQ);
	check_eq(ifr() & IFR_IRQ, IFR_IRQ, "writing bit 7 does not clear it");
	check_eq(ifr() & IFR_T1, IFR_T1, "and does not disturb the flag beneath it");

	// The two routes the datasheet gives instead.
	via2_write(VIA_IFR, IFR_T1);
	check_eq(ifr() & IFR_IRQ, 0, "clearing the flag clears bit 7 with it");

	raise_t1();
	check_eq(ifr() & IFR_IRQ, IFR_IRQ, "raised again");
	via2_write(VIA_IER, IER_CLEAR | IFR_T1);
	check_eq(ifr() & IFR_IRQ, 0, "and disabling the interrupt clears bit 7 too");
	check_eq(ifr() & IFR_T1, IFR_T1, "while the flag itself stays raised");
}

// Table 2-11, the "CLEARED BY" column:
//     Time out of T1   Read T1C-L low or write T1L-H high
//     Time out of T2   Read T2 low or write T2 high
static void
test_the_timer_flags_clear_on_their_own_accesses(void)
{
	via2_init();

	raise_t1();
	via2_read(VIA_T1C_H, false);
	check_eq(ifr() & IFR_T1, IFR_T1, "reading T1C-H does not clear T1");
	via2_read(VIA_T1C_L, false);
	check_eq(ifr() & IFR_T1, 0, "reading T1C-L does");

	raise_t1();
	via2_write(VIA_T1L_L, 0x10);
	check_eq(ifr() & IFR_T1, IFR_T1, "writing T1L-L does not clear T1");
	via2_write(VIA_T1L_H, 0x00);
	check_eq(ifr() & IFR_T1, 0, "writing T1L-H does");

	raise_t2();
	via2_read(VIA_T2C_H, false);
	check_eq(ifr() & IFR_T2, IFR_T2, "reading T2C-H does not clear T2");
	via2_read(VIA_T2C_L, false);
	check_eq(ifr() & IFR_T2, 0, "reading T2C-L does");

	raise_t2();
	via2_write(VIA_T2C_H, 0x00);
	check_eq(ifr() & IFR_T2, 0, "writing T2C-H clears T2");
}

// A debug read is the debugger looking. The flag-clearing side effects above
// are exactly what it must not perform -- reading $9F14 in a memory view would
// otherwise acknowledge the guest's timer interrupt for it.
static void
test_a_debug_read_clears_no_flag(void)
{
	via2_init();

	raise_t1();
	via2_read(VIA_T1C_L, true);
	check_eq(ifr() & IFR_T1, IFR_T1, "a debug read of T1C-L leaves the flag raised");
	via2_read(VIA_T1C_L, false);
	check_eq(ifr() & IFR_T1, 0, "while a real read clears it");

	raise_t2();
	via2_read(VIA_T2C_L, true);
	check_eq(ifr() & IFR_T2, IFR_T2, "a debug read of T2C-L leaves the flag raised");
	via2_read(VIA_T2C_L, false);
	check_eq(ifr() & IFR_T2, 0, "while a real read clears it");
}

// Two chips, not one with two addresses.
static void
test_the_two_vias_keep_their_own_flags(void)
{
	via1_init();
	via2_init();

	raise_t1();
	via2_write(VIA_IER, IER_SET | IFR_T1);

	check(via2_irq(), "VIA 2 is interrupting");
	check(!via1_irq(), "VIA 1 is not");
	check_eq(via1_read(VIA_IFR, true) & IFR_T1, 0, "and holds no flag of its own");
	check_eq(via1_read(VIA_IER, true), 0x80, "nor any enable");
}

int
main(void)
{
	test_ier_sets_and_clears_selected_bits();
	test_ifr_flags_clear_on_writing_one();
	test_ifr_bit_7_is_the_irq_output();
	test_ifr_bit_7_is_not_a_flag();
	test_the_timer_flags_clear_on_their_own_accesses();
	test_a_debug_read_clears_no_flag();
	test_the_two_vias_keep_their_own_flags();
	return x16_test_summary("via_irq");
}
