// Checks for the shadow of live JSRs (src/c_frames.c), fed synthetic flow events.
// Hook `sp` is before the push for a call or interrupt, after the pull for a return.

#include "c_frames.h"
#include "cpu/irq_ctx.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void
check(bool cond, const char *what)
{
	if (!cond) {
		failures++;
		printf("FAIL: %s\n", what);
	} else {
		printf("ok  : %s\n", what);
	}
}

#define CSP_ADDR 0x22
#define CALLAX   0x0900

static uint8_t  mem[0x10000];
static uint16_t reg_ax;
static uint8_t  reg_e;
static uint16_t reg_dp;

static uint8_t
fake_read(uint16_t a)
{
	return mem[a];
}

static uint16_t
fake_ax(void)
{
	return reg_ax;
}

static void
set_csp(uint16_t v)
{
	mem[CSP_ADDR]     = (uint8_t)v;
	mem[CSP_ADDR + 1] = (uint8_t)(v >> 8);
}

static void
fresh(void)
{
	memset(mem, 0xEA, sizeof mem);
	reg_ax = 0;
	reg_e  = 1;
	reg_dp = 0;
	const c_frames_config_t cfg = {CSP_ADDR, CALLAX, mem, &reg_e, &reg_dp, fake_read, fake_ax};
	c_frames_configure(&cfg);
}

static void
call(uint16_t target, uint16_t sp, uint16_t ret)
{
	c_frames_flow(CPU_FLOW_CALL, target, sp, ret);
}

static void
rts(uint16_t sp)
{
	c_frames_flow(CPU_FLOW_RETURN, 0x0810, sp, 0);
}

static bool
record_is(uint16_t sp, uint16_t slot, uint16_t ret, uint16_t c_sp, uint16_t target)
{
	c_frame_record_t r;
	return c_frames_at_slot(sp, slot, &r) && r.slot == slot && r.ret == ret && r.c_sp == c_sp &&
	       r.target == target;
}

typedef struct {
	const char *name;
	uint32_t    addr;
} fake_sym_t;

static const fake_sym_t *sym_table;

static bool
sym_at(int i, const char **name, uint32_t *addr)
{
	*name = sym_table[i].name;
	*addr = sym_table[i].addr;
	return true;
}

static bool
pick(const fake_sym_t *t, int n, uint16_t *sp, int *callax)
{
	sym_table = t;
	return c_frames_pick_symbols(n, sym_at, sp, callax);
}

