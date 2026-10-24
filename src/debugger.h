// *******************************************************************************************
// *******************************************************************************************
//
//		File:		debugger.h
//		Date:		5th September 2019
//		Purpose:	Debugger header
//		Author:		Paul Robson (paul@robson.org.uk)
//
// *******************************************************************************************
// *******************************************************************************************

#ifndef _DEBUGGER_H
#define _DEBUGGER_H

#include <SDL.h>
#include "render_backend.h"

// struct breakpoint and the breakpoint table itself now live in debug_core,
// which is the part a UI-less consumer (a debug server, another front end)
// needs and which can be tested without standing up an SDL window.
#include "debug_core.h"
#include "cpu/irq_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int showDebugOnRender;

void DEBUGRenderDisplay(int width,int height);
void DEBUGBreakToDebugger(void);
void DEBUGBreakOnWatchpoint(void);
int  DEBUGGetCurrentStatus(void);
// Preserve execution-control and stop checks without entering either debugger
// UI pump. Used from nested host event loops that cannot safely poll SDL.
int  DEBUGGetCurrentStatusNoPump(void);
void DEBUGInitUI(render_backend_t *pRenderer);
void DEBUGFreeUI();

// Execution control. These mirror the debug window's F5/F10/F11 handlers, and
// are what a debug server or an alternative front end drives instead of
// synthesising key presses. Safe to call whether or not the window exists.
void DEBUGContinue(void);                   // resume free-run
void DEBUGStepInto(void);                   // one instruction, then stop

// The step-over/step-out target lives outside the shared breakpoint table, so
// it carries its own owner: a session tearing down must retract a step it
// started, and must not retract one the user started at the keyboard.
void DEBUGStepOver(debug_owner_t owner);    // step over JSR/JSL; else single-step
void DEBUGStepOut(debug_owner_t owner);     // run to the current routine's return

// Step by SOURCE LINE rather than by instruction: repeat the step above until
// the PC reaches a different line. One C statement is many instructions, so in
// the Source panel a plain step looks like it does nothing for several presses.
//
// These are what the UI and DAP should call. They fall back to the instruction
// step whenever stepping by line would mean nothing -- the preference is off,
// the .dbg carries no high-level (C) line info, or the PC is not on a line at
// all -- so an assembly project steps exactly as it always did.
void DEBUGStepIntoAuto(void);
void DEBUGStepOverAuto(debug_owner_t owner);

// Whether stepping by line is wanted. On by default; only consulted where the
// debug info makes it meaningful. Abandoning a source step in progress is what
// DEBUGCancelSourceStep() is for -- pausing, or hitting a breakpoint, ends it.
void DEBUGSetSourceStep(bool on);
bool DEBUGGetSourceStep(void);
void DEBUGCancelSourceStep(void);

// Abandon a step-over/step-out that is still running. Its breakpoint is the
// debugger's own, so nothing else can retract it -- and left armed it stops the
// machine later for a session that has gone away.
void DEBUGCancelStep(void);

// Who asked for the pending step, or DEBUG_OWNER_COUNT if none is pending, and
// the retract-only-if-mine form that a session teardown wants.
debug_owner_t DEBUGStepOwner(void);
bool          DEBUGCancelStepFor(debug_owner_t owner);

void DEBUGPause(void);                      // halt now
void DEBUGRunTo(uint16_t pc, uint8_t bank, debug_owner_t owner); // run until (pc,bank)
bool DEBUGIsRunning(void);
bool DEBUGIsPaused(void);

// Interrupt following. With follow-interrupts on, an interrupt taken while
// stepping stops at the handler's first instruction instead of being run
// through invisibly; break-on-interrupt stops on every entry. Both off by
// default, so stepping behaves exactly as it did without them.
void DEBUGSetFollowInterrupts(bool on);
bool DEBUGGetFollowInterrupts(void);
void DEBUGSetBreakOnInterrupt(bool on);
bool DEBUGGetBreakOnInterrupt(void);

