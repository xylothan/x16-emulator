// Link seam for testing the real video.c.
//
// video.c is the VERA implementation and the emulator's SDL host rolled into
// one file. The emulation half -- register decode, the data ports, address
// increment, the layer and sprite arithmetic, video_step() and the framebuffer
// -- touches no SDL at all. The host half owns the window, the event pump and
// the debugger overlay.
//
// The tests want the first half and nothing else, so this supplies everything
// the second half refers to. Every function here is inert: a test that reaches
// one has left emulation and gone into presentation, and wants rewriting rather
// than accommodating. Nothing records, because nothing here should be called.
//
// video_init(), video_update() and video_end() are therefore never called by a
// test. video_reset() is, and is deterministic.
//
// The audio modules are linked for real rather than faked. video.c routes
// $9F3B-$9F3D straight into them, they have no dependencies of their own, and
// they already have their own tests -- so a fake would add a second thing to
// keep true for no benefit.

#include "video_fixture.h"

#include "cpu/registers.h"
#include "glue.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ─── Machine state video.c reads ────────────────────────────────────────────

struct regs regs;

uint8_t  activity_led;
uint8_t  MHZ = 8;
uint16_t opcode_addr;
bool     warp_mode;
bool     testbench;
bool     enable_midline;
bool     grab_mouse;
bool     is_gen2;
bool     debugger_enabled;
bool     imgui_debugger_enabled;
bool     disable_emu_cmd_keys;
bool     log_video;
// An enum, not a bool: declaring it too narrow leaves the linker with a symbol
// smaller than the reads against it (MSVC C4739).
gif_recorder_state_t record_gif = RECORD_GIF_PAUSE;
char    *gif_path;
char     window_title[64] = "x16 test";
int      showDebugOnRender;

// ─── Host side: window, input, lifecycle ────────────────────────────────────

void machine_dump(const char *reason) { (void)reason; }
void machine_nmi(void) { }
void machine_paste(char *text, bool handle_free) { (void)text; (void)handle_free; }
void machine_reset(void) { }
void machine_toggle_warp(void) { }
void main_shutdown(void) { }

void handle_keyboard(bool down, SDL_Keycode sym, SDL_Scancode scancode)
	{ (void)down; (void)sym; (void)scancode; }

void joystick_add(int index) { (void)index; }
void joystick_remove(int index) { (void)index; }
void joystick_button_down(int instance_id, uint8_t button) { (void)instance_id; (void)button; }
void joystick_button_up(int instance_id, uint8_t button) { (void)instance_id; (void)button; }

void mouse_button_down(int num) { (void)num; }
void mouse_button_up(int num) { (void)num; }
void mouse_move(int x, int y) { (void)x; (void)y; }
void mouse_send_state(void) { }
void mouse_set_wheel(int8_t y) { (void)y; }

void sdcard_attach(void) { }
void sdcard_detach(void) { }

// vera_spi.c drives the card through these; there is no card in a test.
bool    sdcard_attached;
void    sdcard_select(bool select) { (void)select; }
uint8_t sdcard_handle(uint8_t inbyte) { (void)inbyte; return 0xFF; }

SDL_Surface *CommanderX16Icon(void) { return NULL; }

// video.c declares these itself under _WIN32 and calls them from window setup.
#ifdef _WIN32
void video_win32_set_rounded_corners(SDL_Window *window) { (void)window; }
void video_win32_install_move_hook(SDL_Window *window) { (void)window; }
void video_win32_remove_move_hook(void) { }
#endif

// ─── Debugger overlay ───────────────────────────────────────────────────────

void DEBUGBreakToDebugger(void) { }
void DEBUGFreeUI(void) { }
void DEBUGInitUI(SDL_Renderer *r) { (void)r; }
void DEBUGRenderDisplay(int width, int height) { (void)width; (void)height; }
void DEBUGSetStopReason(const char *reason) { (void)reason; }

// ─── Audio mixing ───────────────────────────────────────────────────────────
//
// video.c calls this before every audio register access so the mixer catches up
// to the current cycle. The PSG and PCM modules themselves are linked for real;
// only the pull from the audio backend is stubbed, since there is no device to
// render into.

void audio_render(void) { }

