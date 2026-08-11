
// *******************************************************************************************
// *******************************************************************************************
//
//		File:		debugger.c
//		Date:		5th September 2019
//		Purpose:	Debugger code
//		Author:		Paul Robson (paul@robson.org.uk)
//
// *******************************************************************************************
// *******************************************************************************************

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <SDL.h>
#include "glue.h"
#include "timing.h"
#include "disasm.h"
#include "memory.h"
#include "video.h"
#include "cpu/fake6502.h"
#include "debugger.h"
#include "debug_server.h"
#include "rendertext.h"

static void DEBUGHandleKeyEvent(SDL_Keycode key,int isShift);

static void DEBUGNumber(int x,int y,int n,int w, SDL_Color colour);
static void DEBUGNumberDec(int x, int y, int n, int w, SDL_Color colour);
static void DEBUGAddress(int x, int y, int x16Bank, int addr, uint8_t bank, SDL_Color colour);
static void DEBUGVAddress(int x, int y, int addr, SDL_Color colour);

static void DEBUGRenderData(int y,uint32_t data);
static int DEBUGRenderZeroPageRegisters(int y);
static void DEBUGRenderVERAState(int y);
static int DEBUGRenderRegisters(void);
static void DEBUGRenderVRAM(int y, int data);
static void DEBUGRenderCode(int lines,int initialPC);
static void DEBUGRenderStack(int bytesCount);
static void DEBUGRenderCmdLine(int x, int width, int height);
static bool DEBUGEditCmdLine(SDL_Keycode key);
static void DEBUGBuildCmdLine(char *text);
static void DEBUGExecCmd();

// *******************************************************************************************
//
//		This is the minimum-interference flag. It's designed so that when
//		its non-zero DEBUGRenderDisplay() is called.
//
//			if (showDebugOnRender != 0) {
//				DEBUGRenderDisplay(SCREEN_WIDTH,SCREEN_HEIGHT,renderer);
//				SDL_RenderPresent(renderer);
//				return true;
//			}
//
//		before the SDL_RenderPresent call in video_update() in video.c
//
//		This controls what is happening. It is at the top of the main loop in main.c
//
//			if (isDebuggerEnabled != 0) {
//				int dbgCmd = DEBUGGetCurrentStatus();
//				if (dbgCmd > 0) continue;
//				if (dbgCmd < 0) break;
//			}
//
//		Both video.c and main.c require debugger.h to be included.
//
//		isDebuggerEnabled should be a flag set as a command line switch - without it
//		it will run unchanged. It should not be necessary to test the render code
//		because showDebugOnRender is statically initialised to zero and will only
//		change if DEBUGGetCurrentStatus() is called.
//
// *******************************************************************************************

//
//				0-9A-F sets the program address, with shift sets the data address.
//
#define DBGKEY_HOME     SDLK_F1                         // F1 is "Goto PC"
#define DBGKEY_RESET    SDLK_F2                         // F2 resets the 6502
#define DBGKEY_RUN      SDLK_F5                         // F5 is run.
#define DBGKEY_SETBRK   SDLK_F9                         // F9 sets breakpoint
#define DBGKEY_STEP     SDLK_F11                        // F11 is step into.
#define DBGKEY_STEPOVER SDLK_F10                        // F10 is step over.
#define DBGKEY_BANK_NEXT	SDLK_KP_PLUS
#define DBGKEY_BANK_PREV	SDLK_KP_MINUS

#define DBGSCANKEY_BRK  SDL_SCANCODE_F12                // F12 is break into running code.
#define DBGSCANKEY_SHOW SDL_SCANCODE_TAB                // Show screen key.
                                                        // *** MUST BE SCAN CODES ***

#define DBGMAX_ZERO_PAGE_REGISTERS 16

#define DDUMP_RAM	0
#define DDUMP_VERA	1

enum DBG_CMD { CMD_DUMP_MEM='m', CMD_DUMP_VERA='v', CMD_DISASM='d', CMD_SET_BANK='b', CMD_SET_REGISTER='r', CMD_FILL_MEMORY='f' };

// RGB colours
const SDL_Color col_bkgnd= {0, 0, 0, 255};
const SDL_Color col_label= {0, 255, 0, 255};
const SDL_Color col_data= {0, 255, 255, 255};
const SDL_Color col_highlight= {255, 255, 0, 255};
const SDL_Color col_cmdLine= {255, 255, 255, 255};

const SDL_Color col_directpage= {192, 224, 255, 255};

const SDL_Color col_vram_tilemap = {0, 255, 255, 255};
const SDL_Color col_vram_tiledata = {0, 255, 0, 255};
const SDL_Color col_vram_special  = {255, 92, 92, 255};
const SDL_Color col_vram_other  = {128, 128, 128, 255};

int showDebugOnRender = 0;                            // Used to trigger rendering in video.c
int showFullDisplay = 0;                              // If non-zero show the whole thing.
int currentPC = -1;                                   // Current disassembly PC value (16-bit).
uint8_t currentPCBank = 0;                            // Current disassembly PC bank (like regs.k) value.
int currentData = 0;                                  // Current data display address (RAM or VRAM).
int currentPCX16Bank = -1;                            // Current disassembly X16 RAM/ROM bank, -1 unless $A000-$FFFF, .K = 0
int currentX16Bank = -1;                              // Current data display (memory dump) X16 RAM/ROM bank
int currentMode = DMODE_RUN;                          // Start running.
uint32_t debugCPUClocks = 0;

int dumpmode          = DDUMP_RAM;

// Where we resumed from, so the breakpoint we are parked on does not fire again
// the instant we continue. Cleared once the CPU has actually moved on.
//
// Pressing F5 never needed this: the key is read inside DEBUGGetCurrentStatus,
// after the breakpoint test, so by the next call the CPU has already stepped
// off the address. Anything driving execution from outside -- which is the
// point of the execution-control functions -- changes the mode between calls
// instead, and would otherwise re-trigger the same breakpoint forever without
// ever advancing.
//
// The clock is part of the test, not just the address: `JMP *` at a breakpoint
// never changes the PC, and an address-only guard would then suppress that
// breakpoint permanently. Conversely the clock alone is not enough, in two
// ways: handle_ieee_intercept() can skip an iteration without executing
// anything, and -- the sharp one -- step6502() ticks the clock and returns
// immediately while the CPU is parked in WAI. A breakpoint on the instruction
// after a WAI, which is the idiomatic frame-sync placement, would otherwise
// clear the guard every poll and buy one emulated cycle per resume.
static int      resumeSkipPC    = -1;
static uint8_t  resumeSkipBank  = 0;
static uint32_t resumeSkipClock = 0;

// A watchpoint fired mid-instruction and the reported location still has to be
// taken, once the instruction that did the write has finished. See
// DEBUGBreakOnWatchpoint().
static bool watchpointStopPending = false;

static void DEBUGClearStepBreakPoint(void);

static void DEBUGArmResumeSkip(void) {
	resumeSkipPC    = regs.pc;
	resumeSkipBank  = regs.k;
	resumeSkipClock = clockticks6502;
}

struct breakpoint stepBreakPoint = { -1, 0, -1, 0, true };     // Single step break.

// Who asked for the pending step-over/step-out.
//
// The step target deliberately lives outside the shared breakpoint table, so it
// needs its own record of who wanted it -- and that record belongs next to the
// state it describes, rather than in a flag kept by whichever front end happened
// to start the step. DEBUG_OWNER_COUNT means nothing is pending.
static debug_owner_t stepOwner = DEBUG_OWNER_COUNT;

// Interrupt following. followInterrupts makes a step that is interrupted stop
// at the handler instead of running through it; breakOnInterrupt stops on every
// interrupt entry, even while free-running. Both default off so normal stepping
// behaves exactly as before.
static bool followInterrupts = false;
static bool breakOnInterrupt = false;

void DEBUGSetFollowInterrupts(bool on) { followInterrupts = on; }
bool DEBUGGetFollowInterrupts(void)    { return followInterrupts; }
void DEBUGSetBreakOnInterrupt(bool on) { breakOnInterrupt = on; }
bool DEBUGGetBreakOnInterrupt(void)    { return breakOnInterrupt; }