// Why execution last stopped: "step", "breakpoint", "data breakpoint", "user",
// "interrupt", or "" before the first stop. The returned pointer is a string
// literal and is never freed. A UI uses this to keep the view still after a
// step but re-centre it after an arrival somewhere new.
void        DEBUGSetStopReason(const char *reason);
// Record the reason and tell a DAP client execution stopped. Edge-triggered:
// call this exactly once per RUN->STOP transition, from the site that knows it
// is one. Sites that run on every poll while already halted, or that set a
// default a later caller overrides, must use DEBUGSetStopReason() instead.
void        DEBUGAnnounceStop(const char *reason);
const char *DEBUGGetStopReason(void);

#define DBG_WIDTH 		(60)									// Char cells across
#define DBG_HEIGHT 		(60)

#define DBG_ASMX 		(1)										// Disassembly starts here
#define DBG_LBLX 		(26) 									// Debug labels start here
#define DBG_DATX		(30)									// Debug data starts here.
#define DBG_STCK		(40)									// Debug stack starts here.
#define DBG_MEMX 		(1)										// Memory Display starts here
#define DBG_ZP_REG     (45)                             // Zero page registers start here
#define DBG_VERA_REGX   (45)                             // VERA registers start here

#define DMODE_STOP 		(0)										// Debugger is waiting for action.
#define DMODE_STEP 		(1)										// Debugger is doing a single step
#define DMODE_RUN 		(2)										// Debugger is running normally.

// The run state itself, one of DMODE_*. Defined in debugger.c and driven by
// whichever front end is attached -- the SDL debugger, the ImGui debugger, or
// the DAP server when a client is stepping.
extern int currentMode;

// ── The per-instruction fast path ───────────────────────────────────────────
// DEBUGGetCurrentStatus() runs before every instruction whenever a debugger is
// attached. While the machine is running freely it almost always has nothing
// to do, and calling it anyway cost about 0.9 ms of host time per frame -- more
// than every other debugger feature put together (docs/profiler-plan.md).
//
// So the loops call DEBUGPollStatus() instead, which runs the full check only
// when something could actually stop the machine at this instruction, and
// otherwise every DEBUG_POLL_INTERVAL instructions, so the DAP socket and the
// Break key are still serviced. Everything it reads is the real state, not a
// copy, so a front end that changes the state directly -- the DAP server sets
// currentMode itself -- is seen on the very next instruction.
//
// A clear bit in debug_bp_pc_map is the only thing that can skip a breakpoint
// test, and it is clear only when no entry at all has that PC. Banks,
// conditions, hit counts and ignore budgets are still decided in full by the
// slow path, so their semantics are unchanged.
#define DEBUG_POLL_INTERVAL 1000

extern struct breakpoint stepBreakPoint;
extern int               resumeSkipPC;
extern bool              watchpointStopPending;
extern int               debug_poll_tick;

static inline bool
DEBUGNothingToDo(uint16_t pc)
{
	return currentMode == DMODE_RUN
	    && stepBreakPoint.pc < 0
	    && resumeSkipPC < 0
	    && !watchpointStopPending
	    && !cpu_irq_entered_pending()
	    && !debug_bp_maybe_at(pc);
}

static inline int
DEBUGPollStatus(uint16_t pc)
{
	if (DEBUGNothingToDo(pc) && ++debug_poll_tick < DEBUG_POLL_INTERVAL)
		return 0;
	return DEBUGGetCurrentStatus();
}

static inline int
DEBUGPollStatusNoPump(uint16_t pc)
{
	if (DEBUGNothingToDo(pc) && ++debug_poll_tick < DEBUG_POLL_INTERVAL)
		return 0;
	return DEBUGGetCurrentStatusNoPump();
}
#ifdef __cplusplus
}
#endif

#endif
