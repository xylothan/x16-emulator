// Commander X16 Emulator — interrupt context tracking.
//
// The CPU core has no notion of "currently servicing an interrupt": it vectors
// through, and the only trace is a return address on the stack. A debugger
// needs more than that. It wants to say whether you are inside a handler and
// how deep, to stop at the first instruction of one instead of stepping over
// the whole thing invisibly, and to show where the handler will return to.
//
// interrupt6502() reports an entry and rti() reports a leave; the two are
// paired by the stack pointer at entry, which keeps nested handlers straight
// and lets the depth recover when a handler exits by some route other than a
// balanced RTI.
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

// ---- Reported by the CPU core ----------------------------------------------
// `vector` is the enum InterruptType that got us here, `from_pc` the PC that
// was interrupted, and `sp` the stack pointer BEFORE the interrupt pushed
// anything -- which is what the matching RTI will restore it to.
void cpu_irq_ctx_enter(int vector, uint16_t from_pc, uint16_t sp);
void cpu_irq_ctx_leave(uint16_t sp);
void cpu_irq_ctx_reset(void);

// ---- Asked by the debugger -------------------------------------------------
bool     cpu_in_interrupt(void);
int      cpu_irq_depth(void);      // nested handlers currently open
uint32_t cpu_irq_count(void);      // interrupts taken since reset
int      cpu_irq_last_vector(void);
uint16_t cpu_irq_return_pc(void);  // where the innermost handler returns to

// True once per interrupt entry; reading it clears the flag. Lets a debugger
// notice "an interrupt was just taken" without polling the depth and having to
// work out whether it changed.
bool cpu_irq_take_entered_flag(void);

#ifdef __cplusplus
}
#endif

#endif // _CPU_IRQ_CTX_H_