// Why execution last stopped. A UI needs this to tell a step apart from an
// arrival somewhere new: after a step the view should stay put, while a
// breakpoint or a pause has to re-centre or the highlight lands off-screen.
// Recorded here rather than in a debug client so it is correct whether or not
// one is attached. Only ever set to a string literal, so there is nothing to
// free and no buffer to overrun.
static const char *lastStopReason = "";

void
DEBUGSetStopReason(const char *reason)
{
	lastStopReason = reason ? reason : "";
}

const char *
DEBUGGetStopReason(void)
{
	return lastStopReason;
}
char cmdLine[64]= "";                                 // command line buffer
int currentPosInLine= 0;                              // cursor position in the buffer (NOT USED _YET_)
int currentLineLen= 0;                                // command line buffer length

int    oldRegisters[DBGMAX_ZERO_PAGE_REGISTERS];      // Old ZP Register values, for change detection
char * oldRegChange[DBGMAX_ZERO_PAGE_REGISTERS];      // Change notification flags for output
int    oldRegisterTicks = 0;                          // Last PC when change notification was run

//
//		This flag controls
//

SDL_Renderer *dbgRenderer;                            // Renderer passed in.

// Unbanked, the RAM window, or the ROM window. A bank number selected for one
// of these means nothing in the others.
static int bank_window_of(int addr) {
	if (addr < 0xA000)
		return 0;
	return addr < 0xC000 ? 1 : 2;
}
// Which RAM/ROM window the PC is in, or -1 where a bank means nothing there.
// Delegates to debug_core so the rule that *produces* a selector is the same one
// the matcher applies. The copy that used to live here required the program bank
// to be zero unconditionally, which disagrees on gen1 with -c816: read6502
// forces the bank to zero there, so the window registers do apply.
static inline int getCurrentBank(int pc, uint8_t bank) {
	return debug_current_x16_bank(pc, bank);
}

// *******************************************************************************************
//
//      	This determines if we have hit a breakpoint, both in pc and bank
//
// *******************************************************************************************

static inline bool hitBreakpoint(int pc, uint8_t bank, struct breakpoint bp) {
	// Uses the same bank rule as every other matcher, so this field cannot mean
	// "any bank" in one place and "not banked" in another.
	return pc == bp.pc && bank == bp.bank
	       && debug_bank_selector_matches(bp.x16Bank, pc, bank);
}

// *******************************************************************************************
//
//			This is used to determine who is in control. If it returns zero then
//			everything runs normally till the next call.
//			If it returns +ve, then it will update the video, and loop round.
//			If it returns -ve, then exit.
//
// *******************************************************************************************

