// Commander X16 Emulator — interrupt context tracking. See irq_ctx.h.

#include "irq_ctx.h"

#include <string.h>

// Past CPU_IRQ_CTX_MAX (see irq_ctx.h) the depth still counts, but there is no
// room to record which stack pointer each of those entries had, so they can
// only be unwound one at a time.

// Nesting beyond this is a runaway loop -- a BRK handler that BRKs, say -- and
// the exact number stops meaning anything. Saturating keeps a signed counter
// from overflowing, which would be undefined behaviour rather than merely a
// wrong answer.
#define CPU_IRQ_DEPTH_MAX 0x10000

struct irq_frame {
	uint8_t  vector;
	uint32_t from_pc;  // 24-bit: the program bank matters on the 65816
	uint16_t sp;       // stack pointer before the interrupt pushed anything
};

static struct irq_frame frames[CPU_IRQ_CTX_MAX];
static int              depth;
static uint32_t         taken;
static uint8_t          last_vector;
static bool             just_entered;

void
cpu_irq_ctx_enter(int vector, uint32_t from_pc, uint16_t sp)
{
	taken++;
	last_vector  = (uint8_t)vector;
	just_entered = true;

	if (depth >= 0 && depth < CPU_IRQ_CTX_MAX) {
		frames[depth].vector  = (uint8_t)vector;
		frames[depth].from_pc = from_pc;
		frames[depth].sp      = sp;
	}
	if (depth < CPU_IRQ_DEPTH_MAX)
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

	// Nothing was recorded for entries above the array, so they come off singly.
	if (depth > CPU_IRQ_CTX_MAX) {
		depth--;
		return;
	}

	// A balanced RTI restores the stack pointer to exactly what it was when the
	// interrupt was taken, so the frame being returned from is the one whose
	// recorded pointer equals this one. Matching exactly, rather than measuring
	// how far the stack has moved, is what keeps this honest: the stack is
	// circular -- 256 bytes in emulation mode, 64K in native -- so a handler
	// that pushes enough to wrap can leave a numerically LOWER pointer than the
	// frame nested inside it, and any distance-based test then reads a balanced
	// inner return as having unwound past its caller and collapses the depth to
	// zero while that caller is still running.
	//
	// Searching from the innermost outwards also handles a handler that returns
	// past its own frame, unwinding several at once.
	for (int i = depth - 1; i >= 0; i--) {
		if (frames[i].sp == sp) {
			depth = i;
			return;
		}
	}

	// No frame matches: a handler that rewrote the stack, or an RTI for an
	// interrupt taken before this was watching. Pop one and stay conservative.
	// A depth left too high corrects itself at the next matching return, while
	// one left too low claims no interrupt is running when one is -- and a
	// debugger confidently reporting the wrong thing is worse than a stale one.
	depth--;
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

uint32_t
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
