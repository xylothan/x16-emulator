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

// Matches the limit in irq_ctx.c: past this the depth still counts, but no
// per-frame detail is recorded.
#define CPU_IRQ_CTX_MAX 16

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

	// ── Stack pointers that wrap ────────────────────────────────────────────
	// The stack wraps -- within page 1 in emulation mode, and at 16 bits in
	// native mode -- so a frame entered near the bottom has a numerically LOWER
	// pointer than the one nested inside it. Comparing absolutely would read a
	// balanced inner return as having unwound past the outer frame and collapse
	// the whole depth.
	{
		// Emulation mode: outer entered at $0101, its three pushes wrap the
		// pointer round to $01FE, where the inner one is entered.
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x0101);
		cpu_irq_ctx_enter(VEC_NMI, 0x2000, 0x01FE);
		cpu_irq_ctx_leave(0x01FE);                    // balanced inner RTI
		check(cpu_irq_depth() == 1,
		      "a balanced return across a page-1 wrap unwinds only its own frame");
		check(cpu_irq_return_pc() == 0x1000, "and the outer frame is still reported");

		// Native mode: the same shape at the 16-bit boundary.
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x0002);
		cpu_irq_ctx_enter(VEC_NMI, 0x2000, 0xFFFE);
		cpu_irq_ctx_leave(0xFFFE);
		check(cpu_irq_depth() == 1,
		      "a balanced return across the 16-bit wrap unwinds only its own frame");

		// The cascade must still work when the wrap is in the other direction:
		// a return that genuinely unwinds past a wrapped outer frame.
		cpu_irq_ctx_reset();
		cpu_irq_ctx_enter(VEC_IRQ, 0x1000, 0x0101);
		cpu_irq_ctx_enter(VEC_NMI, 0x2000, 0x01FE);
		cpu_irq_ctx_leave(0x0101);                    // back to the outer entry
		check(cpu_irq_depth() == 0,
		      "a return to the outer frame's own entry still unwinds both");
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

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