int  DEBUGGetCurrentStatus(void) {

	SDL_Event event;
	if (currentPC < 0) currentPC = regs.pc;                                      // Initialise current PC displayed.

	// Interrupt following. cpu_irq_take_entered_flag() is true exactly once per
	// interrupt entry, and at this point the PC is already the handler's first
	// instruction. "Break on interrupt" stops whenever one is taken; "follow
	// interrupts" only stops if we were stepping, so that an interrupt landing
	// in the middle of a step-over/step-out becomes visible instead of being
	// silently run through.
	//
	// The flag is consumed unconditionally -- reading it is what clears it -- so
	// that a stale entry cannot fire a stop later, after the feature is enabled.
	{
		const bool entered = cpu_irq_take_entered_flag();
		const bool wasStepping = (currentMode == DMODE_STEP) || (stepBreakPoint.pc >= 0);
		if (entered && (breakOnInterrupt
		                || (followInterrupts && wasStepping && currentMode != DMODE_STOP))) {
			currentPC = regs.pc;
			currentPCBank = regs.k;
			currentPCX16Bank = getCurrentBank(regs.pc, regs.k);
			currentMode = DMODE_STOP;
			stepBreakPoint.pc = -1;                             // the pending step target is now unreachable
			stepBreakPoint.bank = 0;
			stepBreakPoint.x16Bank = -1;
			DEBUGSetStopReason("interrupt");
		}
	}

	// A watchpoint stopped us from inside a store. The instruction that did the
	// write has now finished, so this is the first point at which regs.pc says
	// where the CPU actually ended up.
	if (watchpointStopPending) {
		watchpointStopPending = false;
		DEBUGSetStopReason("data breakpoint");
		currentPC = regs.pc;
		currentPCBank = regs.k;
		currentPCX16Bank = getCurrentBank(regs.pc, regs.k);
		// Reported here rather than from the store itself, for the same reason:
		// only now does regs.pc name the instruction after the write.
		debug_server_notify_stopped("data breakpoint");
	}

	if (currentMode == DMODE_STEP) {                                // Single step before
		if (currentPC != regs.pc || currentPCBank != regs.k || currentPCX16Bank != getCurrentBank(regs.pc, regs.k)) { // Ensure that the PC moved
			currentPC = regs.pc;                                    // Update current PC
			currentPCBank = regs.k;
			currentPCX16Bank = getCurrentBank(regs.pc, regs.k);     // Update the bank if we are in upper memory.
			currentMode = DMODE_STOP;                               // So now stop, as we've done it.
			// A single step retires here without ever arming a step target, so
			// there is none to retract: the owner of a pending step is derived
			// from whether one is armed at all.
			DEBUGSetStopReason("step");
			debug_server_notify_stopped("step");
		}
	}

	// debug_bp_on_arrival() also counts the hit and spends the ignore budget,
	// so the DMODE_STOP guard is load-bearing rather than an optimisation: while
	// halted this function is polled continuously with the PC parked on the
	// breakpoint, and counting each poll would burn through an ignore count
	// while the machine is standing still.
	//
	// The resume guard covers the other half of the same problem: having just
	// been told to continue from a breakpoint, the PC is still sitting on it,
	// and testing it again would stop instantly and never make progress.
	// Cycles only count as progress when the CPU is actually running them: in
	// WAI it is parked, ticking the clock without retiring anything.
	const bool cpuAdvanced = (clockticks6502 != resumeSkipClock) && !waiting;
	if (resumeSkipPC >= 0
	    && (regs.pc != resumeSkipPC || regs.k != resumeSkipBank || cpuAdvanced)) {
		resumeSkipPC = -1;                                      // the CPU moved on; guard done
	}
	const bool stopped     = (currentMode == DMODE_STOP);
	const bool justResumed = (resumeSkipPC >= 0);
	// Parked in WAI counts as standing still here too. The CPU stops on the
	// instruction AFTER the WAI -- the opcode fetch has already advanced the PC
	// -- and then ticks the clock without retiring anything, so this poll runs
	// once per emulated cycle with the PC unchanged. Testing arrival there
	// stops before the awaited interrupt has happened, and because the test is
	// not a query -- it counts the hit and spends the ignore budget -- an
	// "ignore 60" meant as sixty frames is exhausted in sixty cycles, still
	// inside the first wait.
	//
	// Both ways out stay correct: a masked interrupt clears `waiting` without
	// vectoring, so the next poll sees the PC still on the breakpoint and stops
	// there; a vectored one arrives after the handler returns.
	if (!stopped && !justResumed && !waiting) {
		// Order matters: debug_bp_on_arrival() counts the hit and spends the
		// ignore budget, so it has to be reached exactly as often as before.
		const bool viaUser = debug_bp_on_arrival(regs.pc, regs.k);
		const bool viaStep = !viaUser && hitBreakpoint(regs.pc, regs.k, stepBreakPoint);
		if (viaUser || viaStep) {                               // Hit a breakpoint.
			currentPC = regs.pc;                                    // Update current PC
			currentPCBank = regs.k;
			currentPCX16Bank = getCurrentBank(regs.pc, regs.k);     // Update the bank if we are in upper memory.
			currentMode = DMODE_STOP;                               // So now stop, as we've done it.
			DEBUGClearStepBreakPoint();                             // and the step target is retired
			// A step-over or step-out finishing is a completed step, not a
			// breakpoint nobody set. Both the graphical debugger and a DAP
			// client key off this: a step leaves the view where it is, an
			// arrival re-centres it.
			DEBUGSetStopReason(viaStep ? "step" : "breakpoint");
			debug_server_notify_stopped(viaStep ? "step" : "breakpoint");
		}
	}

	if (SDL_GetKeyboardState(NULL)[DBGSCANKEY_BRK]) {            // Stop on break pressed.
		const bool wasRunning = (currentMode != DMODE_STOP);
		currentMode = DMODE_STOP;
		DEBUGSetStopReason("user");
		currentPC = regs.pc;                                     // Set the PC to what it is.
		currentPCBank = regs.k;
		currentPCX16Bank = getCurrentBank(regs.pc, regs.k);      // Update the bank if we are in upper memory.
		if (wasRunning) {                                        // Tell a DAP client too; see DEBUGBreakToDebugger().
			debug_server_notify_stopped("pause");
		}
	}

	// Repair the bank when it is unset: currentPC moves independently of
	// regs.pc via the navigation keys, so it can land in a banked window while
	// currentPCX16Bank still says "none". Uses the shared rule rather than a
	// local copy of it, which is what let this one keep the old requirement
	// that the program bank be zero.
	if (currentPCX16Bank < 0) {
		currentPCX16Bank = debug_current_x16_bank(currentPC, currentPCBank);
	}

	// A DAP client can drive the run state while we are halted, so give it a
	// turn before anything blocks: continue/step arriving over the socket has
	// to be able to get us moving again, whichever UI owns the screen.
	if (currentMode != DMODE_RUN) {
		debug_server_poll();
		if (currentMode == DMODE_RUN || currentMode == DMODE_STEP) {
			return 0;                                                   // Mode changed remotely — execute.
		}
	}

	// Who owns the UI while the machine is not running. With -imgui and no
	// text debugger the graphical one does: it pumps events, repaints, and
	// decides when to resume. Gated on the window having actually been created
	// -- if it failed to open there is no control bar to resume from, and
	// swallowing the loop here would park the machine unrecoverably.
	//
	// Tested on debug_window_enabled, not debugger_enabled: -debugport turns
	// debugger_enabled on by itself to get the breakpoint machinery, so keying
	// on it handed -imgui -debugport to the text debugger -- drawing its
	// overlay over a session that never asked for one, and swallowing the
	// emulator window's keys. debug_window_enabled means -debug/-bp/-wp, which
	// is the question being asked. (-imgui -bp still goes to the text pump, as
	// before: -bp does ask for it.)
	const bool imguiOwnsUI = (!debug_window_enabled && video_debug_ui_available());

	// DMODE_STOP only, not "not RUN". DMODE_STEP means one instruction has been
	// asked for and must be executed now: routing it through the pump costs it
	// the pump's 16ms sleep, and a step stays in DMODE_STEP until the PC
	// actually moves. Over a WAI the PC does not move for the whole wait, so
	// stepping one instruction would run at ~60 emulated cycles a second and
	// take tens of minutes. The classic path below has the same shape: it only
	// sleeps once currentMode == DMODE_STOP.
	if (currentMode == DMODE_STOP && imguiOwnsUI) {
		showDebugOnRender = 0;                                  // no SDL overlay in this mode
		return video_debug_ui_pump_paused();
	}

	// The classic text debugger's event pump. Skipped entirely when the ImGui
	// window owns the UI: it drains the whole SDL queue into the text
	// debugger's key handler and drops everything else, so letting DMODE_STEP
	// fall in here destroys every mouse and keyboard event before ImGui or the
	// machine sees it. That is reachable whenever a step leaves the PC where it
	// was -- for the duration of a WAI, and permanently on a `JMP *` self-loop,
	// which would leave the debugger input-dead with no way to quit.
	if (currentMode != DMODE_RUN && !imguiOwnsUI) {                     // Not running, we own the keyboard.
		showFullDisplay =                                               // Check showing screen.
					SDL_GetKeyboardState(NULL)[DBGSCANKEY_SHOW];
		while (SDL_PollEvent(&event)) {                                 // We now poll events here.
			// With -debug and -imgui together this loop is the only one
			// running while halted -- video_update() returns early whenever
			// the text overlay is on screen -- so it has to feed the graphical
			// debugger too. Without this the ImGui window stayed on screen but
			// received no input at all: dead buttons, dead text fields, stale
			// mouse state. -imgui -bp reaches this combination without asking
			// for it, since -bp switches the text debugger on.
			if (video_debug_ui_available()) {
				video_debug_ui_feed_event(&event);
				// Events aimed at the debugger window must not also drive the
				// text debugger's command line.
				if (video_event_targets_debug_ui(&event)) {
					if (event.type == SDL_WINDOWEVENT &&
					    event.window.event == SDL_WINDOWEVENT_CLOSE) {
						continue;   // closing the debugger must not quit
					}
					video_debug_ui_shortcut_key(&event);
					continue;
				}
			}
			switch(event.type) {
				case SDL_QUIT:                                  // Time for exit
					return -1;

				case SDL_WINDOWEVENT:
					// With the ImGui window open SDL2 no longer synthesises
					// SDL_QUIT on a window close -- it only does that for the
					// last window -- so a close arrives here as a bare event.
					// Dropping it made the emulator window's [X] inert while
					// the text debugger was on screen under -debug -imgui.
					if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
						return -1;
					}
					break;

				case SDL_KEYDOWN:                               // Handle key presses.
					DEBUGHandleKeyEvent(event.key.keysym.sym, event.key.keysym.mod & (KMOD_LSHIFT|KMOD_RSHIFT));
					break;

				case SDL_TEXTINPUT:
					DEBUGBuildCmdLine(event.text.text);
					break;
			}
		}
	}

	showDebugOnRender = (currentMode != DMODE_RUN);                         // Do we draw it - only in RUN mode.
	if (currentMode == DMODE_STOP) {                                        // We're in charge.
		video_update();
		SDL_Delay(10);
		return 1;
	}

	// While running, a client still has to be able to connect and ask us to
	// break. This runs per instruction, so polling the socket every time would
	// cost more than the emulation; once every few thousand is still well
	// inside a frame.
	{
		static int poll_counter = 0;
		if (++poll_counter >= 1000) {
			poll_counter = 0;
			if (debug_server_poll()) {
				return (currentMode == DMODE_STOP) ? 1 : 0;
			}
		}
	}

	return 0;                                                               // Run wild, run free.
}

// *******************************************************************************************
//
//								Setup fonts and co
//
// *******************************************************************************************
void DEBUGInitUI(SDL_Renderer *pRenderer) {
		DEBUGInitChars(pRenderer);
		dbgRenderer = pRenderer;				// Save renderer.
}

// *******************************************************************************************
//
//								Setup fonts and co
//
// *******************************************************************************************
void DEBUGFreeUI() {
}

// *******************************************************************************************
//
//								Break into debugger from code.
//
// *******************************************************************************************

void DEBUGBreakToDebugger(void) {
	const bool wasRunning = (currentMode != DMODE_STOP);
	// Default reason for "something broke into the debugger". Set here, before
	// the mode changes, so no path can leave the previous stop's reason
	// showing; a caller with something more specific to say (the STP hook)
	// overrides it immediately after.
	DEBUGSetStopReason("user");
	currentMode = DMODE_STOP;
	currentPC = regs.pc;
	currentPCBank = regs.k;
	currentPCX16Bank = getCurrentBank(regs.pc, regs.k);
	// A DAP client only refreshes its view -- stack, variables, step controls --
	// when it is told execution stopped. Halting without saying so leaves it
	// believing the machine is still running.
	if (wasRunning) {
		debug_server_notify_stopped("pause");
	}
}

