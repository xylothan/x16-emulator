// Commander X16 Emulator — interrupt context tracking. See irq_ctx.h.

#include "irq_ctx.h"

#include <string.h>

// Past CPU_IRQ_CTX_MAX (see irq_ctx.h) the depth still counts, but there is no
// room to record which stack pointer each of those entries had, so they can
// only be unwound one at a time.

// Nesting beyond this is a runaway loop, and the exact number stops meaning
// anything. This is reachable, not merely defensive: retirement can only judge
// frames it recorded, so above CPU_IRQ_CTX_MAX it stops happening and every
// further entry adds one. A handler that BRKs on entry would otherwise climb
// until a signed counter overflowed, which is undefined behaviour rather than
// merely a wrong answer.
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

	// Retire frames the stack has already passed. Nesting drives the pointer
	// DOWN, so open frames must be strictly decreasing in sp; one recorded at or
	// below where this interrupt is being taken cannot still be live, because
	// the stack has risen back above it.
	//
	// This is what clears a handler that left without an RTI -- a BRK reaching a
	// warm start, which resets the stack instead of returning. Nothing on the
	// return path can do it: the abandoned frame has no matching return, so it
	// would sit there for the rest of the session, and every further BRK would
	// add another.
	//
	// Judged here rather than on return because entry is the moment the stack
	// level is known to be real. A return only tells us where the stack ended
	// up, which a handler is free to fake.
	//
	// Levels above the array have no recorded pointer of their own, so they are
	// judged by the deepest one that has: an interrupt taken at or above
	// frames[CPU_IRQ_CTX_MAX - 1] proves everything from there up is gone,
	// unrecorded levels included. Skipping them instead would strand every frame
	// underneath as soon as the depth passed the array, which is the same
	// permanent staleness this rule exists to prevent.
	while (depth > 0) {
		const int top = (depth < CPU_IRQ_CTX_MAX ? depth : CPU_IRQ_CTX_MAX) - 1;
		if (frames[top].sp > sp)
			break;
		depth = top;
	}

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
	// how far the stack has moved, is what keeps this honest: a distance test
	// cannot tell a genuine unwind from a stack that wrapped, and guessing
	// wrong collapses the depth to zero while a handler is still running.
	//
	// Retirement on entry keeps the recorded pointers strictly decreasing, so
	// at most one frame can match and the search direction does not matter.
	// Searching outwards also handles a handler that returns past its own
	// frame, unwinding several at once.
	for (int i = depth - 1; i >= 0; i--) {
		if (frames[i].sp == sp) {
			depth = i;
			return;
		}
	}

	// No frame matches: a handler that rewrote the stack, or an RTI for an
	// interrupt taken before this was watching. Leave the depth alone rather
	// than popping a frame that may still be live -- a handler can fabricate a
	// frame and RTI to itself with three pushes, which lands on no recorded
	// pointer at all while the real interrupt is still running.
	//
	// So this errs high, and stays there until a return matches an outer frame.
	// Too high corrects itself at the next matching return; too low would claim
	// no interrupt is running while one is, and a debugger confidently
	// reporting that is worse than one reporting a stale depth.
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
