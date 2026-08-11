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

// struct breakpoint and the breakpoint table itself now live in debug_core,
// which is the part a UI-less consumer (a debug server, another front end)
// needs and which can be tested without standing up an SDL window.
#include "debug_core.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int showDebugOnRender;

void DEBUGRenderDisplay(int width,int height);
void DEBUGBreakToDebugger(void);
void DEBUGBreakOnWatchpoint(void);
int  DEBUGGetCurrentStatus(void);
void DEBUGInitUI(SDL_Renderer *pRenderer);
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
#ifdef __cplusplus
}
#endif

#endif