// A watched address was written. Called from the CPU store path, so it does as
// little as possible: memory.c has already established that a watchpoint
// matched.
//
// The reported location is captured later, not here. At store time the CPU is
// mid-instruction: the addressing mode has already advanced regs.pc past the
// operands, but the instruction's own effect on the PC has not happened yet. An
// ordinary STA lands where regs.pc already points, but a JSR pushing its return
// address, an interrupt pushing its frame, or MVN/MVP rewinding to repeat do
// not -- and those are exactly the writes a stack watchpoint is set to catch.
// Deferring the snapshot to the next status poll reports where the CPU
// actually ended up.
void DEBUGBreakOnWatchpoint(void) {
	if (currentMode != DMODE_STOP) {
		currentMode          = DMODE_STOP;
		watchpointStopPending = true;
	}
}

// *******************************************************************************************
//
//		Execution control.
//
//		One implementation, driven either by the debug window's function keys or
//		by another front end. Previously this logic lived only inside the key
//		handler, so anything else wanting to step had to synthesise key presses
//		-- and would drift out of step with it. Each resume re-bases the timing
//		so the emulator does not fast-forward to "catch up" on the time spent
//		halted.
//
// *******************************************************************************************

static void DEBUGClearStepBreakPoint(void) {
	// Whoever owned the pending step no longer does.
	stepOwner = DEBUG_OWNER_COUNT;
	stepBreakPoint.pc = -1;
	stepBreakPoint.bank = 0;
	stepBreakPoint.x16Bank = -1;
}

// Public form of the same thing: a DAP session tearing down has to retract a
// step it started, and nothing outside this file can reach stepBreakPoint.
void DEBUGCancelStep(void) {
	DEBUGClearStepBreakPoint();
}

debug_owner_t DEBUGStepOwner(void) {
	return stepBreakPoint.pc < 0 ? DEBUG_OWNER_COUNT : stepOwner;
}

// Retract a step only if `owner` is the one that started it.
//
// A session tearing down has to take back a step it asked for -- left armed, it
// stops the machine later for a client that has gone away -- but it must not
// take back one the user started at the keyboard. The server used to answer
// this from a flag of its own; the state being asked about lives here, so the
// question belongs here too.
bool DEBUGCancelStepFor(debug_owner_t owner) {
	if (DEBUGStepOwner() != owner)
		return false;
	DEBUGClearStepBreakPoint();
	return true;
}

// F5 — run until break.
//
// One deliberate difference from before: this abandons a pending step-over
// target, which the old F5 left armed. That target only survives at all if the
// step-over was interrupted by something other than reaching it -- F12, in
// practice -- and resuming afterwards would then stop at a return address the
// user had already broken out of and said nothing more about. Continuing means
// continuing; ask for the step again if that is what you wanted.
void DEBUGContinue(void) {
	DEBUGClearStepBreakPoint();
	DEBUGArmResumeSkip();
	currentMode = DMODE_RUN;
	debugCPUClocks = clockticks6502;
	timing_init();
	// Last, so the run state and any step target are settled before a client
	// can see them: this can reach the socket, and a peer that has stopped
	// reading is dropped from inside it.
	debug_server_note_resumed();
}

void DEBUGStepInto(void) {                              // F11 — single instruction
	DEBUGArmResumeSkip();                               // step OFF a breakpoint, not into it again
	currentMode = DMODE_STEP;                           // runs once, then DEBUGGetCurrentStatus stops us
	currentPC = regs.pc;
	currentPCBank = regs.k;
	currentPCX16Bank = getCurrentBank(regs.pc, regs.k);
	debugCPUClocks = clockticks6502;
	// Last, so the run state and any step target are settled before a client
	// can see them: this can reach the socket, and a peer that has stopped
	// reading is dropped from inside it.
	debug_server_note_resumed();
}

void DEBUGStepOver(debug_owner_t owner) {                // F10 — step over calls
	// Read the opcode through the bank live for the CURRENT pc. Using the
	// stale currentPCX16Bank (set when we last stopped) misreads the opcode
	// once the mapped bank has changed, so a JSR could be missed or invented.
	int x16Bank = getCurrentBank(regs.pc, regs.k);
	int opcode = debug_read6502(regs.pc, regs.k, x16Bank);
	// $22 and $FC are JSL and JSR (abs,X) only on the 65816. On a 65C02 they
	// are NOPs -- $22 a two-byte one -- so treating them as calls set the
	// return breakpoint past the following instruction and stepping over ran
	// away instead of stopping.
	const bool is_call = (opcode == 0x20)
	                  || (regs.is65c816 && (opcode == 0xFC || opcode == 0x22));
	if (is_call) {
		const int target = (regs.pc + 3 + (opcode == 0x22)) & 0xFFFF; // JSL is 4 bytes
		stepBreakPoint.pc = target;
		stepBreakPoint.bank = regs.k;
		// Derived from the target, not from the calling instruction. A call near
		// the top of the RAM window returns into the ROM window, where the
		// caller's RAM bank number selects nothing and the breakpoint could
		// never fire. Where the two are in the same window this is the same
		// answer, and it is what stepping out and running-to already do.
		stepBreakPoint.x16Bank = debug_current_x16_bank(target, regs.k);
		stepOwner = owner;
		DEBUGArmResumeSkip();
		currentMode = DMODE_RUN;
		debugCPUClocks = clockticks6502;
		timing_init();
	} else {                                            // not a call — same as step into
		DEBUGArmResumeSkip();
		currentMode = DMODE_STEP;
		currentPC = regs.pc;
		currentPCBank = regs.k;
		currentPCX16Bank = x16Bank;
		debugCPUClocks = clockticks6502;
	}
	// Last, so the run state and any step target are settled before a client
	// can see them: this can reach the socket, and a peer that has stopped
	// reading is dropped from inside it.
	debug_server_note_resumed();
}

void DEBUGStepOut(debug_owner_t owner) {                 // run to the return address
	// The return address is not necessarily on top of the stack: by the time
	// you want to step out, the routine has usually pushed registers or locals
	// over it. So scan upward for the first plausible return frame, keyed on
	// the call that must have created it -- a pushed 16-bit value V is a return
	// address if the byte at V-2 is JSR abs / JSR (abs,X), or at V-3 is JSL.
	// (Both push return-1, so execution resumes at V+1.)
	bool     native = regs.is65c816 && !regs.e;
	uint32_t sp     = regs.sp;
	uint32_t start  = native ? (sp + 1) : (0x100 + (sp & 0xFF) + 1);
	uint32_t end    = native ? (sp + 1 + 256) : 0x200;

	int     retAddr = -1;
	uint8_t retBank = regs.k;
	for (uint32_t a = start; a + 1 < end; a++) {
		uint8_t  lo     = debug_read6502((uint16_t)a, 0, -1);
		uint8_t  hi     = debug_read6502((uint16_t)(a + 1), 0, -1);
		uint16_t pushed = (uint16_t)(lo | (hi << 8));

		// $FC is JSR (abs,X) only on the 65816; on a 65C02 it is a NOP, and
		// accepting it there turns any pushed value preceded by one into a
		// false frame. Same gate as the one stepping over uses.
		uint8_t o2 = debug_read6502((uint16_t)(pushed - 2), regs.k, -1);
		if (o2 == 0x20 || (regs.is65c816 && o2 == 0xFC)) {   // JSR abs / JSR (abs,X)
			retAddr = (pushed + 1) & 0xFFFF;
			break;
		}

		// A JSL pushes the caller's program bank below the return address, so
		// the call it came from is in THAT bank, not the one we are stopped in.
		// Reading the opcode through regs.k would inspect whatever happens to
		// sit at that address in the callee's bank -- which for a long call is
		// exactly the bank that is wrong.
		// A long call only exists on the 65816 at all.
		if (regs.is65c816 && a + 2 < end) {
			uint8_t callerBank = debug_read6502((uint16_t)(a + 2), 0, -1);
			if (debug_read6502((uint16_t)(pushed - 3), callerBank, -1) == 0x22) {   // JSL
				retAddr = (pushed + 1) & 0xFFFF;
				retBank = callerBank;
				break;
			}
		}
	}

	if (retAddr < 0) {
		// No recognisable frame — a hand-rolled stack, or we are not inside a
		// call at all. Single-step so the caller still makes progress.
		DEBUGStepInto();
		return;
	}

	stepBreakPoint.pc = retAddr;
	stepBreakPoint.bank = retBank;
	stepBreakPoint.x16Bank = getCurrentBank(stepBreakPoint.pc, retBank);
	stepOwner = owner;
	DEBUGArmResumeSkip();
	currentMode = DMODE_RUN;
	debugCPUClocks = clockticks6502;
	timing_init();
	// Last, so the run state and any step target are settled before a client
	// can see them: this can reach the socket, and a peer that has stopped
	// reading is dropped from inside it.
	debug_server_note_resumed();
}

