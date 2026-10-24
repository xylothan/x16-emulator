// Commander X16 Emulator — calling-context tree (CCT) for capture mode.
//
// perf_hist gives self time per address. This adds WHO CALLED IT: one node
// per distinct call path (main -> update -> draw_sprites), with self cycles,
// calls, worst single call and worst frame, and inclusive cycles summed from
// the tree. Its size depends on the program's call structure, not on how long
// the capture runs.
//
// Events come from the CPU core's call, return and interrupt points (see
// cpu_flow_hook in cpu/irq_ctx.h). Cycles come from perf_hist_step_, after
// each instruction. The order of the two matters, and matches perf_hist's own
// main/IRQ split exactly:
//
//   - calls and returns are QUEUED and applied after the instruction's cycles
//     are counted, so a JSR's cycles belong to the caller and an RTS's to the
//     callee;
//   - interrupt entry and RTI are applied AT ONCE, so BRK's cycles and those
//     of the first handler instruction land in the handler, and RTI's in the
//     code it returns to.
//
// Calls and returns are paired by stack pointer on one shadow stack, like
// cpu/irq_ctx.c. Every event first retires frames the stack has risen past:
//
//     pop while innermost.sp_before <= event sp
//
// A return pops the frame it matches and anything inner (PLA/PLA early exits);
// a return BELOW the innermost frame matched nothing and is a computed jump
// (RTS used as a jump table), so it pops nothing; and TXS resets or longjmps
// heal at the next event above the stale frames.
//
// Interrupt time has its own root, so it never inflates the routine it
// interrupted: a handler is a child of [IRQ], not of the code it landed in.
//
// Like perf_budget.c, this touches no emulator globals; tests drive it with
// synthetic event streams (tests/test_profile_cct.c).

#ifndef PROFILE_CCT_H
#define PROFILE_CCT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Keys are perf_hist keys (22 bits); these are above that range.
#define CCT_KEY_MAIN  0xFFFFFFF0u  // node 0: main line with no known caller
#define CCT_KEY_IRQ   0xFFFFFFF1u  // node 1: parent of every interrupt handler
#define CCT_KEY_OTHER 0xFFFFFFF2u  // paths that did not fit in the pool

#define CCT_NODE_MAIN 0
#define CCT_NODE_IRQ  1

typedef struct {
	uint32_t key;
	int32_t  parent;      // -1 for the two roots
	uint64_t self;        // cycles with this node innermost
	uint64_t incl;        // self plus every descendant; valid after cct_finish()
	uint32_t calls;
	uint32_t call_max;    // worst single call
	uint32_t frame_max;   // worst frame (cycles of calls that ended in it)
} cct_node_t;

// Allocate a pool of `max_nodes` (at least 64). False if out of memory.
bool cct_init(int max_nodes);
void cct_free(void);
void cct_reset(void);

// ---- Events (sp as cpu/irq_ctx.h reports it) -----------------------------
// A call: `sp_before` is SP before the return address was pushed.
void cct_call(uint32_t target_key, uint16_t sp_before);
// A return: `sp_after` is SP after the return address was pulled.
void cct_return(uint16_t sp_after);
// Interrupt entry (`sp_before` before anything was pushed) and RTI.
void cct_irq_enter(uint32_t handler_key, uint16_t sp_before);
void cct_irq_leave(uint16_t sp_after);

// After every instruction: credit `clocks` to the innermost node, then apply
// queued calls and returns.
void cct_step(uint32_t clocks);
void cct_close_frame(void);

// Close every open frame (a call that never returned ends here) and compute
// inclusive totals. Call once when the capture stops; events after it start
// from the roots again.
void cct_finish(void);

// ---- Queries ---------------------------------------------------------------
int  cct_node_count(void);           // nodes are numbered from 0; parents first
bool cct_node_get(int i, cct_node_t *out);
int  cct_depth(void);                // open shadow frames
int  cct_current(void);              // innermost node
bool cct_overflowed(void);           // some paths were folded into [other]

#ifdef __cplusplus
}
#endif

#endif
