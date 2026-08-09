// Commander X16 Emulator — interrupt context tracking.
//
// The CPU core has no notion of "currently servicing an interrupt": it vectors
// through, and the only trace is a return address on the stack. A debugger
// needs more than that. It wants to say whether you are inside a handler and
// how deep, to stop at the first instruction of one instead of stepping over
// the whole thing invisibly, and to show where the handler will return to.
//
// interrupt6502() reports an entry and rti() reports a leave; the two are
// paired by matching the stack pointer at entry exactly, which keeps nested
// handlers straight and unwinds several at once when a handler returns past
// its own frame.
//
// A handler that leaves by some other route -- discarding its frame and exiting
// through RTS or JMP, or a BRK that lands in a warm start which resets the
// stack -- has no matching RTI. Those entries are retired the next time an
// interrupt is taken at or above where they were recorded, since the stack
// having risen back past a frame proves it is gone. Until then the depth reads
// high rather than low, which is the safe direction: claiming no interrupt is
// running while one is would be worse than being stale.
//
// What this cannot survive is the stack pointer moving for a reason other than
// nesting. Both rules -- frames decreasing in sp, and a balanced RTI landing
// exactly on the recorded pointer -- read position as evidence of depth. A
// guest that pushes past the bottom of page 1 wraps, and a handler that
// relocates the stack with TXS or TCS moves it outright; either can make a live
// outer frame look like one the stack has passed, and it will be retired.
//
// No rule based on the stack pointer alone can tell those apart. What decides
// it is not which case is commoner but how each fails: retiring wrongly costs
// the remaining lifetime of one handler, because the next interrupt taken at a
// sane stack level is tracked correctly again, whereas never retiring leaves a
// frame open for the rest of the session and adds another every time it
// happens. A bounded, self-healing error beats an unbounded permanent one.
//
// A consumer that acts on cpu_irq_return_pc() -- setting a breakpoint there to
// step over a handler, say -- therefore wants an explicit validity signal, so
// it can fall back to single-stepping rather than trusting a stale address.
// That belongs with the consumer, which does not exist yet.
//
// Kept apart from fake6502.c so that the CPU core carries only the two calls,
// and so the bookkeeping can be tested without standing up a machine.

#ifndef _CPU_IRQ_CTX_H_
#define _CPU_IRQ_CTX_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// How many nested handlers keep per-frame detail. Deep enough for any plausible
// nesting: the X16 has IRQ, NMI and BRK, and a handler that re-enables
// interrupts can nest a few more. Past this the depth still counts, but the
// per-frame detail stops being recorded. Exposed so tests cannot drift from it.
#define CPU_IRQ_CTX_MAX 16

// ---- Reported by the CPU core ----------------------------------------------
// `vector` is the enum InterruptType that got us here, `from_pc` the 24-bit PC
// that was interrupted -- the program bank is part of it on the 65816, and is
// zeroed on entry, so it cannot be recovered later -- and `sp` the stack
// pointer BEFORE the interrupt pushed anything, which is what the matching RTI
// will restore it to.
void cpu_irq_ctx_enter(int vector, uint32_t from_pc, uint16_t sp);
void cpu_irq_ctx_leave(uint16_t sp);
void cpu_irq_ctx_reset(void);

// ---- Asked by the debugger -------------------------------------------------
bool     cpu_in_interrupt(void);
int      cpu_irq_depth(void);      // nested handlers currently open
uint32_t cpu_irq_count(void);      // interrupts taken since reset
int      cpu_irq_last_vector(void);
uint32_t cpu_irq_return_pc(void);  // where the innermost handler returns to (24-bit)

// True once per interrupt entry; reading it clears the flag. Lets a debugger
// notice "an interrupt was just taken" without polling the depth and having to
// work out whether it changed.
bool cpu_irq_take_entered_flag(void);

#ifdef __cplusplus
}
#endif

#endif // _CPU_IRQ_CTX_H_