void DEBUGPause(void) {
	DEBUGBreakToDebugger();   // records "user"
}

void DEBUGRunTo(uint16_t pc, uint8_t bank, debug_owner_t owner) {
	stepBreakPoint.pc = pc;
	stepBreakPoint.bank = bank;
	stepBreakPoint.x16Bank = getCurrentBank(pc, bank);
	stepOwner = owner;
	DEBUGArmResumeSkip();
	currentMode = DMODE_RUN;
	debugCPUClocks = clockticks6502;
	timing_init();
	// Last, so the run state and any step target are settled before a client
	// can see them: this can reach the socket, and a peer that has stopped
	// reading is dropped from inside it.
	debug_server_note_resumed();
}

bool DEBUGIsRunning(void) {
	return currentMode == DMODE_RUN;
}

bool DEBUGIsPaused(void) {
	return currentMode == DMODE_STOP;
}


// *******************************************************************************************
//
//									Handle keyboard state.
//
// *******************************************************************************************

static void DEBUGHandleKeyEvent(SDL_Keycode key, int isShift) {

	switch(key) {

		case DBGKEY_STEP:      // Single step (F11 by default)
			if (isShift) {
				DEBUGStepOut(DEBUG_OWNER_UI);
			} else {
				DEBUGStepInto();
			}
			break;

		case DBGKEY_STEPOVER:  // Step over (F10 by default)
			DEBUGStepOver(DEBUG_OWNER_UI);
			break;

		case DBGKEY_RUN:                                // F5 Runs until Break.
			DEBUGContinue();
			break;

		case DBGKEY_SETBRK:                             // F9 Set breakpoint to displayed.
			// Now a toggle over the table rather than over one slot, so F9 adds
			// a breakpoint without silently discarding the previous one.
			//
			// Deleting takes the breakpoint away whoever asked for it, rather
			// than merely dropping this debugger's own claim on it. Someone at
			// the keyboard pressing F9 on a breakpoint a DAP client happens to
			// have set expects it gone, not to watch the key do nothing because
			// an editor somewhere else still wants it.
			debug_bp_toggle_for(currentPC, (uint8_t)currentPCBank, currentPCX16Bank,
			                    DEBUG_OWNER_UI);
			break;

		case DBGKEY_HOME:                               // F1 sets the display PC to the actual one.
			currentPC = regs.pc;
			currentPCBank = regs.k;
			currentPCX16Bank = getCurrentBank(regs.pc, regs.k);
			break;

		case DBGKEY_RESET:                              // F2 reset the 6502
			reset6502(regs.is65c816);
			currentPC = regs.pc;
			currentPCBank = regs.k;
			// Derive the window bank like every other handler. The reset vector
			// puts the PC in ROM, so hardcoding "no bank" here left the display
			// disagreeing with the machine -- and F9 would then arm a
			// breakpoint for every ROM bank rather than the one on screen.
			currentPCX16Bank = getCurrentBank(regs.pc, regs.k);
			break;

		case DBGKEY_BANK_NEXT:
			currentX16Bank += 1;
			break;

		case DBGKEY_BANK_PREV:
			currentX16Bank -= 1;
			break;

		case SDLK_PAGEDOWN:
			if (isShift) {
				currentPC = (currentPC + 0x10) & 0xFFFF;
				if (currentPCBank == 0 && currentPC < 0xA000) {
					currentPCX16Bank = -1;
				}
			} else {
				if (dumpmode == DDUMP_RAM) {
					currentData = (currentData + 0x100) & (is_gen2 ? 0xFFFFFF : 0xFFFF);
				} else {
					currentData = (currentData + 0x200) & 0x1FFFF;
				}
			}
			break;

		case SDLK_PAGEUP:
			if (isShift) {
				currentPC = (currentPC - 0x10) & 0xFFFF;
				if (currentPCBank == 0 && currentPC < 0xA000) {
					currentPCX16Bank = -1;
				}
			} else {
				if (dumpmode == DDUMP_RAM) {
					currentData = (currentData - 0x100) & (is_gen2 ? 0xFFFFFF : 0xFFFF);
				} else {
					currentData = (currentData - 0x200) & 0x1FFFF;
				}
			}
			break;

		case SDLK_DOWN:
			if (isShift) {
				currentPC = (currentPC + 1) & 0xFFFF;
				if (currentPCBank == 0 && currentPC < 0xA000) {
					currentPCX16Bank = -1;
				}
			} else {
				if (dumpmode == DDUMP_RAM) {
					currentData = (currentData + 0x08) & (is_gen2 ? 0xFFFFFF : 0xFFFF);
				} else {
					currentData = (currentData + 0x10) & 0x1FFFF;
				}
			}
			break;

		case SDLK_UP:
			if (isShift) {
				currentPC = (currentPC - 1) & 0xFFFF;
				if (currentPCBank == 0 && currentPC < 0xA000) {
					currentPCX16Bank = -1;
				}
			} else {
				if (dumpmode == DDUMP_RAM) {
					currentData = (currentData - 0x08) & (is_gen2 ? 0xFFFFFF : 0xFFFF);
				} else {
					currentData = (currentData - 0x10) & 0x1FFFF;
				}
			}
			break;

		default:
			if(DEBUGEditCmdLine(key)) {
				// printf("cmd line: %s\n", cmdLine);
				DEBUGExecCmd();
			}
			break;
	}

}

static void DEBUGBuildCmdLine(char *text) {
	uint8_t *ptr = (uint8_t *)text;
	while(currentLineLen < sizeof(cmdLine)-1 && *ptr != 0) {
		if (*ptr < 0x80) { // We only care about characters in ASCII range.
		                   // UTF-8 encodings will always have the high bit
		                   // set for any byte which is part of a multibyte
		                   // character, so we need not parse it whatsoever.
			uint8_t key = *ptr;

			if (key >= 0x20 && key <= 0x7D) {
				if (key >= 'A' && key <= 'Z') {
					key += 0x20; // case-fold to lower
				}
				cmdLine[currentPosInLine++]= key;
				if(currentPosInLine > currentLineLen) {
					currentLineLen++;
					cmdLine[currentLineLen]= 0;
				}
			}
		}
		ptr++;
	}
}

static bool DEBUGEditCmdLine(SDL_Keycode key) {
	// right now, let's have a rudimentary input: only backspace to delete last char
	// later, I want a real input line with delete, backspace, left and right cursor
	// devs like their comfort ;)
	if(key == SDLK_BACKSPACE) {
		currentPosInLine--;
		if(currentPosInLine<0) {
			currentPosInLine= 0;
		}
		currentLineLen--;
		if(currentLineLen<0) {
			currentLineLen= 0;
		}
		cmdLine[currentLineLen]= 0;
	}
	return (key == SDLK_RETURN) || (key == SDLK_KP_ENTER);
}

