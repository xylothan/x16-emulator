// Checks for interrupt context tracking (src/cpu/irq_ctx.c).
//
// The CPU core reports an entry from interrupt6502() and a leave from rti(),
// and the two are paired by the stack pointer. That pairing is the whole
// design, and it has to survive things a real program does: nested handlers, a
// handler that returns further than its own frame, an RTI with no matching
// entry, and a reset in the middle of one. Getting it wrong leaves the debugger
// permanently convinced the machine is inside an interrupt, or never in one.
//
// The 6502 stack grows DOWN, so a deeper frame has a LOWER stack pointer, and
// an interrupt entry records the pointer BEFORE anything is pushed -- which is
// exactly what its matching RTI restores.

#include "cpu/irq_ctx.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

// The vector values the CPU core passes through; their exact numbering does not
// matter here, only that it is reported back unchanged.
#define VEC_IRQ 0
#define VEC_NMI 1
#define VEC_BRK 2

// CPU_IRQ_CTX_MAX comes from the header rather than being restated here: a
// private copy that drifted from the real limit would leave the boundary checks
// below passing while no longer testing the boundary.

int
main(void)
{
	// ── Nothing has happened yet ────────────────────────────────────────────
	{
		cpu_irq_ctx_reset();
		check(!cpu_in_interrupt(), "starts outside any interrupt");
		check(cpu_irq_depth() == 0, "at depth zero");
		check(cpu_irq_count() == 0, "with nothing counted");
		check(cpu_irq_return_pc() == 0, "and no return address to report");
		check(!cpu_irq_take_entered_flag(), "and nothing just entered");
	}

	// ── One interrupt, taken and returned from ──────────────────────────────
	{
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1234, 0x01FD);

		check(cpu_in_interrupt(), "reports being inside a handler");
		check(cpu_irq_depth() == 1, "at depth one");
		check(cpu_irq_count() == 1, "counts the interrupt");
		check(cpu_irq_last_vector() == VEC_IRQ, "remembers which vector");
		check(cpu_irq_return_pc() == 0x1234, "reports where it will return to");

		// A balanced RTI restores the stack pointer to the entry value.
		cpu_irq_ctx_leave(0x01FD);
		check(!cpu_in_interrupt(), "reports leaving the handler");
		check(cpu_irq_depth() == 0, "back to depth zero");
		check(cpu_irq_count() == 1, "the count survives the return");
		check(cpu_irq_return_pc() == 0, "and there is no return address again");
	}

	// ── The just-entered flag is a one-shot ─────────────────────────────────
	// A debugger uses this to notice "an interrupt was just taken" while
	// stepping. It has to be true exactly once, or every poll would look like a
	// fresh interrupt.
	{
		cpu_irq_ctx_reset();
		check(!cpu_irq_take_entered_flag(), "no entry reported before one happens");
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x01FD);
		check(cpu_irq_take_entered_flag(), "reports the entry once");
		check(!cpu_irq_take_entered_flag(), "and only once");

		// A second interrupt sets it again.
		cpu_irq_ctx_enter(VEC_NMI, 0x2000, 0x01FA);
		check(cpu_irq_take_entered_flag(), "a second entry is reported too");
	}

	// ── Nesting ─────────────────────────────────────────────────────────────
	// An NMI can interrupt an IRQ handler, and a handler that re-enables
	// interrupts can be interrupted again. Each RTI must unwind exactly one.
	{
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x01FD);   // outer
		cpu_irq_ctx_enter(VEC_NMI, 0x2000, 0x01FA);   // inner, deeper stack

		check(cpu_irq_depth() == 2, "counts nested handlers");
		check(cpu_irq_count() == 2, "counts both interrupts");
		check(cpu_irq_last_vector() == VEC_NMI, "reports the innermost vector");
		check(cpu_irq_return_pc() == 0x2000, "reports the innermost return address");

		cpu_irq_ctx_leave(0x01FA);                    // inner RTI
		check(cpu_irq_depth() == 1, "unwinds one level per return");
		check(cpu_in_interrupt(), "still inside the outer handler");
		check(cpu_irq_return_pc() == 0x1000, "and reports its return address again");

		cpu_irq_ctx_leave(0x01FD);                    // outer RTI
		check(cpu_irq_depth() == 0, "unwinds the outer handler too");
		check(!cpu_in_interrupt(), "and is outside any interrupt");
	}

	// ── Returning past more than one frame ──────────────────────────────────
	// A handler that unwinds several frames at once, or discards them, must not
	// leave the depth stuck high for the rest of the session.
	{
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x01FD);
		cpu_irq_ctx_enter(VEC_IRQ, 0x2000, 0x01FA);
		cpu_irq_ctx_enter(VEC_IRQ, 0x3000, 0x01F7);
		check(cpu_irq_depth() == 3, "three handlers open");

		// One RTI that restores the stack all the way back to the outermost
		// entry: every frame it returned past is finished.
		cpu_irq_ctx_leave(0x01FD);
		check(cpu_irq_depth() == 0, "a return past every frame unwinds them all");
		check(!cpu_in_interrupt(), "and leaves the machine outside any interrupt");
	}

	// ── An RTI with nothing to match ────────────────────────────────────────
	// A program can RTI without us having seen the entry -- an interrupt
	// already in flight at startup, or a hand-rolled stack frame. That is not
	// an error, and must not drive the depth negative.
	{
		cpu_irq_ctx_reset();
		cpu_irq_ctx_leave(0x01FD);
		check(cpu_irq_depth() == 0, "an unmatched return leaves depth at zero");
		check(!cpu_in_interrupt(), "and does not report being in a handler");

		cpu_irq_ctx_leave(0x01FD);
		cpu_irq_ctx_leave(0x01FD);
		check(cpu_irq_depth() == 0, "repeated unmatched returns stay at zero");

		// And the tracker still works afterwards.
		cpu_irq_ctx_enter(VEC_IRQ, 0x4000, 0x01FD);
		check(cpu_irq_depth() == 1 && cpu_irq_return_pc() == 0x4000,
		      "tracking still works after an unmatched return");
	}

	// ── Reset abandons anything in flight ───────────────────────────────────
	{
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x01FD);
		cpu_irq_ctx_enter(VEC_NMI, 0x2000, 0x01FA);
		cpu_irq_ctx_reset();

		check(cpu_irq_depth() == 0, "reset abandons open handlers");
		check(!cpu_in_interrupt(), "and reports being outside one");
		check(cpu_irq_count() == 0, "and clears the count");
		check(cpu_irq_return_pc() == 0, "and the return address");
		check(!cpu_irq_take_entered_flag(), "and the just-entered flag");
	}

	// ── Deeper nesting than the tracker records detail for ──────────────────
	// Past its limit the depth still has to count correctly and come back down
	// to zero; only the per-frame detail is allowed to go missing.
	{
		cpu_irq_ctx_reset();
		uint16_t sp = 0x01FD;
		for (int i = 0; i < 40; i++) {
			cpu_irq_ctx_enter(VEC_IRQ, (uint16_t)(0x1000 + i), sp);
			sp = (uint16_t)(sp - 3);
		}
		check(cpu_irq_depth() == 40, "counts deeper nesting than it records");
		check(cpu_irq_count() == 40, "and counts every interrupt");

		// Unwind them one balanced return at a time.
		for (int i = 39; i >= 0; i--) {
			sp = (uint16_t)(sp + 3);
			cpu_irq_ctx_leave(sp);
		}
		check(cpu_irq_depth() == 0, "and unwinds all the way back out");
		check(!cpu_in_interrupt(), "ending outside any interrupt");

		// Just past the recorded frames, an unbalanced return must still pop
		// exactly one level rather than cascading on detail it never kept.
		cpu_irq_ctx_reset();
		sp = 0x01FD;
		for (int i = 0; i < CPU_IRQ_CTX_MAX + 1; i++) {
			cpu_irq_ctx_enter(VEC_IRQ, (uint16_t)(0x1000 + i), sp);
			sp = (uint16_t)(sp - 3);
		}
		check(cpu_irq_depth() == CPU_IRQ_CTX_MAX + 1, "opens one more than it records");
		// An sp far above every recorded frame: were the unrecorded level to
		// consult the frames it does not have, this would cascade.
		cpu_irq_ctx_leave(0x01FD);
		check(cpu_irq_depth() == CPU_IRQ_CTX_MAX,
		      "an unrecorded level pops exactly one, without cascading");
	}

	// ── A stack that wraps is out of contract ───────────────────────────────
	// Both rules here read the stack pointer: frames must decrease as nesting
	// deepens, and a balanced RTI lands exactly on the pointer its entry
	// recorded. A guest that pushes past the bottom of page 1 breaks the first
	// one -- an outer frame near the bottom ends up numerically ABOVE the frame
	// nested inside it, and is indistinguishable from a frame the stack has
	// legitimately risen back past.
	//
	// Nothing based on the stack pointer alone can tell those apart, so this
	// picks the case that actually happens. Overflowing a 256-byte stack has
	// already destroyed the guest's own return addresses; a BRK reaching a warm
	// start happens in every debugging session. What is pinned here is that the
	// wrapped case stays bounded and recovers at the next clean interrupt,
	// rather than corrupting anything or sticking forever.
	{
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x0101);   // outer, near the bottom
		cpu_irq_ctx_enter(VEC_NMI, 0x2000, 0x01FE);   // pushes wrapped it round

		check(cpu_irq_depth() >= 1, "a wrapped stack still reports an interrupt");
		check(cpu_irq_depth() <= 2, "and does not invent depth");

		cpu_irq_ctx_leave(0x01FE);
		check(cpu_irq_depth() >= 0, "and unwinds without going negative");

		// The tracker is usable again as soon as the guest stops wrapping.
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x01FD);
		cpu_irq_ctx_leave(0x01FD);
		check(cpu_irq_depth() == 0, "and recovers completely on the next clean pair");
	}

	// ── Nesting that does not wrap is exact ─────────────────────────────────
	// The ordinary case, which is what the rules are for: each nested frame
	// sits below the one outside it, and a balanced return unwinds exactly one.
	{
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x01FD);
		cpu_irq_ctx_enter(VEC_NMI, 0x2000, 0x01F9);
		check(cpu_irq_depth() == 2, "nested handlers count two");

		cpu_irq_ctx_leave(0x01F9);
		check(cpu_irq_depth() == 1, "a balanced inner return unwinds only its own");
		check(cpu_irq_return_pc() == 0x1000, "and the outer frame is reported again");

		cpu_irq_ctx_leave(0x01FD);
		check(cpu_irq_depth() == 0, "and the outer return closes it");
	}

	// ── A return past several frames unwinds them all ───────────────────────
	{
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x01FD);
		cpu_irq_ctx_enter(VEC_NMI, 0x2000, 0x01F9);
		cpu_irq_ctx_enter(VEC_BRK, 0x3000, 0x01F5);
		cpu_irq_ctx_leave(0x01FD);                    // straight back to the outermost
		check(cpu_irq_depth() == 0,
		      "a return to an outer frame's own entry unwinds everything inside it");
	}
	// ── The count is cumulative ─────────────────────────────────────────────
	{
		cpu_irq_ctx_reset();
		for (int i = 0; i < 100; i++) {
			cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x01FD);
			cpu_irq_ctx_leave(0x01FD);
		}
		check(cpu_irq_count() == 100, "counts every interrupt taken, not just open ones");
		check(cpu_irq_depth() == 0, "while ending balanced");
	}

	// ── Each vector is reported back unchanged ──────────────────────────────
	{
		cpu_irq_ctx_reset();
		const int vectors[] = { VEC_IRQ, VEC_NMI, VEC_BRK };
		bool ok = true;
		for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
			cpu_irq_ctx_enter(vectors[i], 0x1000, 0x01FD);
			if (cpu_irq_last_vector() != vectors[i])
				ok = false;
			cpu_irq_ctx_leave(0x01FD);
		}
		check(ok, "reports back whichever vector was taken");
	}

	// ── A frame abandoned without an RTI ────────────────────────────────────
	// A BRK reaching a warm start resets the stack instead of returning, so its
	// entry never gets a matching RTI. Later interrupts must still be tracked
	// correctly rather than being swallowed by the stale frame, and the depth
	// must read HIGH rather than low: too high corrects itself at the next
	// matching return, too low claims nothing is running when something is.
	{
		cpu_irq_ctx_reset();

		cpu_irq_ctx_enter(VEC_BRK, 0x4000, 0x01F0);   // BRK, never returns
		// Warm start resets the stack to the top; an ordinary IRQ then arrives.
		cpu_irq_ctx_enter(VEC_IRQ, 0x5000, 0x01FD);

		// Taking that IRQ is what retires the abandoned BRK frame: the stack
		// had risen back above it, which proves it was gone.
		check(cpu_irq_depth() == 1, "an abandoned frame is retired, not stacked");

		cpu_irq_ctx_leave(0x01FD);                    // balanced IRQ return
		check(cpu_irq_depth() == 0,
		      "so the depth comes back to zero once the IRQ returns");

		// And it does not accumulate: a second BRK that never returns must not
		// leave the machine permanently looking like it is inside an interrupt.
		cpu_irq_ctx_enter(VEC_BRK, 0x4100, 0x01F0);
		cpu_irq_ctx_enter(VEC_IRQ, 0x5100, 0x01FD);
		cpu_irq_ctx_leave(0x01FD);
		check(cpu_irq_depth() == 0, "however many times it happens");
	}

	// ── The program bank is part of the return address ──────────────────────
	// Native-mode interrupts push the program bank, and the CPU zeroes it on
	// entry, so a handler cannot recover it. Recording only the low 16 bits
	// would send a step-over to the right offset in the wrong bank.
	{
		cpu_irq_ctx_reset();

		cpu_irq_ctx_enter(VEC_IRQ, 0x123456, 0x01FD);
		check(cpu_irq_return_pc() == 0x123456,
		      "the return address keeps its program bank");

		cpu_irq_ctx_enter(VEC_NMI, 0x008000, 0x01F9);
		check(cpu_irq_return_pc() == 0x008000,
		      "and a bank-zero address is unchanged");

		cpu_irq_ctx_leave(0x01F9);
		check(cpu_irq_return_pc() == 0x123456,
		      "and the outer one is restored when the inner returns");
	}

	// ── Runaway nesting stays safe ──────────────────────────────────────────
	// A BRK handler that BRKs never returns. Each entry has to sit strictly
	// below the last to count as nested, so the retirement rule already bounds
	// how deep this can go -- the stack pointer runs out of room. The explicit
	// saturation in the code is there so that a signed counter can never
	// overflow, which would be undefined behaviour rather than a wrong number.
	{
		cpu_irq_ctx_reset();

		// Walk the pointer all the way down, one byte per entry.
		for (int sp = 0xFFFF; sp >= 0; sp--)
			cpu_irq_ctx_enter(VEC_BRK, 0x6000, (uint16_t)sp);

		const int deep = cpu_irq_depth();
		check(deep > CPU_IRQ_CTX_MAX,
		      "nesting can exceed the frames it records detail for");
		check(deep <= 0x10000, "but the depth stays bounded");
		check(cpu_in_interrupt(), "and still reports an interrupt");

		// No detail is kept past the array, so those come off one at a time;
		// the recorded ones then close together when the outermost returns.
		for (int i = 0; i < deep - CPU_IRQ_CTX_MAX; i++)
			cpu_irq_ctx_leave(0x0000);
		check(cpu_irq_depth() == CPU_IRQ_CTX_MAX,
		      "undetailed levels come off one at a time");

		cpu_irq_ctx_leave(0xFFFF);   // back to the outermost frame's own entry
		check(cpu_irq_depth() == 0, "and the outermost return closes the rest");
	}
	// ── A frame abandoned at the same stack level ───────────────────────────
	// Two interrupts taken at the identical pointer cannot both be live: the
	// stack being back at that level means the first one's frame is gone. This
	// is the boundary of the retirement rule, and it has to be inclusive.
	{
		cpu_irq_ctx_reset();

		cpu_irq_ctx_enter(VEC_BRK, 0x7000, 0x01FD);   // abandoned
		cpu_irq_ctx_enter(VEC_IRQ, 0x7100, 0x01FD);   // same level again
		check(cpu_irq_depth() == 1,
		      "an interrupt at the same level retires the one before it");

		cpu_irq_ctx_leave(0x01FD);
		check(cpu_irq_depth() == 0, "and returns cleanly to zero");
	}

	// ── A return that matches nothing changes nothing ───────────────────────
	// A handler can push a fabricated frame and RTI to itself, landing on a
	// pointer no entry recorded, while the interrupt it is running inside is
	// still live. Popping on that would report no interrupt during one -- the
	// one answer a debugger must never give. Frames only close when something
	// proves they have: a return that lands exactly on them, or a later
	// interrupt taken at or above them.
	{
		cpu_irq_ctx_reset();

		cpu_irq_ctx_enter(VEC_IRQ, 0x8000, 0x01FD);
		cpu_irq_ctx_leave(0x01FA);                    // matches no recorded frame
		check(cpu_irq_depth() == 1,
		      "a return matching no frame leaves the depth alone");
		check(cpu_in_interrupt(),
		      "so a live handler is never reported as finished");

		cpu_irq_ctx_leave(0x01FD);                    // the real balanced return
		check(cpu_irq_depth() == 0, "and the real return still closes it");
	}

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
