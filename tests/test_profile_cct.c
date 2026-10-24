// Checks for the calling-context tree (src/profile_cct.c).
//
// A toy CPU drives it with the same event order the real core produces: a
// call or return is reported during its instruction and the instruction's
// cycles follow; an interrupt is reported at once. Expected numbers are worked
// out by hand next to each scenario.

#include "profile_cct.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>

enum { K_A = 0x1000, K_B = 0x2000, K_C = 0x3000, K_H = 0xE000 };

static uint16_t sp;
static uint64_t fed;

static void start(void)  { cct_reset(); sp = 0xFF; fed = 0; }
static void work(uint32_t c) { cct_step(c); fed += c; }
static void jsr(uint32_t key, uint32_t c) { cct_call(key, sp); sp -= 2; work(c); }
static void rts(uint32_t c) { sp += 2; cct_return(sp); work(c); }
static void irq(uint32_t key) { cct_irq_enter(key, sp); sp -= 3; }
static void brk(uint32_t key, uint32_t c) { cct_irq_enter(key, sp); sp -= 3; work(c); }
static void rti(uint32_t c) { sp += 3; cct_irq_leave(sp); work(c); }

// The node for a path of keys under main (or under [IRQ] when irq is set).
static int
find(bool under_irq, int n, const uint32_t *path)
{
	int parent = under_irq ? CCT_NODE_IRQ : CCT_NODE_MAIN;
	for (int d = 0; d < n; d++) {
		int found = -1;
		for (int i = 0; i < cct_node_count(); i++) {
			cct_node_t nd;
			cct_node_get(i, &nd);
			if (nd.parent == parent && nd.key == path[d])
				found = i;
		}
		if (found < 0)
			return -1;
		parent = found;
	}
	return parent;
}

static cct_node_t
node(int i)
{
	cct_node_t n;
	memset(&n, 0, sizeof n);
	cct_node_get(i, &n);
	return n;
}

static uint64_t
self_sum(void)
{
	uint64_t s = 0;
	for (int i = 0; i < cct_node_count(); i++)
		s += node(i).self;
	return s;
}