static void DEBUGExecCmd() {
	int number, bnumber, addr, size, incr;
	char reg[10];
	char cmd;
	char *line= ltrim(cmdLine);

	cmd= *line;
	if(*line) {
		line++;
	}
	// printf("cmd:%c line: '%s'\n", cmd, line);

	switch (cmd) {
		case CMD_DUMP_MEM:
			if (sscanf(line, "%x:%x", &bnumber, &number) == 2) {
				currentX16Bank = bnumber & 0xFF;
			} else if (sscanf(line, "%x", &number) == 1) {
				if (!is_gen2) {
					currentX16Bank = (number >> 16) & 0xFF;
				}
			} else {
				break;
			}
			addr = number & (is_gen2 ? 0xFFFFFF : 0xFFFF);
			currentData = addr;
			dumpmode    = DDUMP_RAM;
			break;

		case CMD_DUMP_VERA:
			if (sscanf(line, "%x", &number) == 1) {
				addr = number & 0x1FFFF;
				currentData = addr;
				dumpmode    = DDUMP_VERA;
			}
			break;

		case CMD_FILL_MEMORY:
			size = 1;
			incr = 1;
			if (sscanf(line, "%x %x %d %d", &addr, &number, &size, &incr) >= 2) {
				if (dumpmode == DDUMP_RAM) {
					addr &= 0xFFFFFF;
					do {
						if (addr >= 0xC000 && addr < 0x10000) {
							// Nop.
						} else if (addr >= 0xA000 && addr < 0xC000) {
							// -1 means "whichever bank is mapped", as it does
							// everywhere else this is used, so resolve it before
							// bounds-checking rather than rejecting it -- the
							// initial value is -1 and refusing it would make the
							// commonest case silently do nothing.
							//
							// Bounds-checked like the RAM branch below, which it
							// was not: the bank is taken from user input with
							// only a & 0xFF, so an out-of-range one indexed past
							// the end of the allocation, and the unset -1 indexed
							// before its start.
							const int fill_bank = currentX16Bank < 0
							                          ? (int)memory_get_ram_bank()
							                          : currentX16Bank;
							if (fill_bank >= 0 && fill_bank < (int)num_ram_banks) {
								BRAM[(fill_bank << 13) + addr - 0xA000] = number;
							}
						} else if ((addr >> 16) < num_banks) {
							RAM[addr] = number;
						}
						if (incr) {
							addr += incr;
						} else {
							++addr;
						}
						addr &= 0xFFFFFF;
						--size;
					} while (size > 0);
				} else {
					addr &= 0x1FFFF;
					do {
						video_space_write(addr, number);
						if (incr) {
							addr += incr;
						} else {
							++addr;
						}
						addr &= 0x1FFFF;
						--size;
					} while (size > 0);
				}
			}
			break;

		case CMD_DISASM:
			if (sscanf(line, "%x:%x", &bnumber, &number) == 2) {
				currentPCX16Bank = (number >= 0xA000 && number <= 0xFFFF) ? bnumber & 0xFF : -1;
			} else if (sscanf(line, "%x", &number) == 1) {
				if (!is_gen2) {
					currentPCX16Bank = (number & 0xFFFF) >= 0xA000 ? (number >> 16) & 0xFF : -1;
				} else if (number < 0x00A000 || number >= 0x010000) { // treat input as 24-bit (K+PC)
					currentPCX16Bank = -1; // if outside of the X16 banked range, set to undef (-1)
				}
			} else {
				break;
			}

			addr = number & (is_gen2 ? 0xFFFFFF : 0xFFFF);

			currentPC = addr & 0xFFFF;
			if (is_gen2) {
				currentPCBank = (addr >> 16) & 0xFF;
			}
			break;

		case CMD_SET_BANK:
			if (sscanf(line, "%s %d", reg, &number) != 2) {
				break;
			}

			if (!strcmp(reg, "rom")) {
				memory_set_rom_bank(number & 0x00FF);
			} else if (!strcmp(reg, "ram")) {
				memory_set_ram_bank(number & 0x00FF);
			}
			break;

		case CMD_SET_REGISTER:
			if (sscanf(line, "%s %x", reg, &number) != 2) {
				break;
			}

			if(!strcmp(reg, "pc")) {
				regs.pc= number & 0xFFFF;
				waiting = 0;
			}
			if(!strcmp(reg, "a")) {
				regs.a= number & 0x00FF;
			}
			if(!strcmp(reg, "b")) {
				regs.b= number & 0x00FF;
			}
			if(!strcmp(reg, "c")) {
				regs.c= number & 0xFFFF;
			}
			if(!strcmp(reg, "d")) {
				regs.dp= number & 0xFFFF;
			}
			if(!strcmp(reg, "k")) {
				regs.k= number & 0x00FF;
			}
			if(!strcmp(reg, "dbr")) {
				regs.db= number & 0x00FF;
			}
			if(!strcmp(reg, "x")) {
				if (regs.e) {
					regs.xl= number & 0x00FF;
				} else {
					regs.x= number & 0xFFFF;
				}
			}
			if(!strcmp(reg, "y")) {
				if (regs.e) {
					regs.yl= number & 0x00FF;
				} else {
					regs.y= number & 0xFFFF;
				}
			}
			if(!strcmp(reg, "sp")) {
				if (regs.e) {
					regs.sp= 0x100 | (number & 0x00FF);
				} else {
					regs.sp= number & 0xFFFF;
				}
			}
			break;

		default:
			break;
	}

	currentPosInLine= currentLineLen= *cmdLine= 0;
}

// *******************************************************************************************
//
//							Render the emulator debugger display.
//
// *******************************************************************************************

void DEBUGRenderDisplay(int width, int height) {
	if (showFullDisplay) return;								// Not rendering debug.

	SDL_Rect rc;
	rc.w = DBG_WIDTH * 6 * CHAR_SCALE;							// Erase background, set up rect
	rc.h = height;
	xPos = width-rc.w;yPos = 0; 								// Position screen
	rc.x = xPos;rc.y = yPos; 									// Set rectangle and black out.
	SDL_SetRenderDrawColor(dbgRenderer,0,0,0,SDL_ALPHA_OPAQUE);
	SDL_RenderFillRect(dbgRenderer,&rc);

	int register_lines = DEBUGRenderRegisters();							// Draw register name and values.
	DEBUGRenderCode(register_lines, currentPC);							// Render 6502 disassembly.
	if (dumpmode == DDUMP_RAM) {
		DEBUGRenderData(register_lines + 1, currentData);
		int zp_lines = DEBUGRenderZeroPageRegisters(register_lines + 1);
		DEBUGRenderVERAState(zp_lines + 1);
	} else {
		DEBUGRenderVRAM(register_lines + 1, currentData);
	}
	DEBUGRenderStack(register_lines);

	DEBUGRenderCmdLine(xPos, rc.w, height);
}

// *******************************************************************************************
//
//									 Render command Line
//
// *******************************************************************************************

