// Commander X16 Emulator — interrupt context tracking. See irq_ctx.h.

#include "irq_ctx.h"

#include <string.h>

// Deep enough for any plausible nesting: the X16 has IRQ, NMI and BRK, and a
// handler that re-enables interrupts can nest a few more. Past this the depth
// still counts correctly; only the per-frame detail stops being recorded.
#define CPU_IRQ_CTX_MAX 16

// How far the stack may have unwound and still count as "past" the next frame
// down. An interrupt frame is 3 bytes (4 on the 65C816 in native mode), so the
// deepest nest we record spans 64; anything beyond that is a wrap or a handler
// that abandoned the stack wholesale, neither of which should cascade.
#define CPU_IRQ_UNWIND_WINDOW (CPU_IRQ_CTX_MAX * 8)

struct irq_frame {
	uint8_t  vector;
	uint16_t from_pc;
	uint16_t sp;      // stack pointer before the interrupt pushed anything
};

static struct irq_frame frames[CPU_IRQ_CTX_MAX];
static int              depth;
static uint32_t         taken;
static uint8_t          last_vector;
static bool             just_entered;

void
cpu_irq_ctx_enter(int vector, uint16_t from_pc, uint16_t sp)
{
	taken++;
	last_vector  = (uint8_t)vector;
	just_entered = true;

	if (depth >= 0 && depth < CPU_IRQ_CTX_MAX) {
		frames[depth].vector  = (uint8_t)vector;
		frames[depth].from_pc = from_pc;
		frames[depth].sp      = sp;
	}
	depth++;
}

void
cpu_irq_ctx_leave(uint16_t sp)
{
	// An RTI outside anything we saw entered is not an error -- an interrupt
	// could already have been in flight when the emulator started, or a handler
	// could have been entered before a reset. There is simply nothing to pop.
	if (depth <= 0) {
		depth = 0;
		return;
	}

	// Pop the innermost frame, then keep popping while the stack has unwound to
	// or past the frame below it. A balanced RTI restores the stack pointer to
	// exactly what it was on entry, so it pops one; a handler that returns
	// further than its own frame -- unwinding several at once -- would
	// otherwise leave the depth stuck high.
	//
	// "Past" is judged on a bounded difference, not on the raw values, because
	// the stack wraps: within page 1 in emulation mode, and at 16 bits in
	// native mode. A frame entered a few bytes from the bottom has a
	// numerically LOWER pointer than the one nested inside it, so comparing
	// absolutely would read a balanced inner return as having unwound past the
	// outer frame and collapse the whole depth. Every frame this can cascade
	// through was pushed by an interrupt, costing 3 or 4 bytes, so a genuine
	// unwind of the frames we record spans well under this window while a wrap
	// lands far outside it.
	while (depth > 0) {
		const int top = depth - 1;
		depth--;
		if (top >= CPU_IRQ_CTX_MAX)
			break;                       // no detail recorded for this one
		if (depth > 0 && depth - 1 < CPU_IRQ_CTX_MAX
		    && (uint16_t)(sp - frames[depth - 1].sp) <= CPU_IRQ_UNWIND_WINDOW)
			continue;                    // the next one down is finished too
		break;
	}
}

void
cpu_irq_ctx_reset(void)
{
	depth        = 0;
	taken        = 0;
	last_vector  = 0;
	just_entered = false;
	memset(frames, 0, sizeof(frames));
}

bool
cpu_in_interrupt(void)
{
	return depth > 0;
}

int
cpu_irq_depth(void)
{
	return depth;
}

uint32_t
cpu_irq_count(void)
{
	return taken;
}

int
cpu_irq_last_vector(void)
{
	return last_vector;
}

uint16_t
cpu_irq_return_pc(void)
{
	if (depth <= 0)
		return 0;
	const int top = depth - 1;
	return top < CPU_IRQ_CTX_MAX ? frames[top].from_pc : 0;
}

bool
cpu_irq_take_entered_flag(void)
{
	const bool v = just_entered;
	just_entered = false;
	return v;
}