int
main(void)
{
	check(cct_init(4096), "init");

	// ── Simple nesting: main -> A -> B ──────────────────────────────────────
	{
		start();
		work(10);
		jsr(K_A, 6);   // JSR's cycles: main
		work(20);
		jsr(K_B, 6);   // A
		work(30);
		rts(6);        // RTS's cycles: B
		work(5);
		rts(6);        // A
		work(7);
		cct_finish();
		const uint32_t pa[] = {K_A}, pab[] = {K_A, K_B};
		const int a = find(false, 1, pa), b = find(false, 2, pab);
		check(a > 1 && b > 1, "main/A and main/A/B exist");
		check_eq((uint32_t)node(CCT_NODE_MAIN).self, 23, "main self = 10 + 6 + 7");
		check_eq((uint32_t)node(a).self, 37, "A self = 20 + 6 + 5 + 6");
		check_eq((uint32_t)node(b).self, 36, "B self = 30 + 6");
		check_eq((uint32_t)node(b).incl, 36, "B incl = self");
		check_eq((uint32_t)node(a).incl, 73, "A incl = 37 + 36");
		check_eq((uint32_t)node(CCT_NODE_MAIN).incl, 96, "main incl = everything");
		check_eq(node(a).calls, 1, "A called once");
		check_eq(node(a).call_max, 73, "A's one call = its inclusive");
		check_eq((uint32_t)self_sum(), (uint32_t)fed, "self sums to every cycle fed");
		check_eq(cct_depth(), 0, "balanced");
	}

	// ── Same path twice: one node; worst call and worst frame ───────────────
	{
		start();
		jsr(K_A, 6); work(10); rts(6);   // call 1: 16
		jsr(K_A, 6); work(30); rts(6);   // call 2: 36, same frame
		cct_close_frame();
		jsr(K_A, 6); work(40); rts(6);   // call 3: 46, next frame
		cct_finish();
		const uint32_t pa[] = {K_A};
		const int a = find(false, 1, pa);
		check_eq(node(a).calls, 3, "three calls, one node");
		check_eq(cct_node_count(), 3, "no duplicate nodes");
		check_eq(node(a).call_max, 46, "worst call");
		check_eq(node(a).frame_max, 52, "worst frame: 16 + 36 in frame 0 beats 46");
	}

	// ── PLA/PLA early exit: B drops its return and returns from A ───────────
	{
		start();
		jsr(K_A, 6);
		jsr(K_B, 6);
		sp += 2; work(4);   // PLA PLA
		rts(6);             // lands where A's caller is waiting
		work(3);
		check_eq(cct_depth(), 0, "both frames closed");
		check_eq(cct_current(), CCT_NODE_MAIN, "back in main");
		cct_finish();
		const uint32_t pa[] = {K_A}, pab[] = {K_A, K_B};
		check_eq(node(find(false, 1, pa)).calls, 1, "A closed once");
		check_eq(node(find(false, 2, pab)).calls, 1, "B closed once");
		check_eq((uint32_t)node(find(false, 2, pab)).self, 10, "PLA PLA and RTS are B's");
	}

	// ── RTS as a computed jump (jump table) ─────────────────────────────────
	{
		start();
		jsr(K_A, 6);
		sp -= 2; work(6);   // push target - 1
		rts(6);             // "returns" into the table entry: still inside A
		check_eq(cct_depth(), 1, "computed RTS pops nothing");
		const uint32_t pa[] = {K_A};
		check_eq(cct_current(), find(false, 1, pa), "still in A");
		work(9);
		rts(6);             // A's real return
		check_eq(cct_depth(), 0, "the real RTS closes A");
		cct_finish();
		check_eq((uint32_t)node(find(false, 1, pa)).self, 27, "A self = 6 + 6 + 9 + 6");
	}

	// ── IRQ in mid-routine: its own root; excluded from A ───────────────────
	{
		start();
		jsr(K_A, 6);
		work(10);
		irq(K_H);
		work(40);           // the handler
		rti(6);             // RTI's cycles: back in A
		work(5);
		rts(6);
		cct_finish();
		const uint32_t pa[] = {K_A}, ph[] = {K_H};
		const int a = find(false, 1, pa), h = find(true, 1, ph);
		check(h > 1, "handler is under [IRQ]");
		check_eq((uint32_t)node(h).self, 40, "handler self");
		check_eq((uint32_t)node(CCT_NODE_IRQ).incl, 40, "[IRQ] holds all interrupt time");
		check_eq((uint32_t)node(a).self, 27, "A self = 10 + 6 + 5 + 6, no IRQ");
		check_eq(node(a).call_max, 27, "A's call excludes the interrupt");
		check_eq(node(h).calls, 1, "one interrupt");
		check_eq((uint32_t)self_sum(), (uint32_t)fed, "nothing lost");
	}

	// ── A call inside a handler nests under it ──────────────────────────────
	{
		start();
		irq(K_H);
		work(3);
		jsr(K_B, 6);
		work(10);
		rts(6);
		rti(6);
		cct_finish();
		const uint32_t phb[] = {K_H, K_B};
		check_eq((uint32_t)node(find(true, 2, phb)).self, 16, "[IRQ]/H/B");
		check_eq((uint32_t)node(CCT_NODE_IRQ).incl, 25, "3 + 6 + 16");
		check_eq((uint32_t)node(CCT_NODE_MAIN).self, 6, "RTI back in main");
	}

	// ── BRK: reported inside its own instruction, so its cycles are the handler's
	{
		start();
		brk(K_H, 7);
		work(20);
		rti(6);
		cct_finish();
		const uint32_t ph[] = {K_H};
		check_eq((uint32_t)node(find(true, 1, ph)).self, 27, "BRK's 7 + 20");
	}

	// ── TXS reset / longjmp: stale frames retire at the next call above them ─
	{
		start();
		jsr(K_A, 6);
		jsr(K_B, 6);
		sp = 0xFF; work(2); // TXS: back to the top of the stack
		jsr(K_C, 6);        // at the same level main called A from
		check_eq(cct_depth(), 1, "A and B retired, C open");
		const uint32_t pc[] = {K_C};
		check_eq(cct_current(), find(false, 1, pc), "C is a child of main, not of B");
	}

	// ── Recursion: each level is its own node ───────────────────────────────
	{
		start();
		jsr(K_A, 1);
		jsr(K_A, 1);
		jsr(K_A, 1);
		rts(1); rts(1); rts(1);
		cct_finish();
		const uint32_t p3[] = {K_A, K_A, K_A};
		check(find(false, 3, p3) > 1, "main/A/A/A");
		check_eq(cct_node_count(), 5, "three levels plus two roots");
	}

	// ── A return with nothing open, as at the start of a capture ────────────
	{
		start();
		sp = 0xF0;
		rts(6);
		check_eq(cct_depth(), 0, "nothing to pop");
		check_eq(cct_current(), CCT_NODE_MAIN, "stays in main");
	}

	// ── A call that never returns is closed at the end ──────────────────────
	{
		start();
		jsr(K_A, 6);
		work(100);
		cct_finish();
		const uint32_t pa[] = {K_A};
		check_eq(node(find(false, 1, pa)).calls, 1, "closed by finish");
		check_eq(node(find(false, 1, pa)).call_max, 100, "with its cycles so far");
	}

	// ── The pool filling: new paths fold into [other] ───────────────────────
	{
		check(cct_init(64), "small pool");
		start();
		for (uint32_t k = 0; k < 100; k++) {
			jsr(0x100 + k, 1);
			rts(1);
		}
		cct_finish();
		check(cct_overflowed(), "overflow is reported");
		check(cct_node_count() <= 64, "never past the pool");
		const uint32_t po[] = {CCT_KEY_OTHER};
		const int o = find(false, 1, po);
		check(o > 1, "an [other] under main");
		check(node(o).calls > 0, "holding the calls that did not fit");
		check_eq((uint32_t)self_sum(), (uint32_t)fed, "no cycles lost to overflow");
	}

	cct_free();
	return x16_test_summary("profile_cct");
}