static void DEBUGRenderCmdLine(int x, int width, int height) {
	char buffer[sizeof(cmdLine)+1];

	SDL_SetRenderDrawColor(dbgRenderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
	SDL_RenderDrawLine(dbgRenderer, x, height-12, x+width, height-12);

	sprintf(buffer, ">%s", cmdLine);
	DEBUGString(dbgRenderer, 0, DBG_HEIGHT-1, buffer, col_cmdLine);
	DEBUGString(dbgRenderer, currentPosInLine+1, DBG_HEIGHT-1, "_", col_cmdLine); // crude cursor
}

// *******************************************************************************************
//
//									 Render Zero Page Registers
//
// *******************************************************************************************

static int DEBUGRenderZeroPageRegisters(int y) {
#define LAST_R 15
	unsigned char reg = 0;
	int y_start = y;
	char lbl[12];
	while (reg < DBGMAX_ZERO_PAGE_REGISTERS) {
		if (((y-y_start) % 5) != 0) {           // Break registers into groups of 5, easier to locate
			if (reg <= LAST_R)
				sprintf(lbl, "R%d", reg);
			else
				sprintf(lbl, "x%d", reg);

			DEBUGString(dbgRenderer, DBG_ZP_REG, y, lbl, col_label);

			int reg_addr = 2 + reg * 2;
			int n = debug_read6502(direct_page_add(reg_addr+1), 0, USE_CURRENT_X16_BANK)*256+debug_read6502(direct_page_add(reg_addr), 0, USE_CURRENT_X16_BANK);

			DEBUGNumber(DBG_ZP_REG+5, y, n, 4, col_data);

			if (oldRegChange[reg] != NULL)
				DEBUGString(dbgRenderer, DBG_ZP_REG+9, y, oldRegChange[reg], col_data);

			if (oldRegisterTicks != clockticks6502) {   // change detection only when the emulated CPU changes
				oldRegChange[reg] = n != oldRegisters[reg] ? "*" : " ";
				oldRegisters[reg]=n;
			}
			reg++;
		}
		y++;
	}

	if (oldRegisterTicks != clockticks6502) {
		oldRegisterTicks = clockticks6502;
	}

	return y;
}

// *******************************************************************************************
//
//									 Render Data Area
//
// *******************************************************************************************

static void DEBUGRenderData(int y,uint32_t data) {
	while (y < DBG_HEIGHT-2) {									// To bottom of screen
		DEBUGAddress(DBG_MEMX, y, (uint8_t)currentX16Bank, data & 0xFFFF, data >> 16, col_label);	// Show label.

		for (int i = 0;i < 8;i++) {
			bool isDP = (data >> 16) == 0 && ((data+i - regs.dp) & 0xffff) < 256;
			int byte = debug_read6502((data+i) & 0xFFFF, data >> 16, currentX16Bank);
			DEBUGNumber(DBG_MEMX+8+i*3,y,byte,2, isDP ? col_directpage : col_data);
			DEBUGWrite(dbgRenderer, DBG_MEMX+33+i,y,byte, isDP ? col_directpage : col_data);
		}
		y++;
		data = (data + 8) & 0xFFFFFF;
	}
}

static void DEBUGRenderVRAM(int y, int data) {
	while (y < DBG_HEIGHT - 2) {                                                   // To bottom of screen
		DEBUGVAddress(DBG_MEMX, y, data & 0x1FFFF, col_label); // Show label.

		for (int i = 0; i < 16; i++) {
			int addr = (data + i) & 0x1FFFF;
			int byte = video_space_read(addr);

			if (video_is_tilemap_address(addr)) {
				DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, col_vram_tilemap);
			} else if (video_is_tiledata_address(addr)) {
				DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, col_vram_tiledata);
			} else if (video_is_special_address(addr)) {
				DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, col_vram_special);
			} else {
				DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, col_vram_other);
			}
		}
		y++;
		data += 16;
	}
}

// *******************************************************************************************
//
//									 Render Disassembly
//
// *******************************************************************************************


static void DEBUGRenderCode(int lines, int initialPC) {
	char buffer[32];
	uint8_t implied_status = regs.status;
	uint8_t implied_e = regs.e;
	uint8_t opcode, operand, carry;
	int implied_x16_bank = currentPCX16Bank;
	// Which window that bank belongs to. A bank number only means something
	// inside the window it names -- a RAM bank does not select a ROM bank -- so
	// when the walk crosses out of it the carried value is re-derived from what
	// is mapped there instead of being carried on regardless.
	int implied_window = bank_window_of(initialPC);

	for (int y = 0; y < lines; y++) { 							// Each line
		DEBUGAddress(DBG_ASMX, y, implied_x16_bank, initialPC, currentPCBank, col_label);
		int32_t eff_addr;

		// Attempt to display the disassembly correctly more often
		// if the code logic is reasonably straightforward with respect
		// to status flags changing in the immediate instruction list

		// This doesn't predict status flags changes except by the few
		// opcodes below. The PLP instruction, for instance, could easily
		// render disassembly following it invalid, but this would have
		// still been true without the added logic, anyway.

		if (regs.is65c816) {
			// Read through the bank this line is labelled with, not the one the
			// pane started in. This scan predicts the M/X widths, which decide
			// how many bytes the next immediate instruction occupies -- so
			// reading it from a different bank than the one being disassembled
			// mis-sizes the instruction and every line after it lands at the
			// wrong offset.
			opcode = debug_read6502(initialPC, currentPCBank, implied_x16_bank);
			switch (opcode) {
				case 0x81: // CLC
					implied_status &= ~FLAG_CARRY;
					;;
				case 0x83: // SEC
					implied_status |= FLAG_CARRY;
					;;
				case 0xC2: // REP
					operand = debug_read6502((initialPC+1) & 0xffff, currentPCBank, implied_x16_bank);
					implied_status = ~operand & implied_status;
					;;
				case 0xE2: // SEP
					operand = debug_read6502((initialPC+1) & 0xffff, currentPCBank, implied_x16_bank);
					implied_status = operand | implied_status;
					;;
				case 0xFB: // XCE
					carry = implied_status & FLAG_CARRY;
					implied_status = (implied_status & ~FLAG_CARRY) | (implied_e ? FLAG_CARRY : 0);
					implied_e = carry != 0;
					;;
				default:
					;;
			}
			if (implied_e) implied_status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;

		}
		int size = disasm(initialPC, currentPCBank, RAM, buffer, sizeof(buffer), implied_x16_bank, implied_status, &eff_addr);	// Disassemble code
		// Output assembly highlighting PC
		DEBUGString(dbgRenderer, DBG_ASMX+8, y, buffer, initialPC == regs.pc ? col_highlight : col_data);
		// Populate effective address
		if (initialPC == regs.pc) {
			if (eff_addr < 0) {
				DEBUGString(dbgRenderer, DBG_DATX, lines-1, "----", col_data);
			} else {
				DEBUGNumber(DBG_DATX, lines-1, eff_addr, 4, col_data);
			}
		}
		initialPC = (initialPC + size) & 0xFFFF;										// Forward to next
		// Re-derive on leaving the window the carried bank belongs to. Without
		// this the bank the pane started with is used for every line, so a walk
		// from the RAM window into the ROM window selects a ROM bank by its RAM
		// bank number and renders bytes from the wrong one.
		if (bank_window_of(initialPC) != implied_window) {
			implied_window   = bank_window_of(initialPC);
			implied_x16_bank = debug_current_x16_bank(initialPC, currentPCBank);
		}
	}
}

// *******************************************************************************************
//
//									Render Register Display
//
// *******************************************************************************************

static char *labels_c816[] = { "NVMXDIZCE","","","A","B","C","X","Y","K","DB","","PC","DP","SP","BKA","BKO","","BRK","EFF", NULL };
static char *labels_c02[] = { "NV-BDIZC","","","A","X","Y","","PC","SP","BKA","BKO","","BRK","EFF", NULL };

static void DEBUGNumberHighByteCondition(int x, int y, int n, bool condition, SDL_Color ifTrue, SDL_Color ifFalse) {
	if (condition) {
		DEBUGNumber(x, y, n >> 8, 2, ifTrue);
		DEBUGNumber(x + 2, y, n & 0xFF, 2, ifFalse);
	} else {
		DEBUGNumber(x, y, n, 4, ifFalse);
	}
}

