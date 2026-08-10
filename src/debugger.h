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
// whichever front end is attached -- the SDL debugger, or the DAP server when a
// client is stepping.
extern int currentMode;

#endif