int
main(void)
{
	c_frames_configure(NULL);
	call(0x0900, 0x01F0, 0x0805);
	check(c_frames_live(0x01EE) == 0, "unconfigured: nothing recorded");

	// ── One call ────────────────────────────────────────────────────────────
	{
		fresh();
		set_csp(0x9E00);
		call(0x0840, 0x01F0, 0x0805);
		check(c_frames_live(0x01EE) == 1, "a JSR makes a record");
		check(record_is(0x01EE, 0x01EF, 0x0805, 0x9E00, 0x0840),
		      "slot is the pushed low byte; ret, c_sp, target kept");
		check(!c_frames_at_slot(0x01EE, 0x01F0, NULL), "another slot finds nothing");
		check(c_frames_live(0x01EE) == 1, "still live at the post-push SP");
		check(c_frames_live(0x01EF) == 0, "gone once SP reaches the slot");
	}

	// ── Nesting, recursion ──────────────────────────────────────────────────
	{
		fresh();
		set_csp(0x9E00);
		call(0x0840, 0x01F0, 0x0805);
		set_csp(0x9DFC);
		call(0x0850, 0x01EE, 0x0845);
		set_csp(0x9DF8);
		call(0x0850, 0x01EC, 0x0855);
		check(c_frames_live(0x01EA) == 3, "three nested");
		check(record_is(0x01EA, 0x01EF, 0x0805, 0x9E00, 0x0840), "outer");
		check(record_is(0x01EA, 0x01ED, 0x0845, 0x9DFC, 0x0850), "recursive, first");
		check(record_is(0x01EA, 0x01EB, 0x0855, 0x9DF8, 0x0850), "recursive, second");
		rts(0x01EC);
		check(c_frames_live(0x01EC) == 2 && !c_frames_at_slot(0x01EC, 0x01EB, NULL), "RTS retires the deepest");
		rts(0x01EE);
		rts(0x01F0);
		check(c_frames_live(0x01F0) == 0, "all returned");
	}

	// ── Same slot, new call ─────────────────────────────────────────────────
	{
		fresh();
		set_csp(0x9E00);
		call(0x0840, 0x01F0, 0x0805);
		set_csp(0x9D00);
		call(0x0860, 0x01F0, 0x0806);
		check(c_frames_live(0x01EE) == 1, "a JSR at the same SP replaces");
		check(record_is(0x01EE, 0x01EF, 0x0806, 0x9D00, 0x0860), "with the new values");
	}

	// ── callax and stubs ────────────────────────────────────────────────────
	{
		fresh();
		reg_ax = 0x0A34;
		call(CALLAX, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0A34), "callax resolves through A/X");
		reg_ax = 0x0B00;
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0A34), "at the call, not later");

		fresh();
		reg_ax = 0x0A34;
		call(0x0841, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0841), "other calls ignore A/X");

		fresh();
		mem[0x0A00] = 0x4C;
		mem[0x0A01] = 0x40;
		mem[0x0A02] = 0x08;
		call(0x0A00, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0840), "through JMP abs");

		fresh();
		mem[0x0A00] = 0x6C;
		mem[0x0A01] = 0x00;
		mem[0x0A02] = 0x0B;
		mem[0x0B00] = 0x50;
		mem[0x0B01] = 0x08;
		call(0x0A00, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0850), "through JMP (abs)");

		fresh();
		reg_ax      = 0x0A00;
		mem[0x0A00] = 0x4C;
		mem[0x0A01] = 0x40;
		mem[0x0A02] = 0x08;
		call(CALLAX, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0840), "callax to a stub");

		fresh();
		for (uint16_t a = 0x0A00; a < 0x0A30; a += 3) {
			mem[a]     = 0x4C;
			mem[a + 1] = (uint8_t)(a + 3);
			mem[a + 2] = 0x0A;
		}
		call(0x0A00, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0A0C), "a chain stops after four hops");

		fresh();
		mem[0x0A00] = 0x4C;
		mem[0x0A01] = 0x00;
		mem[0x0A02] = 0x0A;
		call(0x0A00, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0A00), "a JMP to itself ends");
	}

	// ── longjmp ─────────────────────────────────────────────────────────────
	{
		fresh();
		call(0x0840, 0x01F0, 0x0805);
		call(0x0850, 0x01EE, 0x0845);
		call(0x0860, 0x01EC, 0x0855);
		check(c_frames_live(0x01EC) == 2, "a raised SP retires with no event");
		check(!c_frames_at_slot(0x01EC, 0x01EB, NULL), "the record is gone");
		check(c_frames_live(0x01EA) == 2, "and does not return when SP falls again");

		fresh();
		call(0x0840, 0x01F0, 0x0805);
		call(0x0850, 0x01EE, 0x0845);
		call(0x0860, 0x01F0, 0x0806);
		check(c_frames_live(0x01EE) == 1 && record_is(0x01EE, 0x01EF, 0x0806, 0xEAEA, 0x0860),
		      "the next JSR retires what a longjmp left");
	}

	// ── Interrupts ──────────────────────────────────────────────────────────
	{
		fresh();
		call(0x0840, 0x01F0, 0x0805);
		c_frames_flow(CPU_FLOW_IRQ_ENTER, 0x0C00, 0x01EE, 0xFFFE);
		check(c_frames_live(0x01EB) == 1, "interrupt entry makes no record");
		call(0x0C40, 0x01EB, 0x0C05);
		check(c_frames_live(0x01E9) == 2, "a call inside the handler does");
		rts(0x01EB);
		c_frames_flow(CPU_FLOW_IRQ_LEAVE, 0x0848, 0x01EE, 0);
		check(c_frames_live(0x01EE) == 1 && record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0840),
		      "RTI leaves the interrupted frame");

		fresh();
		call(0x0840, 0x01F0, 0x0805);
		c_frames_flow(CPU_FLOW_IRQ_ENTER, 0x0C00, 0x01F2, 0xFFFE);
		check(c_frames_live(0x01EF) == 0, "interrupt entry retires by its own SP");
	}

	// ── JSL ─────────────────────────────────────────────────────────────────
	{
		fresh();
		call(0x0840, 0x01F0, 0x0805);
		c_frames_flow(CPU_FLOW_CALL_LONG, 0x0850, 0x01EE, 0x0845);
		check(c_frames_live(0x01EB) == 1, "JSL makes no record");

		fresh();
		call(0x0840, 0x01F0, 0x0805);
		call(0x0850, 0x01EE, 0x0845);
		c_frames_flow(CPU_FLOW_CALL_LONG, 0x0860, 0x01EE, 0x0845);
		check(c_frames_live(0x01EB) == 1 && !c_frames_at_slot(0x01EB, 0x01ED, NULL),
		      "JSL retires a record its push overwrites");
	}

	// ── Not capturable ──────────────────────────────────────────────────────
	{
		fresh();
		call(0x0840, 0x01F0, 0x0805);
		reg_e = 0;
		call(0x0850, 0x01EE, 0x0845);
		check(c_frames_live(0x01EC) == 0, "native mode drops every record");
		reg_e = 1;
		check(c_frames_live(0x01EC) == 0, "and they stay dropped");
		call(0x0860, 0x01EC, 0x0855);
		check(c_frames_live(0x01EA) == 1, "capture resumes with the next call");

		fresh();
		call(0x0840, 0x01F0, 0x0805);
		reg_e = 0;
		rts(0x01F0);
		check(c_frames_live(0x01EE) == 0, "a return while not capturable drops them too");

		fresh();
		call(0x0840, 0x01F0, 0x0805);
		reg_dp = 0x0200;
		call(0x0850, 0x01EE, 0x0845);
		check(c_frames_live(0x01EC) == 0, "D != 0 drops every record");
	}

	// ── Targets above low RAM ───────────────────────────────────────────────
	{
		fresh();
		set_csp(0x9E00);
		call(0xC000, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0x9E00, 0xC000), "a target in ROM that is no stub");

		fresh();
		mem[0xFFD2] = 0x4C;
		mem[0xFFD3] = 0x34;
		mem[0xFFD4] = 0xC2;
		call(0xFFD2, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0xC234), "a stub in ROM");

		fresh();
		mem[0x9EFF] = 0x4C;
		mem[0x9F00] = 0x40;
		mem[0x9F01] = 0x08;
		call(0x9EFF, 0x01F0, 0x0805);
		check(record_is(0x01EE, 0x01EF, 0x0805, 0xEAEA, 0x0840), "a stub on the last byte of low RAM");
	}

	// ── Wrap of the 8-bit stack ─────────────────────────────────────────────
	{
		for (uint16_t sp = 0x0100; sp <= 0x0102; sp++) {
			fresh();
			call(0x0840, 0x01F0, 0x0805);
			call(0x0850, sp, 0x0845);
			check(c_frames_live(0x01FF) == 0, "a JSR at the page edge records nothing, keeps nothing");
		}
		fresh();
		call(0x0840, 0x01F0, 0x0805);
		call(0x0850, 0x0103, 0x0845);
		check(c_frames_live(0x0101) == 2, "three bytes above the edge is ordinary");

		fresh();
		call(0x0840, 0x01F0, 0x0805);
		rts(0x0101);
		check(c_frames_live(0x0101) == 0, "a pull that wraps drops every record");

		fresh();
		call(0x0840, 0x01F0, 0x0805);
		c_frames_flow(CPU_FLOW_CALL_LONG, 0x0850, 0x0102, 0x0845);
		check(c_frames_live(0x01FF) == 0, "JSL at the edge");

		fresh();
		call(0x0840, 0x01F0, 0x0805);
		c_frames_flow(CPU_FLOW_IRQ_ENTER, 0x0C00, 0x0100, 0xFFFE);
		check(c_frames_live(0x01FD) == 0, "interrupt at the edge");
	}

	// ── Bounds ──────────────────────────────────────────────────────────────
	{
		fresh();
		c_frames_flow(CPU_FLOW_CALL, 0x010840, 0x01F0, 0x0805);
		check(c_frames_live(0x01EE) == 0, "a target in another program bank is not recorded");

		fresh();
		uint16_t sp = 0x7FFC;
		for (int i = 0; i < C_FRAMES_MAX + 8; i++) {
			if ((sp & 0xFF) < 3)
				sp = (uint16_t)(sp - 4);
			call(0x0840, sp, (uint16_t)(0x0805 + i));
			sp = (uint16_t)(sp - 2);
		}
		check(c_frames_live(sp) == C_FRAMES_MAX, "the table holds its maximum");
		check(c_frames_at_slot(sp, 0x7FFB, NULL), "and keeps the oldest");

		fresh();
		call(0x0840, 0x01F0, 0x0805);
		c_frames_clear();
		check(c_frames_live(0x01EE) == 0, "clear drops every record");
		call(0x0840, 0x01F0, 0x0805);
		const c_frames_config_t again = {CSP_ADDR, CALLAX, mem, &reg_e, &reg_dp, fake_read, fake_ax};
		c_frames_configure(&again);
		check(c_frames_live(0x01EE) == 0, "so does configure");
	}

	// ── Finding the symbols ─────────────────────────────────────────────────
	{
		uint16_t at = 0;
		int      cx = 0;
		static const fake_sym_t both[] = {{"sp", 0x30}, {"c_sp", 0x22}, {"callax", 0x0900}};
		check(pick(both, 3, &at, &cx) && at == 0x22 && cx == 0x0900, "c_sp preferred over sp");

		static const fake_sym_t old[] = {{"sp", 0x30}};
		check(pick(old, 1, &at, &cx) && at == 0x30 && cx == -1, "sp when there is no c_sp; callax optional");

		static const fake_sym_t none[] = {{"_main", 0x0840}};
		check(!pick(none, 1, &at, &cx), "no stack pointer");
		check(!pick(none, 0, &at, &cx), "no symbols");

		static const fake_sym_t cased[] = {{"C_SP", 0x22}, {"SP", 0x24}};
		check(!pick(cased, 2, &at, &cx), "names are exact");

		static const fake_sym_t absolute[] = {{"c_sp", 0x0822}, {"sp", 0x30}};
		check(pick(absolute, 2, &at, &cx) && at == 0x30, "a c_sp outside the zero page is not it");

		static const fake_sym_t edge[] = {{"c_sp", 0xFF}};
		check(!pick(edge, 1, &at, &cx), "both bytes must be in the zero page");

		static const fake_sym_t same[] = {{"c_sp", 0x22}, {"c_sp", 0x22}};
		check(pick(same, 2, &at, &cx) && at == 0x22, "files that agree");

		static const fake_sym_t differ[] = {{"c_sp", 0x22}, {"c_sp", 0x24}, {"sp", 0x30}};
		check(!pick(differ, 3, &at, &cx), "files that disagree are refused, sp is no fallback");

		static const fake_sym_t two_callax[] = {{"c_sp", 0x22}, {"callax", 0x0900}, {"callax", 0x0910}};
		check(pick(two_callax, 3, &at, &cx) && cx == -1, "an ambiguous callax is unknown");
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