static int DEBUGRenderRegisters(void) {
	int n = 0,yc = 0;
	if (regs.is65c816) {
		while (labels_c816[n] != NULL) {								// Labels
			DEBUGString(dbgRenderer, DBG_LBLX,n,labels_c816[n], col_label);n++;
		}
		yc++;
		DEBUGNumber(DBG_LBLX, yc, (regs.status >> 7) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+1, yc, (regs.status >> 6) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+2, yc, (regs.status >> 5) & 1, 1, regs.e ? col_vram_other : col_data);
		DEBUGNumber(DBG_LBLX+3, yc, (regs.status >> 4) & 1, 1, regs.e ? col_vram_other : col_data);
		DEBUGNumber(DBG_LBLX+4, yc, (regs.status >> 3) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+5, yc, (regs.status >> 2) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+6, yc, (regs.status >> 1) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+7, yc, (regs.status >> 0) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+8, yc, regs.e, 1, col_data);
		yc+= 2;

		DEBUGNumber(DBG_DATX, yc++, regs.a, 2, col_data);
		DEBUGNumber(DBG_DATX, yc++, regs.b, 2, col_data);
		DEBUGNumber(DBG_DATX, yc++, regs.c, 4, col_data);
		DEBUGNumberHighByteCondition(DBG_DATX, yc++, regs.x, (regs.status >> 4) & 1, col_vram_other, col_data);
		DEBUGNumberHighByteCondition(DBG_DATX, yc++, regs.y, (regs.status >> 4) & 1, col_vram_other, col_data);
		DEBUGNumber(DBG_DATX, yc++, regs.k, 2, col_data);
		DEBUGNumber(DBG_DATX, yc++, regs.db, 2, col_data);
		yc++;

		DEBUGNumber(DBG_DATX, yc++, regs.pc, 4, col_data);
		DEBUGNumber(DBG_DATX, yc++, regs.dp, 4, col_data);
		DEBUGNumberHighByteCondition(DBG_DATX, yc++, regs.sp, regs.e, col_vram_other, col_data);
		DEBUGNumber(DBG_DATX, yc++, memory_get_ram_bank(), 2, col_data);
		DEBUGNumber(DBG_DATX, yc++, memory_get_rom_bank(), 2, col_data);
		yc++;
	} else {
		while (labels_c02[n] != NULL) {									// Labels
			DEBUGString(dbgRenderer, DBG_LBLX,n,labels_c02[n], col_label);n++;
		}
		yc++;
		DEBUGNumber(DBG_LBLX, yc, (regs.status >> 7) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+1, yc, (regs.status >> 6) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+3, yc, (regs.status >> 4) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+4, yc, (regs.status >> 3) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+5, yc, (regs.status >> 2) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+6, yc, (regs.status >> 1) & 1, 1, col_data);
		DEBUGNumber(DBG_LBLX+7, yc, (regs.status >> 0) & 1, 1, col_data);
		yc+= 2;

		DEBUGNumber(DBG_DATX, yc++, regs.a, 2, col_data);
		DEBUGNumber(DBG_DATX, yc++, regs.xl, 2, col_data);
		DEBUGNumber(DBG_DATX, yc++, regs.yl, 2, col_data);
		yc++;

		DEBUGNumber(DBG_DATX, yc++, regs.pc, 4, col_data);
		DEBUGNumber(DBG_DATX, yc++, regs.sp|0x100, 4, col_data);
		DEBUGNumber(DBG_DATX, yc++, memory_get_ram_bank(), 2, col_data);
		DEBUGNumber(DBG_DATX, yc++, memory_get_rom_bank(), 2, col_data);
		yc++;

	}

	// The panel has room for one breakpoint but there can now be several, so
	// show the first and how many more there are rather than pretending the
	// others do not exist.
	if (numBreakpoints == 0) {
		DEBUGString(dbgRenderer, DBG_DATX, yc++, "--:----", col_data);
	} else {
		const struct breakpoint *bp = &breakPoints[0];
		if (bp->x16Bank < 0) {
			if (is_gen2) {
				DEBUGNumber(DBG_DATX, yc, bp->bank, 2, col_data);
				DEBUGNumber(DBG_DATX+3, yc, bp->pc, 4, col_data);
			} else {
				DEBUGString(dbgRenderer, DBG_DATX, yc, "--:", col_data);
				DEBUGNumber(DBG_DATX+3, yc, (uint16_t)bp->pc, 4, col_data);
			}
		} else {
			DEBUGNumber(DBG_DATX, yc, bp->x16Bank, 2, col_data);
			DEBUGString(dbgRenderer, DBG_DATX+2, yc, ":", col_data);
			DEBUGNumber(DBG_DATX+3, yc, bp->pc, 4, col_data);
		}
		if (numBreakpoints > 1) {
			char more[16];
			snprintf(more, sizeof(more), "+%d", numBreakpoints - 1);
			DEBUGString(dbgRenderer, DBG_DATX+8, yc, more, col_data);
		}
		yc++;
	}
	yc++;

	return n; 													// Number of code display lines
}


static char *vera_labels[] = { "ADDR0", "ADDR1", "DATA0","DATA1", "CTRL", "VIDEO", "HSCLE", "VSCLE", "FXCTL", "FXMUL", "CACHE", "ACCUM", "", "CLOCKS ELAPSED", NULL };

static void DEBUGRenderVERAState(int y) {
	int n=0;
	int yc=y;
	while (vera_labels[n] != NULL) { // Labels
		DEBUGString(dbgRenderer, DBG_VERA_REGX, yc, vera_labels[n], col_label);n++;yc++;
	}

	yc=y;

	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_get_address(0), 5, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_get_address(1), 5, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_read(3, true), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_read(4, true), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_read(5, true), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_get_dc_value(0), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_get_dc_value(1), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_get_dc_value(2), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_get_dc_value(8), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_get_dc_value(11), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc, video_get_dc_value(24), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+8, yc, video_get_dc_value(25), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+10, yc, video_get_dc_value(26), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+12, yc++, video_get_dc_value(27), 2, col_data);
	DEBUGNumber(DBG_VERA_REGX+6, yc++, video_get_fx_accum(), 8, col_data);

	yc+=2;
	DEBUGNumberDec(DBG_VERA_REGX, yc++, clockticks6502 - debugCPUClocks, 14, col_data);
}

// *******************************************************************************************
//
//									Render Top of Stack
//
// *******************************************************************************************

static void DEBUGRenderStack(int bytesCount) {
	uint16_t sp = regs.sp;
	increment_wrap_at_page_boundary(&sp);

	int y= 0;
	while (y < bytesCount) {
		DEBUGNumber(DBG_STCK,y, sp,4, col_label);
		int byte = debug_read6502(sp, 0, USE_CURRENT_X16_BANK);
		DEBUGNumber(DBG_STCK+5,y,byte,2, col_data);
		DEBUGWrite(dbgRenderer, DBG_STCK+9,y,byte, col_data);
		y++;
		increment_wrap_at_page_boundary(&sp);
	}
}

// *******************************************************************************************
//
//									Write Hex Constant
//
// *******************************************************************************************

static void DEBUGNumber(int x, int y, int n, int w, SDL_Color colour) {
	char fmtString[8],buffer[16];
	snprintf(fmtString, sizeof(fmtString), "%%0%dX", w);
	snprintf(buffer, sizeof(buffer), fmtString, n);
	DEBUGString(dbgRenderer, x, y, buffer, colour);
}

// *******************************************************************************************
//
//					Write Decimal Constant with thousands separator
//
// *******************************************************************************************

static void DEBUGNumberDec(int x, int y, int n, int w, SDL_Color colour) {
	char buf1[32], buf2[32];
	int i,j;
	snprintf(buf1, sizeof(buf1), "%d", n);
	buf2[sizeof(buf2)-1] = 0; // null terminate string
	int count = 0;
	for (i=strlen(buf1) - 1, j=sizeof(buf2) - 1; i >= 0 && j > 1; i--) {
		buf2[--j] = buf1[i];
		count++;

		if (count == 3) {
			buf2[--j] = ' ';
			count = 0;
		}
	}

	if (buf2[j] == ' ') j++;
	DEBUGString(dbgRenderer, x+(w-strlen(buf2+j)), y, buf2+j, colour);
}

// *******************************************************************************************
//
//									Write Bank:Address
//
// *******************************************************************************************
static void DEBUGAddress(int x, int y, int x16Bank, int addr, uint8_t bank, SDL_Color colour) {
	char buffer[4];

	// Whether a window bank means anything here is the shared rule's decision,
	// not a fourth local copy of it. The one that used to be here required the
	// program bank to be zero, so on gen1 with -c816 it rendered "--:" at an
	// address the window registers really do select.
	if (debug_current_x16_bank(addr, bank) >= 0) {
		snprintf(buffer, sizeof(buffer), "%.2X:", x16Bank);
	} else if (is_gen2) {
		snprintf(buffer, sizeof(buffer), "%.2X ", bank);
	} else {
		strcpy(buffer, "--:");
	}

	DEBUGString(dbgRenderer, x, y, buffer, colour);

	DEBUGNumber(x+3, y, addr, 4, colour);

}

static void
DEBUGVAddress(int x, int y, int addr, SDL_Color colour)
{
	DEBUGNumber(x, y, addr, 5, colour);
}
