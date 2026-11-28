// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#ifndef __APPLE__
#ifndef _MSC_VER
#define _XOPEN_SOURCE   600
#define _POSIX_C_SOURCE 1
#endif
#endif

#include "video.h"
#include "memory.h"
#include "glue.h"
#include "debugger.h"
#include "debug_server.h"
#include "input_log.h"
#ifdef HAS_IMGUI
#include "smc.h" // smc_requested_reset (Ctrl+Shift+F5 restart)
#include "dbg_load.h" // dbg_load_note_debugger, when the UI fails to open
#include "debug_ui/debug_ui.h"
#endif
#include "host_prof.h"
#include "keyboard.h"
#include "gif.h"
#include "joystick.h"
#include "vera_spi.h"
#include "vera_psg.h"
#include "vera_pcm.h"
#include "sprite_trace.h"
#include "vera_bandwidth.h"
#include "icon.h"
#include "sdcard.h"
#include "i2c.h"
#include "audio.h"
#include "audio_events.h"
#include "render_backend.h"
#include "x16_hash.h"
#include "savestate.h"
#include "state_slots.h"
#include "timing.h"
#include "hostkey.h"

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>
#include "compat.h"
#include "export_paths.h"

#ifdef __EMSCRIPTEN__
#include "emscripten.h"
#endif

#ifndef __EMSCRIPTEN__
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

#define APPROX_TITLEBAR_HEIGHT 30

#define VERA_VERSION_MAJOR  49
#define VERA_VERSION_MINOR  0
#define VERA_VERSION_PATCH  6

#define ADDR_VRAM_START     0x00000
#define ADDR_VRAM_END       0x20000
#define ADDR_PSG_START      0x1F9C0
#define ADDR_PSG_END        0x1FA00
#define ADDR_PALETTE_START  0x1FA00
#define ADDR_PALETTE_END    0x1FC00
#define ADDR_SPRDATA_START  0x1FC00
#define ADDR_SPRDATA_END    0x20000

#define NUM_SPRITES 128

// Clocks the sprite renderer gets per scanline before both of its state
// machines are forced to DONE.
//
//   sprite_renderer.v:44  wire render_time_done = (render_time_r == 'd798);
//
// render_time_r is cleared by line_render_start and counts every clock after,
// so this is elapsed time for the line rather than an amount of work. The
// comment above it in the RTL gives the reason for the number: "Limit render
// time so that VGA and composite mode get the same amount of render time".
#define SPRITE_RENDER_TIME 798

// both VGA and NTSC
#define SCAN_HEIGHT 525
#define PIXEL_FREQ 25.0

// VGA
#define VGA_SCAN_WIDTH 800
#define VGA_Y_OFFSET 0

// NTSC: 262.5 lines per frame, lower field first
#define NTSC_HALF_SCAN_WIDTH 794
#define NTSC_X_OFFSET 270
#define NTSC_Y_OFFSET_LOW 42
#define NTSC_Y_OFFSET_HIGH 568
#define TITLE_SAFE_X 0.067
#define TITLE_SAFE_Y 0.05

// visible area we're drawing
#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480

#define SCREEN_RAM_OFFSET 0x00000

#ifdef __APPLE__
#define LSHORTCUT_KEY SDL_SCANCODE_LGUI
#define RSHORTCUT_KEY SDL_SCANCODE_RGUI
#define SHORTCUT_MOD  KMOD_GUI
#else
#define LSHORTCUT_KEY SDL_SCANCODE_LCTRL
#define RSHORTCUT_KEY SDL_SCANCODE_RCTRL
#define SHORTCUT_MOD  KMOD_CTRL
#endif

// Upstream's Ctrl (Cmd) shortcuts, as well as the host key's: -ctrlkeys.
// See video_set_host_key().
static bool ctrl_shortcuts     = false;
static bool host_key_from_cli  = false;
static bool ctrl_keys_from_cli = false;
static const char *video_title_status(void);

// When rendering a layer line, we can amortize some of the cost by calculating multiple pixels at a time.
#define LAYER_PIXELS_PER_ITERATION 8

#define MAX(a,b) ((a) > (b) ? a : b)

static SDL_Window *window;
static render_backend_t *renderer;
static render_texture_t *sdlTexture;
static float renderer_screen_x_scale = 1.0f;
static const char *renderer_quality = "best";
static bool is_fullscreen = false;
static bool legacy_debug_ui_initialized = false;
#ifdef HAS_IMGUI
// ImGui debugger window (separate; created only when -imgui is passed).
static SDL_Window *imgui_window = NULL;
static SDL_Renderer *imgui_renderer = NULL;
// True while the debugger window is drawn with the native DirectX 12 renderer,
// sharing the main backend's device/queue/SRV heap. Mutually exclusive with
// imgui_renderer, which stays NULL in that mode.
static bool imgui_native_dx12 = false;
static Uint32 imgui_window_id = 0;
#endif
bool mouse_grabbed = false;
bool no_keyboard_capture = false;
bool kernal_mouse_enabled = false;

static uint8_t video_ram[0x20000];

// -seed fills VRAM from a private PRNG at reset instead of rand().
static bool     vram_seeded = false;
static uint32_t vram_seed   = 0;
// -deterministic: keyboard, mouse and controller events from the host are
// dropped, so nothing outside the debug connection reaches the machine.
static bool     ignore_host_input = false;
static uint8_t palette[256 * 2];
static uint8_t sprite_data[128][8];

// I/O registers
static uint32_t io_addr[2];
static uint8_t io_rddata[2];
static uint8_t io_inc[2];
static uint8_t io_addrsel;
static uint8_t io_dcsel;

static uint8_t ien;
static uint8_t isr;

static uint16_t irq_line;
static uint8_t comp_first_frame;
static uint8_t comp_vstarted; // composer.v:139 vactive_started_r
static uint8_t line_irq_pending;

static uint8_t reg_layer[2][7];

// Per-scanline record of the layer registers that were actually used to render
// each line, so debug views can reproduce raster-split effects -- programs
// routinely rewrite MAPBASE/TILEBASE/scroll part-way down a frame from a line
// IRQ. Without this a debugger can only ever show one register snapshot for the
// whole frame, which describes just one band of the screen.
static uint8_t  line_reg_layer[SCREEN_HEIGHT][2][7];
static uint16_t line_eff_y[SCREEN_HEIGHT];
// The layer row each line actually displayed, as fetched.
static uint16_t line_layer_row[SCREEN_HEIGHT][2];
static uint8_t  line_layer_enabled[SCREEN_HEIGHT];
static bool     line_state_valid[SCREEN_HEIGHT];

#define COMPOSER_SLOTS 4*64
static uint8_t reg_composer[COMPOSER_SLOTS];

// top.v:729-739, layer_renderer.v, sprite_renderer.v: line y+1's layer and
// sprite buffers are built during line y from live registers, and composed
// during line y+1. Here they are built when line y ends, from the registers as
// it began plus the writes made during it, each at the dot it took effect.
#define FETCH_LOG_MAX 128
enum { FL_DC_VIDEO = 14, FL_VSCALE, FL_VSTART, FL_VSTOP, FL_COMP_N = 4 };
typedef struct { uint16_t dot; uint8_t reg; uint8_t value; } fetch_write_t;
typedef struct { uint16_t dot; uint8_t slot; uint8_t offset; uint8_t old; } sprite_write_t;
static uint8_t        fetch_reg_layer[2][7];
static uint8_t        fetch_reg_comp[FL_COMP_N]; // DC_VIDEO, VSCALE, VSTART, VSTOP
static fetch_write_t  fetch_log[FETCH_LOG_MAX];
static uint16_t       fetch_log_n;
static bool           fetch_log_lost;
static sprite_write_t sprite_log[FETCH_LOG_MAX];
static uint16_t       sprite_log_n;
#define FETCH_NONE 0xFFFF
static uint16_t       fetched_y = FETCH_NONE;

// layer_renderer.v, one clock at a time: the line renderer state machine and
// the pixel renderer it feeds.
enum { LR_WAIT_START, LR_FETCH_MAP, LR_WAIT_FETCH_MAP, LR_FETCH_TILE, LR_WAIT_FETCH_TILE, LR_RENDER };

struct layer_fetch {
	uint8_t  state, strobe, ack, render_start, busy, xcnt, word, htile;
	uint8_t  render_mapdata, next_render_mapdata;
	uint16_t bus_addr, ack_addr, bitmap_addr, lb_idx;
	uint32_t map_data, tile_data, render_data;
};

// Where each layer renderer was when the last line ended. render_start
// resets only the counters and the state (layer_renderer.v:319-325), so one
// that had not finished carries into the next line.
static struct layer_fetch layer_fsm[2];

// top.v:729-739: two buffers per layer, swapped every scanline.
static uint8_t layer_lb[2][2][SCREEN_WIDTH];
#define layer_line (layer_lb[line_buf_sel])
static uint8_t sprite_line_col[SCREEN_WIDTH];
static uint8_t sprite_line_z[SCREEN_WIDTH];
static uint8_t sprite_line_mask[SCREEN_WIDTH];
static uint8_t sprite_line_collisions;

// composer.v:157: no line is rendered once the scaled Y counter reaches 480.
// top.v:731-739: the two line buffers swap on every scanline, blanking included.
// composer.v:130: the sprite buffer is erased only below index 640.
static uint8_t line_buf_sel;
static uint8_t sprite_edge_col[2][2];
static uint8_t sprite_edge_z[2][2];
static uint8_t sprite_edge_mask[2][2];

// top.v:731-739, sprite_line_buffer.v: the buffers swap the clock after
// next_line, which is render_time_done's clock. Its pixel reads the entry the
// composer last addressed in the buffer just shown, and is written into that
// buffer, which is the one rendered next.
typedef struct { uint8_t on; uint16_t x; uint8_t col, z, mask; } sprite_carry_t;
static sprite_carry_t sprite_carry;
static bool layer_line_enable[2];
static bool sprite_line_enable;

////////////////////////////////////////////////////////////
// FX registers
////////////////////////////////////////////////////////////
static uint8_t fx_addr1_mode;

// These are all 16.16 fixed point in the emulator
// even though the VERA uses smaller bit widths
// for the whole and fractional parts.
//
// Sign extension is done manually when assigning negative numbers
//
// Native VERA bit widths are shown below.
static uint32_t fx_x_pixel_increment;  // 11.9 fixed point (6.9 without 32x multiplier, 11.4 with 32x multiplier on)
static uint32_t fx_y_pixel_increment;  // 11.9 fixed point (6.9 without 32x multiplier, 11.4 with 32x multiplier on)
static uint32_t fx_x_pixel_position;   // 11.9 fixed point
static uint32_t fx_y_pixel_position;   // 11.9 fixed point


static uint32_t fx_affine_tile_base;
static uint32_t fx_affine_map_base;

static uint8_t fx_affine_map_size;

static bool fx_4bit_mode;
static bool fx_16bit_hop;
static bool fx_cache_byte_cycling;
static bool fx_cache_fill;
static bool fx_cache_write;
static bool fx_trans_writes;

static bool fx_2bit_poly;
static bool fx_2bit_poking;

static bool fx_cache_increment_mode;
static bool fx_cache_nibble_index;
static uint8_t fx_cache_byte_index;
static bool fx_multiplier;
static bool fx_subtract;

static bool fx_affine_clip;

static uint8_t fx_16bit_hop_align;

static bool fx_nibble_bit[2];
static bool fx_nibble_incr[2];
static bool fx_pending_pos_calc;

// addr_data.v:352-375
static inline uint16_t
fx_fill_length(void)
{
	return (uint16_t)(((fx_y_pixel_position >> 16) - (fx_x_pixel_position >> 16)) & 0x3ff);
}

static uint8_t fx_cache[4];

static int32_t fx_mult_accumulator;

static const uint8_t vera_version_string[] = {'V',
	VERA_VERSION_MAJOR,
	VERA_VERSION_MINOR,
	VERA_VERSION_PATCH
};

float vga_scan_pos_x;
uint16_t vga_scan_pos_y;
float ntsc_half_cnt;
uint16_t ntsc_scan_pos_y;
int frame_count = 0;

// CPU cycles of the current instruction VERA has already been stepped through,
// by accesses that caught it up to their own cycle, and whether those crossed
// a frame.
static unsigned step_consumed;
static bool step_consumed_frame;

// CPU cycles elapsed, accumulated from the counts video_step() is handed. Kept
// here rather than read from clockticks6502 so that video.c does not pull the
// CPU in: the eight test targets that link this file supply a host fixture, not
// a processor. The sum is exact -- main.c passes each instruction's cycle count
// once, and the mid-line raster calls pass zero.
static uint32_t sprite_trace_cpu_cycles = 0;

// The display line the beam is on right now, for stamping attribute writes.
// SPRITE_TRACE_NO_LINE outside the active display, where a write is an ordinary
// full-frame update rather than a raster-timed one.
static uint16_t
sprite_trace_current_line(void)
{
	int y;
	if (reg_composer[0] & 2) {
		y = (ntsc_scan_pos_y < SCAN_HEIGHT) ? (int)ntsc_scan_pos_y - NTSC_Y_OFFSET_LOW
		                                    : (int)ntsc_scan_pos_y - NTSC_Y_OFFSET_HIGH;
	} else {
		y = (int)vga_scan_pos_y - VGA_Y_OFFSET;
	}
	if (y < 0 || y >= SPRITE_TRACE_LINES) {
		return SPRITE_TRACE_NO_LINE;
	}
	return (uint16_t)y;
}

static uint8_t framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT * 4];
#ifndef __EMSCRIPTEN__
static uint8_t png_buffer[SCREEN_WIDTH * SCREEN_HEIGHT * 3];
#endif

static GifWriter gif_writer;

static bool video_fallback_to_sdl(void);

static const uint16_t default_palette[] = {
0x000,0xfff,0x800,0xafe,0xc4c,0x0c5,0x00a,0xee7,0xd85,0x640,0xf77,0x333,0x777,0xaf6,0x08f,0xbbb,0x000,0x111,0x222,0x333,0x444,0x555,0x666,0x777,0x888,0x999,0xaaa,0xbbb,0xccc,0xddd,0xeee,0xfff,0x211,0x433,0x644,0x866,0xa88,0xc99,0xfbb,0x211,0x422,0x633,0x844,0xa55,0xc66,0xf77,0x200,0x411,0x611,0x822,0xa22,0xc33,0xf33,0x200,0x400,0x600,0x800,0xa00,0xc00,0xf00,0x221,0x443,0x664,0x886,0xaa8,0xcc9,0xfeb,0x211,0x432,0x653,0x874,0xa95,0xcb6,0xfd7,0x210,0x431,0x651,0x862,0xa82,0xca3,0xfc3,0x210,0x430,0x640,0x860,0xa80,0xc90,0xfb0,0x121,0x343,0x564,0x786,0x9a8,0xbc9,0xdfb,0x121,0x342,0x463,0x684,0x8a5,0x9c6,0xbf7,0x120,0x241,0x461,0x582,0x6a2,0x8c3,0x9f3,0x120,0x240,0x360,0x480,0x5a0,0x6c0,0x7f0,0x121,0x343,0x465,0x686,0x8a8,0x9ca,0xbfc,0x121,0x242,0x364,0x485,0x5a6,0x6c8,0x7f9,0x020,0x141,0x162,0x283,0x2a4,0x3c5,0x3f6,0x020,0x041,0x061,0x082,0x0a2,0x0c3,0x0f3,0x122,0x344,0x466,0x688,0x8aa,0x9cc,0xbff,0x122,0x244,0x366,0x488,0x5aa,0x6cc,0x7ff,0x022,0x144,0x166,0x288,0x2aa,0x3cc,0x3ff,0x022,0x044,0x066,0x088,0x0aa,0x0cc,0x0ff,0x112,0x334,0x456,0x668,0x88a,0x9ac,0xbcf,0x112,0x224,0x346,0x458,0x56a,0x68c,0x79f,0x002,0x114,0x126,0x238,0x24a,0x35c,0x36f,0x002,0x014,0x016,0x028,0x02a,0x03c,0x03f,0x112,0x334,0x546,0x768,0x98a,0xb9c,0xdbf,0x112,0x324,0x436,0x648,0x85a,0x96c,0xb7f,0x102,0x214,0x416,0x528,0x62a,0x83c,0x93f,0x102,0x204,0x306,0x408,0x50a,0x60c,0x70f,0x212,0x434,0x646,0x868,0xa8a,0xc9c,0xfbe,0x211,0x423,0x635,0x847,0xa59,0xc6b,0xf7d,0x201,0x413,0x615,0x826,0xa28,0xc3a,0xf3c,0x201,0x403,0x604,0x806,0xa08,0xc09,0xf0b
};

uint8_t video_space_read(uint32_t address);
static void video_space_read_range(uint8_t* dest, uint32_t address, uint32_t size);

static void refresh_palette();

void
mousegrab_toggle() {
	mouse_grabbed = !mouse_grabbed;
	SDL_SetWindowGrab(window, mouse_grabbed && !no_keyboard_capture);
	SDL_SetRelativeMouseMode(mouse_grabbed);
	SDL_ShowCursor((mouse_grabbed || kernal_mouse_enabled) ? SDL_DISABLE : SDL_ENABLE);
	timing_refresh_title();
}

// Re-derive the layer properties from the raw layer registers and seed both
// generations of the render pipeline from them. Called on reset: the derived
// properties are otherwise refreshed only when the guest writes a layer
// register, so clearing reg_layer alone left the derived state describing the
// pre-reset machine while the registers read zero -- and a debug view
// correlating the two would pair zeroed registers with stale geometry.
static void video_reset_layer_pipeline(void);
static uint16_t write_dot(void);
static void refresh_sprite_properties(const uint16_t sprite);

void
video_reset()
{
	// Drop the per-scanline debug history: it describes a machine that no
	// longer exists. Both pipeline generations are re-seeded from the cleared
	// registers below, so raw and derived state stay matched rather than one
	// side reading zero while the other still holds pre-reset geometry.
	memset(line_state_valid, 0, sizeof(line_state_valid));
	step_consumed = 0;
	step_consumed_frame = false;

	// init I/O registers
	memset(io_addr, 0, sizeof(io_addr));
	memset(io_inc, 0, sizeof(io_inc));
	io_addrsel = 0;
	io_dcsel = 0;
	io_rddata[0] = 0;
	io_rddata[1] = 0;

	ien = 0;
	isr = 0;
	irq_line = 0;

	// init Layer registers
	memset(reg_layer, 0, sizeof(reg_layer));
	video_reset_layer_pipeline();

	// init composer registers
	memset(reg_composer, 0, sizeof(reg_composer));
	reg_composer[1] = 128; // hscale = 1.0
	reg_composer[2] = 128; // vscale = 1.0
	reg_composer[5] = 640 >> 2;
	reg_composer[7] = 480 >> 1;

	// Initialize FX registers
	fx_addr1_mode = 0;
	fx_x_pixel_position = 0x8000;
	fx_y_pixel_position = 0x8000;
	fx_x_pixel_increment = 0;
	fx_y_pixel_increment = 0;

	fx_cache_write = false;
	fx_cache_fill = false;
	fx_4bit_mode = false;
	fx_16bit_hop = false;
	fx_subtract = false;
	fx_cache_byte_cycling = false;
	fx_trans_writes = false;
	fx_multiplier = false;

	fx_mult_accumulator = 0;

	fx_2bit_poly = false;
	fx_2bit_poking = false;

	fx_cache_nibble_index = 0;
	fx_cache_byte_index = 0;
	fx_cache_increment_mode = 0;

	fx_cache[0] = 0;
	fx_cache[1] = 0;
	fx_cache[2] = 0;
	fx_cache[3] = 0;

	fx_16bit_hop_align = 0;

	fx_pending_pos_calc = false;
	fx_nibble_bit[0] = false;
	fx_nibble_bit[1] = false;
	fx_nibble_incr[0] = false;
	fx_nibble_incr[1] = false;

	fx_affine_tile_base = 0;
	fx_affine_map_base = 0;
	fx_affine_map_size = 2;
	fx_affine_clip = false;

	// init sprite data
	memset(sprite_data, 0, sizeof(sprite_data));
	for (uint16_t i = 0; i < 128; i++) {
		refresh_sprite_properties(i);
	}

	line_buf_sel = 0;
	memset(layer_lb, 0, sizeof(layer_lb));
	memcpy(fetch_reg_comp, (uint8_t[FL_COMP_N]){ reg_composer[0], reg_composer[2], reg_composer[6], reg_composer[7] },
	       sizeof(fetch_reg_comp));
	memset(sprite_edge_col, 0, sizeof(sprite_edge_col));
	memset(sprite_edge_z, 0, sizeof(sprite_edge_z));
	memset(sprite_edge_mask, 0, sizeof(sprite_edge_mask));
	memset(&sprite_carry, 0, sizeof(sprite_carry));
	comp_vstarted = 0;

	// copy palette
	memcpy(palette, default_palette, sizeof(palette));
	for (int i = 0; i < 256; i++) {
		palette[i * 2 + 0] = default_palette[i] & 0xff;
		palette[i * 2 + 1] = default_palette[i] >> 8;
	}

	refresh_palette();

	// fill video RAM with random data
	if (vram_seeded) {
		x16_fill_random(video_ram, sizeof video_ram, vram_seed, 4);
	} else {
		for (int i = 0; i < 128 * 1024; i++) {
			video_ram[i] = rand();
		}
	}

	sprite_line_collisions = 0;

	vga_scan_pos_x = 0;
	vga_scan_pos_y = 0;
	comp_first_frame = 1;
	line_irq_pending = 0;
	ntsc_half_cnt = 0;
	ntsc_scan_pos_y = 0;

	psg_reset();
	pcm_reset();
}

// Repaint the emulator window straight from the last rendered frame. On
// Windows a window drag/resize spins the OS in a modal message loop that
// blocks our main loop, so no new frames are produced; without this the
// window freezes or tears while being moved. Registered as an SDL event
// watch, this fires synchronously from inside that modal loop and keeps the
// last good frame on screen. (The matching timing re-base in timing.c stops
// the post-drag fast-forward.)
static void
video_clear_output(void)
{
	// The legacy debugger leaves its final line color active. Reset it so that
	// letterbox bars stay black instead of inheriting the overlay's white.
	render_backend_set_draw_color(renderer, (render_color_t){ 0, 0, 0, SDL_ALPHA_OPAQUE });
	render_backend_clear(renderer);
}

static int SDLCALL
video_move_resize_watch(void *userdata, SDL_Event *event)
{
	(void)userdata;
	static bool in_watch = false; // guard against present -> pump -> watch recursion
	if (in_watch) {
		return 1;
	}
	if (event->type == SDL_WINDOWEVENT && window && renderer && sdlTexture &&
	    event->window.windowID == SDL_GetWindowID(window)) {
		switch (event->window.event) {
			case SDL_WINDOWEVENT_MOVED:
			case SDL_WINDOWEVENT_EXPOSED:
				in_watch = true;
				video_clear_output();
				render_backend_copy(renderer, sdlTexture, NULL, NULL);
				if (debugger_enabled && showDebugOnRender != 0) {
					DEBUGRenderDisplay(SCREEN_WIDTH, SCREEN_HEIGHT);
				}
				render_backend_present(renderer);
				in_watch = false;
				break;
			case SDL_WINDOWEVENT_RESIZED:
			case SDL_WINDOWEVENT_SIZE_CHANGED:
				in_watch = true;
				render_backend_resize(renderer, event->window.data1, event->window.data2);
				video_clear_output();
				render_backend_copy(renderer, sdlTexture, NULL, NULL);
				if (debugger_enabled && showDebugOnRender != 0) {
					DEBUGRenderDisplay(SCREEN_WIDTH, SCREEN_HEIGHT);
				}
				render_backend_present(renderer);
				in_watch = false;
				break;
		}
	}
	return 1; // leave the event in the queue for normal handling
}

static bool
render_driver_available(const char *name)
{
	const int count = SDL_GetNumRenderDrivers();
	for (int i = 0; i < count; i++) {
		SDL_RendererInfo info;
		if (SDL_GetRenderDriverInfo(i, &info) == 0 &&
		    SDL_strcasecmp(name, info.name) == 0) {
			return true;
		}
	}
	return false;
}

// ─── Window placement (see video.h) ────────────────────────────────────────

static video_window_geometry_t window_geom[VIDEO_WINDOW_COUNT];
static bool window_geom_valid[VIDEO_WINDOW_COUNT];

static SDL_Window *
video_window_for(video_window_t which)
{
	switch (which) {
		case VIDEO_WINDOW_MAIN: return window;
#ifdef HAS_IMGUI
		case VIDEO_WINDOW_DEBUGGER: return imgui_window;
#endif
		default: return NULL;
	}
}

bool
video_window_geometry_poll(void)
{
	bool changed = false;
	for (int i = 0; i < VIDEO_WINDOW_COUNT; i++) {
		SDL_Window *win = video_window_for((video_window_t)i);
		if (!win) {
			continue;
		}
		const Uint32 flags = SDL_GetWindowFlags(win);
		// Neither a minimized nor a fullscreen window says anything about
		// where the user wants it next time; keep the last placement.
		if (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) {
			continue;
		}
		video_window_geometry_t *g = &window_geom[i];
		const bool maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
		if (maximized) {
			// A maximized window reports the monitor's size; keep the normal
			// rect from before, so restoring down still goes back to it.
			if (!g->maximized) {
				g->maximized = true;
				changed = true;
			}
			continue;
		}
		int x, y, w, h;
		SDL_GetWindowPosition(win, &x, &y);
		SDL_GetWindowSize(win, &w, &h);
		if (!window_geom_valid[i] || g->maximized ||
		    x != g->x || y != g->y || w != g->w || h != g->h) {
			g->x = x; g->y = y; g->w = w; g->h = h;
			g->maximized = false;
			window_geom_valid[i] = true;
			changed = true;
		}
	}
	return changed;
}

bool
video_window_geometry(video_window_t which, video_window_geometry_t *out)
{
	// Through int: GCC makes this enum unsigned, and "< 0" on it trips
	// -Wtype-limits under the Linux CI's -Werror.
	if ((int)which < 0 || (int)which >= VIDEO_WINDOW_COUNT || !video_window_for(which) ||
	    !window_geom_valid[which]) {
		return false;
	}
	*out = window_geom[which];
	return true;
}

// True when a usable stretch of the title bar above (x, y) is on some display,
// so the window can still be grabbed and dragged.
static bool
video_title_bar_visible(int x, int y, int w)
{
	const SDL_Rect bar = { x, y - APPROX_TITLEBAR_HEIGHT, w, APPROX_TITLEBAR_HEIGHT };
	const int need = w < 100 ? w : 100;
	const int displays = SDL_GetNumVideoDisplays();
	for (int d = 0; d < displays; d++) {
		SDL_Rect bounds, hit;
		if (SDL_GetDisplayBounds(d, &bounds) != 0) {
			continue;
		}
		if (SDL_IntersectRect(&bar, &bounds, &hit) && hit.w >= need) {
			return true;
		}
	}
	return false;
}

void
video_restore_window_geometry(video_window_t which, const video_window_geometry_t *g)
{
	SDL_Window *win = video_window_for(which);
	if (!win || !g || hidden_windows) {
		return;
	}
	if (which == VIDEO_WINDOW_MAIN && is_fullscreen) {
		return;
	}
	if (g->w < 64 || g->h < 64 || !video_title_bar_visible(g->x, g->y, g->w)) {
		return;
	}
	if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) {
		SDL_RestoreWindow(win);
	}
	SDL_SetWindowSize(win, g->w, g->h);
	SDL_SetWindowPosition(win, g->x, g->y);
	if (g->maximized) {
		SDL_MaximizeWindow(win);
	}
}

// ─── Main window placement without the debugger ────────────────────────────
// With -imgui, ImGui owns imgui.ini and its settings handler saves both windows
// (debug_ui_settings.cpp). A plain run has no ImGui at all, so it reads and
// rewrites just the one MainWindow= line itself: same file, same section, same
// format, so either kind of run picks up where the other left the window.

#define PLACEMENT_INI     "imgui.ini"
#define PLACEMENT_SECTION "[X16Debugger][Prefs]"
#define PLACEMENT_KEY     "MainWindow="

static bool
placement_owned_here(void)
{
	return !imgui_debugger_enabled && !hidden_windows;
}

// The whole file, NUL-terminated, or NULL when there is none.
static char *
placement_read_file(size_t *out_len)
{
	FILE *f = fopen(PLACEMENT_INI, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = len >= 0 ? malloc((size_t)len + 1) : NULL;
	if (buf) {
		const size_t got = fread(buf, 1, (size_t)len, f);
		buf[got] = 0;
		*out_len = got;
	}
	fclose(f);
	return buf;
}

// Where the MainWindow= line of our section starts and ends (end excludes the
// newline), or false when the section or the line is missing. *section_end is
// where a line can be inserted into the section when the key is missing.
static bool
placement_find(const char *buf, size_t len, size_t *line_start, size_t *line_end,
               size_t *section_end, bool *have_section)
{
	bool   in_section = false;
	size_t pos        = 0;
	*have_section     = false;
	while (pos < len) {
		size_t eol = pos;
		while (eol < len && buf[eol] != '\n') {
			eol++;
		}
		size_t end = eol;
		if (end > pos && buf[end - 1] == '\r') {
			end--;
		}
		const size_t n = end - pos;
		if (n > 0 && buf[pos] == '[') {
			if (in_section) {
				return false; // left our section without seeing the key
			}
			in_section = n == strlen(PLACEMENT_SECTION) &&
			             !memcmp(buf + pos, PLACEMENT_SECTION, n);
			if (in_section) {
				*have_section = true;
				*section_end  = eol < len ? eol + 1 : len;
			}
		} else if (in_section) {
			if (n > 0) {
				*section_end = eol < len ? eol + 1 : len;
			}
			if (n >= strlen(PLACEMENT_KEY) &&
			    !memcmp(buf + pos, PLACEMENT_KEY, strlen(PLACEMENT_KEY))) {
				*line_start = pos;
				*line_end   = end;
				return true;
			}
		}
		pos = eol + 1;
	}
	return false;
}

static void
placement_load_main(void)
{
	if (!placement_owned_here()) {
		return;
	}
	size_t len = 0;
	char  *buf = placement_read_file(&len);
	if (!buf) {
		return;
	}
	size_t s, e, se;
	bool   have_section;
	if (placement_find(buf, len, &s, &e, &se, &have_section)) {
		video_window_geometry_t g = { 0 };
		int max = 0;
		if (sscanf(buf + s + strlen(PLACEMENT_KEY), "%d,%d,%d,%d,%d",
		           &g.x, &g.y, &g.w, &g.h, &max) >= 4) {
			g.maximized = max != 0;
			video_restore_window_geometry(VIDEO_WINDOW_MAIN, &g);
		}
	}
	free(buf);
}

static void
placement_save_main(void)
{
	if (!placement_owned_here()) {
		return;
	}
	video_window_geometry_poll();
	video_window_geometry_t g;
	if (!video_window_geometry(VIDEO_WINDOW_MAIN, &g)) {
		return;
	}
	char line[96];
	snprintf(line, sizeof line, PLACEMENT_KEY "%d,%d,%d,%d,%d",
	         g.x, g.y, g.w, g.h, g.maximized ? 1 : 0);

	size_t len = 0;
	char  *buf = placement_read_file(&len);
	size_t s = 0, e = 0, se = 0;
	bool   have_section = false;
	const bool found = buf && placement_find(buf, len, &s, &e, &se, &have_section);
	if (found && e - s == strlen(line) && !memcmp(buf + s, line, e - s)) {
		free(buf); // unchanged; leave the file alone
		return;
	}
	FILE *f = fopen(PLACEMENT_INI, "wb");
	if (!f) {
		free(buf);
		return;
	}
	if (found) {
		// Replace the line in place; everything else is kept byte for byte.
		fwrite(buf, 1, s, f);
		fputs(line, f);
		fwrite(buf + e, 1, len - e, f);
	} else if (have_section) {
		fwrite(buf, 1, se, f);
		if (se > 0 && buf[se - 1] != '\n') {
			fputc('\n', f);
		}
		fprintf(f, "%s\n", line);
		fwrite(buf + se, 1, len - se, f);
	} else {
		if (buf && len) {
			fwrite(buf, 1, len, f);
			if (buf[len - 1] != '\n') {
				fputc('\n', f);
			}
			fputc('\n', f);
		}
		fprintf(f, PLACEMENT_SECTION "\n%s\n\n", line);
	}
	fclose(f);
	free(buf);
}

bool
video_init(int window_scale, float screen_x_scale, const char *quality,
           const char *render_driver, bool fullscreen, float opacity)
{
	uint32_t window_flags = SDL_WINDOW_ALLOW_HIGHDPI;
	const char *requested_driver = render_driver;
	const bool request_dx12 = requested_driver &&
	                          SDL_strcasecmp(requested_driver, "dx12") == 0;
	renderer_screen_x_scale = screen_x_scale;
	renderer_quality = quality;
	timing_title_status = video_title_status;

#ifdef __EMSCRIPTEN__
	// Setting this flag would render the web canvas outside of its bounds on high dpi screens
	window_flags &= ~SDL_WINDOW_ALLOW_HIGHDPI;
#endif

	video_reset();

	if (requested_driver && !request_dx12 && !render_driver_available(requested_driver)) {
		fprintf(stderr,
		        "Warning: SDL render driver '%s' is not available; using SDL's default selection.\n",
		        requested_driver);
		requested_driver = NULL;
	}
	if (requested_driver && !request_dx12 &&
	    !SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, requested_driver, SDL_HINT_OVERRIDE)) {
		fprintf(stderr,
		        "Warning: Could not select SDL render driver '%s'; using SDL's default selection.\n",
		        requested_driver);
		requested_driver = NULL;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, quality);
	SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1"); // Grabs keyboard shortcuts from the system during window grab
	// Hidden, not minimized: a minimized window reports a zero size and ImGui
	// stops drawing into it, which would hide exactly the cost a benchmark is
	// measuring. A hidden one keeps its size, so everything still renders.
	if (hidden_windows)
		window_flags |= SDL_WINDOW_HIDDEN;
	window = SDL_CreateWindow(NULL, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                          SCREEN_WIDTH * window_scale * screen_x_scale,
	                          SCREEN_HEIGHT * window_scale, window_flags);
	if (!window) {
		fprintf(stderr, "Failed to create emulator window: %s\n", SDL_GetError());
		return false;
	}
	renderer = NULL;
#ifdef HAS_DX12
	if (request_dx12) {
		renderer = render_backend_create_dx12(window);
		if (!renderer) {
			fprintf(stderr,
			        "Warning: Native DX12 renderer could not be initialized; falling back to SDL.\n");
		}
	}
#else
	if (request_dx12) {
		fprintf(stderr,
		        "Warning: Native DX12 renderer is not compiled into this build; falling back to SDL.\n");
	}
#endif
	if (!renderer) {
		renderer = render_backend_create_sdl(window);
	}
	if (!renderer) {
		fprintf(stderr, "Failed to create emulator renderer: %s\n", SDL_GetError());
		SDL_DestroyWindow(window);
		window = NULL;
		return false;
	}

	const char *renderer_name = render_backend_name(renderer);
	if (renderer_name) {
		if (!request_dx12 && requested_driver &&
		    SDL_strcasecmp(requested_driver, renderer_name) != 0) {
			fprintf(stderr,
			        "Warning: SDL render driver '%s' could not be initialized; using '%s'.\n",
			        requested_driver, renderer_name);
		}
		if (request_dx12 && SDL_strcasecmp(renderer_name, "dx12") == 0) {
			fprintf(stderr, "Native render backend: %s\n", renderer_name);
		} else {
			fprintf(stderr, "SDL render driver: %s\n", renderer_name);
		}
	}
#ifndef __MORPHOS__
	SDL_SetWindowResizable(window, true);
#endif
	render_backend_set_logical_size(renderer, SCREEN_WIDTH * screen_x_scale, SCREEN_HEIGHT);

	render_scale_quality_t render_quality = RENDER_SCALE_BEST;
	if (!strcmp(quality, "nearest")) {
		render_quality = RENDER_SCALE_NEAREST;
	} else if (!strcmp(quality, "linear")) {
		render_quality = RENDER_SCALE_LINEAR;
	}
	sdlTexture = render_backend_texture_create(renderer, RENDER_PIXEL_FORMAT_BGRA8888,
	                                           RENDER_TEXTURE_STREAMING, render_quality,
	                                           SCREEN_WIDTH, SCREEN_HEIGHT);
	if (!sdlTexture) {
		fprintf(stderr, "Failed to create emulator texture: %s\n", SDL_GetError());
		render_backend_destroy(renderer);
		SDL_DestroyWindow(window);
		renderer = NULL;
		window = NULL;
		return false;
	}

	// Keep the window painted while the OS holds us in a modal move/resize loop.
	SDL_AddEventWatch(video_move_resize_watch, NULL);

	SDL_SetWindowTitle(window, WINDOW_TITLE);
	SDL_SetWindowIcon(window, CommanderX16Icon());
	if (fullscreen) {
		is_fullscreen = true;
		SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
	} else {
		int winX, winY;
		SDL_GetWindowPosition(window, &winX, &winY);
		if (winX < 0 || winY < APPROX_TITLEBAR_HEIGHT) {
			winX = winX < 0 ? 0 : winX;
			winY = winY < APPROX_TITLEBAR_HEIGHT ? APPROX_TITLEBAR_HEIGHT : winY;
			SDL_SetWindowPosition(window, winX, winY);
		}
	}

	// After the off-screen nudge above: a saved placement replaces it.
	placement_load_main();

	SDL_SetWindowOpacity(window, opacity);

#ifdef _WIN32
	extern void video_win32_set_rounded_corners(SDL_Window *window);
	video_win32_set_rounded_corners(window);
	// Subclass the window proc so emulation keeps running during a drag; see
	// video_win32.c for why the SDL message hook is not enough.
	extern void video_win32_install_move_hook(SDL_Window *window);
	video_win32_install_move_hook(window);
#endif

	if (record_gif != RECORD_GIF_DISABLED) {
		if (!strcmp(gif_path+strlen(gif_path)-5, ",wait")) {
			// wait for POKE
			record_gif = RECORD_GIF_PAUSED;
			// move the string terminator to remove the ",wait"
			gif_path[strlen(gif_path)-5] = 0;
		} else {
			// start now
			record_gif = RECORD_GIF_ACTIVE;
		}
		if (!GifBegin(&gif_writer, gif_path, SCREEN_WIDTH, SCREEN_HEIGHT, 1, 8, false)) {
			record_gif = RECORD_GIF_DISABLED;
		}
	}

	if (debugger_enabled) {
		DEBUGInitUI(renderer);
		legacy_debug_ui_initialized = true;
	}

#ifdef HAS_IMGUI
	// ImGui debugger window -- additive, independent of the -debug SDL debugger.
	if (imgui_debugger_enabled) {
		int mainX, mainY, mainW;
		SDL_GetWindowPosition(window, &mainX, &mainY);
		SDL_GetWindowSize(window, &mainW, NULL);

		imgui_window = SDL_CreateWindow("Commander X16 - ImGui Debugger",
			mainX + mainW + 4, mainY,
			960, 720,
			(hidden_windows ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN) |
			    SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
		if (imgui_window) {
#ifdef HAS_DX12
			// With a native DX12 main output, draw the debugger natively too,
			// on the very same device, command queue and shader-visible SRV
			// heap. No second SDL_Renderer is created in that case.
			if (renderer_name && SDL_strcasecmp(renderer_name, "dx12") == 0) {
				imgui_native_dx12 = debug_ui_init_dx12(imgui_window, renderer);
				if (!imgui_native_dx12) {
					printf("Warning: The native DX12 debugger renderer could not be "
					       "initialized; falling back to SDL.\n");
				}
			}
#endif
			if (!imgui_native_dx12) {
				// Deliberately NOT SDL_RENDERER_PRESENTVSYNC. This renderer is
				// presented from the emulator thread, so a vsync'd present would
				// block it for up to a refresh interval every frame -- capping the
				// machine at the monitor's rate and defeating warp mode entirely.
				// The main window does not vsync either; timing.c does the
				// throttling. debug_ui_render() is rate-limited instead (see
				// video_render_all_ex).
				imgui_renderer = SDL_CreateRenderer(imgui_window, -1, SDL_RENDERER_ACCELERATED);
				if (imgui_renderer && !debug_ui_init(imgui_window, imgui_renderer)) {
					SDL_DestroyRenderer(imgui_renderer);
					imgui_renderer = NULL;
				}
			}
		}
		if (imgui_window && (imgui_renderer || imgui_native_dx12)) {
			imgui_window_id = SDL_GetWindowID(imgui_window);
#ifdef _WIN32
			// Keep both windows painted while the OS holds us in a modal
			// move/resize loop dragging the debugger window. Emulation does
			// *not* continue through a drag once a debugger is up --
			// emulator_step_during_move() returns early for exactly that
			// reason, so breakpoints cannot be stepped past behind the UI's
			// back -- but without this the windows would freeze mid-drag.
			video_win32_install_move_hook(imgui_window);
#endif
		} else {
			printf("Warning: Failed to create debugger window: %s\n", SDL_GetError());
			if (imgui_renderer) { SDL_DestroyRenderer(imgui_renderer); imgui_renderer = NULL; }
			if (imgui_window) { SDL_DestroyWindow(imgui_window); imgui_window = NULL; }
			// Stop claiming the UI exists. Left set, -imgui alone would still
			// route the emulator loop through DEBUGGetCurrentStatus() and stop
			// at breakpoints, with no window to resume from and no text
			// debugger either -- the machine would simply hang.
			imgui_debugger_enabled = false;
			printf("The graphical debugger is disabled for this session.\n");
			// Same reasoning as the headless path in main.c: the .dbg policy
			// was noted from a flag that has just been cleared.
			dbg_load_note_debugger(debugger_enabled);
		}
	}
#endif

	if (grab_mouse && !mouse_grabbed)
		mousegrab_toggle();

	return true;
}

#ifdef HAS_IMGUI
// Put the debugger window back on an ordinary SDL_Renderer with the ImGui
// SDLRenderer2 backend. Used both when the native debugger swap chain fails on
// its own -- the healthy main DX12 backend is left completely alone in that
// case -- and after a main-window device loss has already torn it down.
// Everything the ImGui side owned is gone by the time this runs, so it only has
// to build the SDL renderer and re-init. On failure the debugger is retired for
// the session, exactly as a failed window would have been at start-up.
static void
video_debug_ui_use_sdl_renderer(void)
{
	if (!imgui_window) {
		return;
	}
	if (!imgui_renderer) {
		imgui_renderer = SDL_CreateRenderer(imgui_window, -1, SDL_RENDERER_ACCELERATED);
	}
	if (imgui_renderer && debug_ui_init(imgui_window, imgui_renderer)) {
		return;
	}
	fprintf(stderr, "Warning: The debugger window could not be recovered: %s\n", SDL_GetError());
	if (imgui_renderer) {
		SDL_DestroyRenderer(imgui_renderer);
		imgui_renderer = NULL;
	}
#ifdef _WIN32
	extern void video_win32_remove_move_hook(SDL_Window *window);
	video_win32_remove_move_hook(imgui_window);
#endif
	SDL_DestroyWindow(imgui_window);
	imgui_window = NULL;
	imgui_window_id = 0;
	imgui_debugger_enabled = false;
	printf("The graphical debugger is disabled for the rest of this session.\n");
}
#endif

// Device removal is terminal for this compact backend instance. Recreate the
// legacy renderer and its debugger font texture rather than retaining COM
// objects whose device has been lost.
static bool
video_fallback_to_sdl(void)
{
	if (!renderer || render_backend_is_healthy(renderer)) {
		return true;
	}
	fprintf(stderr, "Warning: Native render backend failed; switching to SDL renderer.\n");
	const bool restore_debug_ui = legacy_debug_ui_initialized;
	if (restore_debug_ui) {
		DEBUGFreeUI();
		legacy_debug_ui_initialized = false;
	}
#ifdef HAS_IMGUI
	// The ImGui debugger borrowed this backend's device, queue and SRV heap, so
	// it has to be torn down before the backend is -- and brought back up on an
	// SDL renderer afterwards.
	const bool restore_imgui_ui = imgui_native_dx12;
	if (restore_imgui_ui) {
		debug_ui_shutdown();
		imgui_native_dx12 = false;
	}
#endif
	render_backend_texture_destroy(sdlTexture);
	sdlTexture = NULL;
	render_backend_destroy(renderer);
	renderer = render_backend_create_sdl(window);
	if (!renderer) {
		fprintf(stderr, "Failed to recover SDL renderer: %s\n", SDL_GetError());
		return false;
	}
	render_backend_set_logical_size(renderer, SCREEN_WIDTH * renderer_screen_x_scale, SCREEN_HEIGHT);
	render_scale_quality_t quality = RENDER_SCALE_BEST;
	if (!strcmp(renderer_quality, "nearest")) {
		quality = RENDER_SCALE_NEAREST;
	} else if (!strcmp(renderer_quality, "linear")) {
		quality = RENDER_SCALE_LINEAR;
	}
	sdlTexture = render_backend_texture_create(renderer, RENDER_PIXEL_FORMAT_BGRA8888,
	                                           RENDER_TEXTURE_STREAMING, quality,
	                                           SCREEN_WIDTH, SCREEN_HEIGHT);
	if (!sdlTexture) {
		fprintf(stderr, "Failed to recover SDL texture: %s\n", SDL_GetError());
		render_backend_destroy(renderer);
		renderer = NULL;
		return false;
	}
	if (restore_debug_ui) {
		DEBUGInitUI(renderer);
		legacy_debug_ui_initialized = true;
	}
#ifdef HAS_IMGUI
	if (restore_imgui_ui) {
		video_debug_ui_use_sdl_renderer();
	}
#endif
	return true;
}

struct video_layer_properties
{
	uint8_t color_depth;
	uint32_t map_base;
	uint32_t tile_base;

	bool text_mode;
	bool text_mode_256c;
	bool tile_mode;
	bool bitmap_mode;

	uint16_t hscroll;
	uint16_t vscroll;

	uint8_t  mapw_log2;
	uint8_t  maph_log2;
	uint16_t tilew;
	uint16_t tileh;
	uint8_t  tilew_log2;
	uint8_t  tileh_log2;

	uint16_t mapw_max;
	uint16_t maph_max;
	uint16_t tilew_max;
	uint16_t tileh_max;
	uint16_t layerw_max;
	uint16_t layerh_max;

	uint8_t tile_size_log2;

	int min_eff_x;
	int max_eff_x;

	uint8_t bits_per_pixel;
	uint8_t first_color_pos;
	uint8_t color_mask;
	uint8_t color_fields_max;
};

#define NUM_LAYERS 2
struct video_layer_properties layer_properties[NUM_LAYERS];
// The layer properties as the line being fetched began.
static struct video_layer_properties fetch_layer_properties[NUM_LAYERS];

static int
calc_layer_eff_x(const struct video_layer_properties *props, const int x)
{
	return (x + props->hscroll) & (props->layerw_max);
}

static int
calc_layer_eff_y(const struct video_layer_properties *props, const int y)
{
	return (y + props->vscroll) & (props->layerh_max);
}

static uint32_t
calc_layer_map_addr_base2(const struct video_layer_properties *props, const int eff_x, const int eff_y)
{
	// Slightly faster on some platforms because we know that tilew and tileh are powers of 2.
	return props->map_base + ((((eff_y >> props->tileh_log2) << props->mapw_log2) + (eff_x >> props->tilew_log2)) << 1);
}

//TODO: Unused in all current cases. Delete? Or leave commented as a reminder?
//static uint32_t
//calc_layer_map_addr(struct video_layer_properties *props, int eff_x, int eff_y)
//{
//	return props->map_base + ((eff_y / props->tileh) * props->mapw + (eff_x / props->tilew)) * 2;
//}
static void
refresh_layer_properties(const uint8_t layer)
{
	struct video_layer_properties* props = &layer_properties[layer];

	uint16_t prev_layerw_max = props->layerw_max;
	uint16_t prev_hscroll = props->hscroll;

	props->color_depth    = reg_layer[layer][0] & 0x3;
	props->map_base       = reg_layer[layer][1] << 9;
	props->tile_base      = (reg_layer[layer][2] & 0xFC) << 9;
	props->bitmap_mode    = (reg_layer[layer][0] & 0x4) != 0;
	props->text_mode      = (props->color_depth == 0) && !props->bitmap_mode;
	props->text_mode_256c = (reg_layer[layer][0] & 8) != 0;
	props->tile_mode      = !props->bitmap_mode && !props->text_mode;

	if (!props->bitmap_mode) {
		props->hscroll = reg_layer[layer][3] | (reg_layer[layer][4] & 0xf) << 8;
		props->vscroll = reg_layer[layer][5] | (reg_layer[layer][6] & 0xf) << 8;
	} else {
		props->hscroll = 0;
		props->vscroll = 0;
	}

	uint16_t mapw = 0;
	uint16_t maph = 0;
	props->tilew = 0;
	props->tileh = 0;

	if (props->tile_mode || props->text_mode) {
		props->mapw_log2 = 5 + ((reg_layer[layer][0] >> 4) & 3);
		props->maph_log2 = 5 + ((reg_layer[layer][0] >> 6) & 3);
		mapw      = 1 << props->mapw_log2;
		maph      = 1 << props->maph_log2;

		// Scale the tiles or text characters according to TILEW and TILEH.
		props->tilew_log2 = 3 + (reg_layer[layer][2] & 1);
		props->tileh_log2 = 3 + ((reg_layer[layer][2] >> 1) & 1);
		props->tilew      = 1 << props->tilew_log2;
		props->tileh      = 1 << props->tileh_log2;
	} else if (props->bitmap_mode) {
		// bitmap mode is basically tiled mode with a single huge tile
		props->tilew = (reg_layer[layer][2] & 1) ? 640 : 320;
		props->tileh = SCREEN_HEIGHT;
	}

	// We know mapw, maph, tilew, and tileh are powers of two in all cases except bitmap modes, and any products of that set will be powers of two,
	// so there's no need to modulo against them if we have bitmasks we can bitwise-and against.

	props->mapw_max = mapw - 1;
	props->maph_max = maph - 1;
	props->tilew_max = props->tilew - 1;
	props->tileh_max = props->tileh - 1;
	props->layerw_max = (mapw * props->tilew) - 1;
	props->layerh_max = (maph * props->tileh) - 1;

	// Find min/max eff_x for bulk reading in tile data during draw.
	if (prev_layerw_max != props->layerw_max || prev_hscroll != props->hscroll) {
		int min_eff_x = INT_MAX;
		int max_eff_x = INT_MIN;
		for (int x = 0; x < SCREEN_WIDTH; ++x) {
			int eff_x = calc_layer_eff_x(props, x);
			if (eff_x < min_eff_x) {
				min_eff_x = eff_x;
			}
			if (eff_x > max_eff_x) {
				max_eff_x = eff_x;
			}
		}
		props->min_eff_x = min_eff_x;
		props->max_eff_x = max_eff_x;
	}

	props->bits_per_pixel = 1 << props->color_depth;
	props->tile_size_log2 = props->tilew_log2 + props->tileh_log2 + props->color_depth - 3;

	props->first_color_pos  = 8 - props->bits_per_pixel;
	props->color_mask       = (1 << props->bits_per_pixel) - 1;
	props->color_fields_max = (8 >> props->color_depth) - 1;
}

// See the forward declaration above video_reset() for why this exists.
static void
video_reset_layer_pipeline(void)
{
	refresh_layer_properties(0);
	refresh_layer_properties(1);
	memcpy(fetch_layer_properties, layer_properties, sizeof(fetch_layer_properties));
	memcpy(fetch_reg_layer, reg_layer, sizeof(fetch_reg_layer));
	fetch_log_n    = 0;
	fetch_log_lost = false;
	sprite_log_n   = 0;
	fetched_y      = FETCH_NONE;
	memset(layer_fsm, 0, sizeof(layer_fsm));
}

struct video_sprite_properties
{
	int8_t sprite_zdepth;
	uint8_t sprite_collision_mask;

	int16_t sprite_x;
	int16_t sprite_y;
	uint8_t sprite_width_log2;
	uint8_t sprite_height_log2;
	uint8_t sprite_width;
	uint8_t sprite_height;

	bool hflip;
	bool vflip;

	uint8_t color_mode;
	uint32_t sprite_address;

	uint16_t palette_offset;
};

#ifndef __EMSCRIPTEN__
// The framebuffer stores pixels in BGRA but a PNG wants RGB.
static void
fill_png_buffer(void)
{
	for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
		png_buffer[(i*3)+0] = framebuffer[(i*4)+2];
		png_buffer[(i*3)+1] = framebuffer[(i*4)+1];
		png_buffer[(i*3)+2] = framebuffer[(i*4)+0];
	}
}

static void
screenshot(void)
{
	char name[64], path[PATH_MAX];
	const time_t now = time(NULL);
	strftime(name, sizeof name, "x16emu-%Y-%m-%d-%H-%M-%S.png", localtime(&now));
	// In the screenshot folder (Settings), made if it is missing.
	if (!export_path(EXPORT_SCREENSHOT, name, path, sizeof path))
		printf("WARNING: Couldn't make the screenshot folder %s\n", export_dir(EXPORT_SCREENSHOT));

	fill_png_buffer();

	if (stbi_write_png(path, SCREEN_WIDTH, SCREEN_HEIGHT, 3, png_buffer, SCREEN_WIDTH*3)) {
		printf("Wrote screenshot to %s\n", path);
	} else {
		printf("WARNING: Couldn't write screenshot to %s\n", path);
	}
}

typedef struct {
	uint8_t *data;
	size_t   len, cap;
	bool     failed;
} png_sink;

static void
png_sink_write(void *context, void *data, int size)
{
	png_sink *s = (png_sink *)context;
	if (s->failed) {
		return;
	}
	if (s->len + (size_t)size > s->cap) {
		size_t cap = s->cap ? s->cap : 64 * 1024;
		while (cap < s->len + (size_t)size) {
			cap *= 2;
		}
		uint8_t *grown = (uint8_t *)realloc(s->data, cap);
		if (!grown) {
			s->failed = true;
			return;
		}
		s->data = grown;
		s->cap  = cap;
	}
	memcpy(s->data + s->len, data, (size_t)size);
	s->len += (size_t)size;
}

uint8_t *
video_screenshot_png(size_t *out_len, int *out_w, int *out_h)
{
	png_sink sink = { NULL, 0, 0, false };
	fill_png_buffer();
	const int ok = stbi_write_png_to_func(png_sink_write, &sink, SCREEN_WIDTH, SCREEN_HEIGHT, 3,
	                                      png_buffer, SCREEN_WIDTH * 3);
	if (!ok || sink.failed) {
		free(sink.data);
		return NULL;
	}
	*out_len = sink.len;
	*out_w   = SCREEN_WIDTH;
	*out_h   = SCREEN_HEIGHT;
	return sink.data;
}
#endif

void
video_get_text_layout(int layer, screen_text_layout_t *out)
{
	const uint8_t *r = reg_layer[layer];
	out->enabled  = (reg_composer[0] & 3) != 0 && ((reg_composer[0] >> (4 + layer)) & 1);
	out->is_text  = (r[0] & 0x07) == 0;  // tiles, one bit per pixel
	out->map_base = (uint32_t)r[1] << 9;
	out->map_w    = 32 << ((r[0] >> 4) & 3);
	out->map_h    = 32 << ((r[0] >> 6) & 3);
	out->tile_w   = 8 << (r[2] & 1);
	out->tile_h   = 8 << ((r[2] >> 1) & 1);
	out->hscroll  = r[3] | (r[4] & 0xf) << 8;
	out->vscroll  = r[5] | (r[6] & 0xf) << 8;

	int w, h;
	video_get_active_layer_size(&w, &h);
	out->columns = w / out->tile_w;
	out->rows    = h / out->tile_h;
	if (out->columns > out->map_w) out->columns = out->map_w;
	if (out->rows > out->map_h)    out->rows    = out->map_h;
}

struct video_sprite_properties sprite_properties[128];

static void
refresh_sprite_properties(const uint16_t sprite)
{
	struct video_sprite_properties* props = &sprite_properties[sprite];

	props->sprite_zdepth = (sprite_data[sprite][6] >> 2) & 3;
	props->sprite_collision_mask = sprite_data[sprite][6] & 0xf0;

	props->sprite_x = sprite_data[sprite][2] | (sprite_data[sprite][3] & 3) << 8;
	props->sprite_y = sprite_data[sprite][4] | (sprite_data[sprite][5] & 3) << 8;
	props->sprite_width_log2  = (((sprite_data[sprite][7] >> 4) & 3) + 3);
	props->sprite_height_log2 = ((sprite_data[sprite][7] >> 6) + 3);
	props->sprite_width       = 1 << props->sprite_width_log2;
	props->sprite_height      = 1 << props->sprite_height_log2;

	// fix up negative coordinates
	if (props->sprite_x >= 0x400 - props->sprite_width) {
		props->sprite_x -= 0x400;
	}
	if (props->sprite_y >= 0x400 - props->sprite_height) {
		props->sprite_y -= 0x400;
	}

	props->hflip = sprite_data[sprite][6] & 1;
	props->vflip = (sprite_data[sprite][6] >> 1) & 1;

	props->color_mode     = (sprite_data[sprite][1] >> 7) & 1;
	props->sprite_address = sprite_data[sprite][0] << 5 | (sprite_data[sprite][1] & 0xf) << 13;

	props->palette_offset = (sprite_data[sprite][7] & 0x0f) << 4;
}

// Store one byte of a sprite's attributes, telling the trace what it replaced.
// Both VRAM write paths funnel through here so the two cannot drift apart: a
// multiplexer driven by VERA FX would otherwise go unrecorded.
static void
sprite_attr_write(uint32_t address, uint8_t value)
{
	const uint8_t slot   = (address >> 3) & 0x7f;
	const uint8_t offset = address & 0x7;
	if (sprite_trace_active) {
		sprite_trace_note_write(slot, offset, sprite_data[slot][offset], value,
		                        sprite_trace_current_line(), sprite_trace_cpu_cycles);
	}
	if (sprite_log_n < FETCH_LOG_MAX) {
		sprite_log[sprite_log_n++] = (sprite_write_t){ write_dot(), slot, offset, sprite_data[slot][offset] };
	}
	sprite_data[slot][offset] = value;
	refresh_sprite_properties(slot);
}

struct video_palette
{
	uint32_t entries[256];
	bool dirty;
};

struct video_palette video_palette;

static void
refresh_palette() {
	const uint8_t out_mode = reg_composer[0] & 3;
	const bool chroma_disable = ((reg_composer[0] & 0x07) == 6);
	for (int i = 0; i < 256; ++i) {
		uint8_t r;
		uint8_t g;
		uint8_t b;
		if (out_mode == 0) {
			// video generation off
			// -> show blue screen
			r = 0;
			g = 0;
			b = 255;
		} else {
			uint16_t entry = palette[i * 2] | palette[i * 2 + 1] << 8;
			r = ((entry >> 8) & 0xf) << 4 | ((entry >> 8) & 0xf);
			g = ((entry >> 4) & 0xf) << 4 | ((entry >> 4) & 0xf);
			b = (entry & 0xf) << 4 | (entry & 0xf);
			if (chroma_disable) {
				r = g = b = (r + b + g) / 3;
			}
		}

		video_palette.entries[i] = (uint32_t)(r << 16) | ((uint32_t)g << 8) | ((uint32_t)b);
	}
	video_palette.dirty = false;
}

static void
expand_4bpp_data(uint8_t *dst, const uint8_t *src, int dst_size)
{
	while (dst_size >= 2) {
		*dst = (*src) >> 4;
		++dst;
		*dst = (*src) & 0xf;
		++dst;

		++src;
		dst_size -= 2;
	}
}

// sprite_renderer.v:65-68, 171-176: slot i's attributes are read from the
// clock after render_start, one slot per clock until a sprite is drawn. A
// write through the data port reaches attribute RAM a clock after a register
// write would.
#define SPRITE_ATTR_DOT 1
static const struct video_sprite_properties *sprite_props_at(int i, uint32_t d, struct video_sprite_properties *out);
static const uint8_t *layer_bus_schedule(void);

static void
sprite_apply_carry(void)
{
	if (sprite_carry.on) {
		sprite_line_col[sprite_carry.x]  = sprite_carry.col;
		sprite_line_z[sprite_carry.x]    = sprite_carry.z;
		sprite_line_mask[sprite_carry.x] = sprite_carry.mask;
		sprite_carry.on = 0;
	}
}

// composer.v:172-185: the read index the composer holds at the end of a line.
static uint16_t
composer_last_index(void)
{
	uint16_t hstart = reg_composer[4] << 2, hstop = reg_composer[5] << 2;
	hstop = hstop < VGA_SCAN_WIDTH ? hstop : VGA_SCAN_WIDTH;
	uint32_t r = 0;
	for (uint16_t x = hstart; x < hstop && (r >> 7) < SCREEN_WIDTH; x++) {
		r += reg_composer[1];
	}
	return (r >> 7) & 0x3FF;
}

// The pixel on render_time_done (top.v:731-739, sprite_line_buffer.v).
static void
sprite_last_pixel(uint16_t line_x, uint8_t col, const struct video_sprite_properties *props)
{
	if (col == 0) {
		return;
	}
	const uint8_t  other = line_buf_sel ^ 1;
	const uint16_t q     = composer_last_index();
	uint8_t qcol = 0, qz = 0, qmask = 0;
	if (q >= SCREEN_WIDTH && q < SCREEN_WIDTH + 2) {
		qcol  = sprite_edge_col[other][q - SCREEN_WIDTH];
		qz    = sprite_edge_z[other][q - SCREEN_WIDTH];
		qmask = sprite_edge_mask[other][q - SCREEN_WIDTH];
	}
	if (line_x < SCREEN_WIDTH) {
		sprite_line_collisions |= qmask & props->sprite_collision_mask;
	}
	if (!(props->sprite_zdepth > qz || qcol == 0) || line_x >= SCREEN_WIDTH + 2) {
		return;
	}
	if (col < 16) {
		col += props->palette_offset;
	}
	const uint8_t mask = qmask | props->sprite_collision_mask;
	if (line_x >= SCREEN_WIDTH) {
		const uint8_t e = line_x - SCREEN_WIDTH;
		sprite_edge_col[other][e]  = col;
		sprite_edge_z[other][e]    = props->sprite_zdepth;
		sprite_edge_mask[other][e] = mask;
	} else {
		sprite_carry = (sprite_carry_t){ 1, line_x, col, props->sprite_zdepth, mask };
	}
}

static void
render_sprite_line(const uint16_t y)
{
	memset(sprite_line_col, 0, SCREEN_WIDTH);
	memset(sprite_line_z, 0, SCREEN_WIDTH);
	memset(sprite_line_mask, 0, SCREEN_WIDTH);
	sprite_apply_carry();

	// Timing follows sprite_renderer.v, which runs two state machines against
	// one shared line timer:
	//
	//   sprite_renderer.v:44   wire render_time_done = (render_time_r == 'd798);
	//   sprite_renderer.v:171  if (render_time_done) sf_state_next = SF_DONE;
	//   sprite_renderer.v:349  if (render_time_done) state_next = STATE_DONE;
	//
	// render_time_r is reset by line_render_start and counts every clock
	// afterwards, so the limit is elapsed time for the line, not a quantity of
	// work. When it expires both machines are forced to DONE and stay there
	// until the next line: the search stops, and the sprite in flight stops
	// part-drawn. Sprites are never free -- once the line is out of time,
	// everything after it is simply not on screen.
	//
	// SF_FIND_SPRITE (sprite_renderer.v:146-160) advances one slot per clock
	// whenever the slot is disabled or off the line, and it does so whether or
	// not the line renderer is busy, so stepping over empty slots overlaps with
	// drawing. Only a slot that needs rendering stalls, waiting on !render_busy.
	// That overlap is why `t` and `render_free_at` are tracked separately;
	// charging all 128 lookups serially would drop sprites the hardware draws.
	//
	// Known divergence: the CPU's data port outranks every renderer for VRAM
	// (vram_if.v:124-127) and is not counted against sprite fetches here.
	const bool trace = sprite_trace_active;
	uint16_t   traced_evaluated = 0;
	uint16_t   traced_drawn     = 0;
	uint16_t   traced_cut_slot  = SPRITE_TRACE_NO_LINE;
	uint16_t   traced_dropped   = 0;
	uint32_t   traced_tail      = 0; // cost of the part-drawn sprite left unfinished
	if (trace) {
		sprite_trace_line_begin(y, sprite_trace_cpu_cycles);
	}

	// Clocks are render_time_r. Both machines act on SPRITE_RENDER_TIME, the
	// clock render_time_done is high, and are DONE after it.
	uint32_t t              = 0; // SF_FIND_SPRITE reaches slot i
	uint32_t render_free_at = 0; // render_busy low (:432): STATE_IDLE, no start_render_r
	int      stopped_at     = NUM_SPRITES; // first slot the line never reached
	const uint8_t *taken    = NULL;

	for (int i = 0; i < NUM_SPRITES; i++) {
		if (t >= SPRITE_RENDER_TIME) {
			// render_time_done: SF_DONE, so this slot is never looked at.
			stopped_at = i;
			break;
		}

		const uint32_t t_before = t;
		uint8_t        tflags   = SPRITE_TRACE_EVALUATED;
		struct video_sprite_properties        then;
		const struct video_sprite_properties *props =
			sprite_log_n ? sprite_props_at(i, SPRITE_ATTR_DOT + t, &then) : &sprite_properties[i];

		bool on_line = props->sprite_zdepth != 0 && y >= props->sprite_y &&
		               y < props->sprite_y + props->sprite_height;

		// :146-151: a slot waiting on render_busy is read again every clock.
		while (on_line && sprite_log_n && t < render_free_at && t < SPRITE_RENDER_TIME) {
			t++;
			props   = sprite_props_at(i, SPRITE_ATTR_DOT + t, &then);
			on_line = props->sprite_zdepth != 0 && y >= props->sprite_y &&
			          y < props->sprite_y + props->sprite_height;
		}

		if (!on_line) {
			t++; // one clock to step over the slot, overlapped with rendering
			if (trace) {
				traced_evaluated++;
				sprite_trace_note_slot(y, (uint8_t)i, sprite_data[i],
				                       (uint16_t)(t - t_before), tflags);
			}
			continue;
		}

		tflags |= SPRITE_TRACE_ONSCREEN;

		if (t < render_free_at) {
			t = render_free_at;
		}
		if (t >= SPRITE_RENDER_TIME) {
			stopped_at = i;
			break;
		}
		// SF_START_RENDER at t + 1, then STATE_IDLE takes start_render_r at
		// t + 2 while the search moves on to the next slot.
		t += 2;

		const uint16_t eff_sy = props->vflip ? ((props->sprite_height - 1) - (y - props->sprite_y)) : (y - props->sprite_y);

		int16_t       eff_sx      = (props->hflip ? (props->sprite_width - 1) : 0);
		const int16_t eff_sx_incr = props->hflip ? -1 : 1;

		// line_addr is 15 bits of 32-bit words (sprite_renderer.v:247), so a row
		// running past the top of VRAM continues from address 0, even mid-row.
		const uint32_t row_address = props->sprite_address + ((uint32_t)eff_sy << (props->sprite_width_log2 - (1 - props->color_mode)));

		uint8_t unpacked_sprite_line[64];
		const uint16_t width = (props->sprite_width<64? props->sprite_width : 64);
		const uint8_t vram_fetch_mask = ((2 - props->color_mode) << 2) - 1;
		if (props->color_mode == 0) {
			// 4bpp
			uint8_t packed_sprite_line[32];
			video_space_read_range(packed_sprite_line, row_address, width / 2);
			expand_4bpp_data(unpacked_sprite_line, packed_sprite_line, width);
		} else {
			// 8bpp
			video_space_read_range(unpacked_sprite_line, row_address, width);
		}

		// The line renderer's clock. STATE_WAIT_FETCH raises its strobe at rt
		// and is granted on the first clock no layer asks (vram_if.v:124-138);
		// the ack is registered (:142-146), so STATE_RENDER starts two clocks
		// after the grant and draws one pixel a clock. Pixels hanging off the
		// side cost the same (:365-367).
		uint32_t rt        = t + 1;
		bool     truncated = false;
		uint16_t sx        = 0;

		for (; sx < props->sprite_width; ++sx) {
			if (!(sx & vram_fetch_mask)) {
				if (!taken) {
					taken = layer_bus_schedule();
				}
				while (rt < SPRITE_RENDER_TIME && taken[rt]) {
					rt++;
				}
				rt += 2;
			}
			if (rt > SPRITE_RENDER_TIME) { truncated = true; break; }
			const bool last = rt == SPRITE_RENDER_TIME;
			rt++;

			const uint16_t line_x = props->sprite_x + sx;
			if (last) {
				sprite_last_pixel(line_x, unpacked_sprite_line[eff_sx], props);
				truncated = true;
				break;
			}
			if (line_x >= SCREEN_WIDTH) {
				// sprite_renderer.v:319-323; no collision at 640+ (:327).
				if (line_x < SCREEN_WIDTH + 2) {
					uint8_t edge_col = unpacked_sprite_line[eff_sx];
					const uint8_t e  = line_x - SCREEN_WIDTH;
					if (edge_col > 0 && (props->sprite_zdepth > sprite_edge_z[line_buf_sel][e] ||
					                     sprite_edge_col[line_buf_sel][e] == 0)) {
						if (edge_col < 16) {
							edge_col += props->palette_offset;
						}
						sprite_edge_col[line_buf_sel][e] = edge_col;
						sprite_edge_z[line_buf_sel][e]   = props->sprite_zdepth;
						sprite_edge_mask[line_buf_sel][e] |= props->sprite_collision_mask;
					}
				}
				eff_sx += eff_sx_incr;
				continue;
			}

			uint8_t col_index = unpacked_sprite_line[eff_sx];
			eff_sx += eff_sx_incr;

			// palette offset
			// :319, :325-328: every opaque pixel tests the mask in the buffer,
			// but only one that is drawn writes its own mask there.
			if (col_index > 0) {
				sprite_line_collisions |= sprite_line_mask[line_x] & props->sprite_collision_mask;

				if (props->sprite_zdepth > sprite_line_z[line_x]) {
					if (col_index < 16) {
						col_index += props->palette_offset;
					}
					sprite_line_col[line_x] = col_index;
					sprite_line_z[line_x] = props->sprite_zdepth;
					sprite_line_mask[line_x] |= props->sprite_collision_mask;
					tflags |= SPRITE_TRACE_DREW;
				}
			}
		}

		render_free_at = rt;

		if (trace) {
			traced_evaluated++;
			if (tflags & SPRITE_TRACE_DREW) {
				traced_drawn++;
			}
			if (truncated) {
				tflags |= SPRITE_TRACE_CUT;
				if (traced_cut_slot == SPRITE_TRACE_NO_LINE) {
					traced_cut_slot = (uint16_t)i;
				}
				// Time can run out before this sprite lands a single pixel, in
				// which case it is as dropped as the ones the search never
				// reached, and has to be counted with them.
				if (!(tflags & SPRITE_TRACE_DREW)) {
					traced_dropped++;
				}
				const uint32_t left = (uint32_t)(props->sprite_width - sx);
				traced_tail = left + 2u * (left / (vram_fetch_mask + 1u));
			}
			sprite_trace_note_slot(y, (uint8_t)i, sprite_data[i],
			                       (uint16_t)(rt - t_before), tflags);
		}

		if (truncated) {
			// render_time_done fired mid-sprite: the rest of the line is gone.
			stopped_at = i + 1;
			break;
		}
	}

	if (!trace) {
		return;
	}

	uint32_t used = (t > render_free_at ? t : render_free_at);
	if (used > SPRITE_RENDER_TIME) {
		used = SPRITE_RENDER_TIME;
	}
	uint32_t demand = used + traced_tail;

	// What the line would have needed had it not run out. Pure arithmetic --
	// no VRAM is read and nothing is drawn -- so "how far over was this line"
	// survives the sprites being dropped, which is the number a multiplexer is
	// actually tuning against.
	for (int i = stopped_at; i < NUM_SPRITES; i++) {
		const struct video_sprite_properties *p = &sprite_properties[i];
		const bool on_line = p->sprite_zdepth != 0 && y >= p->sprite_y &&
		                     y < p->sprite_y + p->sprite_height;
		if (!on_line) {
			demand++;
			sprite_trace_note_slot(y, (uint8_t)i, sprite_data[i], 0, SPRITE_TRACE_EVALUATED);
			continue;
		}
		const uint16_t per_fetch = (uint16_t)((2 - p->color_mode) << 2);
		demand += 3u + p->sprite_width + 2u * (uint32_t)(p->sprite_width / per_fetch);
		traced_dropped++;
		if (traced_cut_slot == SPRITE_TRACE_NO_LINE) {
			traced_cut_slot = (uint16_t)i;
		}
		sprite_trace_note_slot(y, (uint8_t)i, sprite_data[i], 0,
		                       (uint8_t)(SPRITE_TRACE_EVALUATED | SPRITE_TRACE_ONSCREEN |
		                                 SPRITE_TRACE_CUT));
	}

	sprite_line_stat_t st;
	memset(&st, 0, sizeof(st));
	st.budget_used = (uint16_t)used;
	st.demand      = demand;
	st.evaluated   = traced_evaluated;
	st.drawn       = traced_drawn;
	st.dropped     = traced_dropped;
	st.cut_slot    = traced_cut_slot;
	st.exhausted   = (stopped_at < NUM_SPRITES);
	sprite_trace_line_end(y, &st);
}
static void
render_layer_line_text(uint8_t layer, uint16_t y)
{
	const struct video_layer_properties *props = &fetch_layer_properties[layer];
	uint8_t *const out = layer_line[layer];

	const uint8_t max_pixels_per_byte = (8 >> props->color_depth) - 1;
	const int     eff_y               = calc_layer_eff_y(props, y);
	const int     yy                  = eff_y & props->tileh_max;

	// additional bytes to reach the correct line of the tile
	const uint32_t y_add = (yy << props->tilew_log2) >> 3;

	const uint32_t map_addr_begin = calc_layer_map_addr_base2(props, props->min_eff_x, eff_y);
	const uint32_t map_addr_end   = calc_layer_map_addr_base2(props, props->max_eff_x, eff_y);
	const int      size           = (map_addr_end - map_addr_begin) + 2;

	uint8_t tile_bytes[512]; // max 256 tiles, 2 bytes each.
	video_space_read_range(tile_bytes, map_addr_begin, size);

	uint32_t tile_start;

	uint8_t  fg_color;
	uint8_t  bg_color;
	uint8_t  s;
	uint8_t  color_shift;

	{
		const int eff_x = calc_layer_eff_x(props, 0);
		const int xx    = eff_x & props->tilew_max;

		// extract all information from the map
		const uint32_t map_addr = calc_layer_map_addr_base2(props, eff_x, eff_y) - map_addr_begin;

		const uint8_t tile_index = tile_bytes[map_addr];
		const uint8_t byte1      = tile_bytes[map_addr + 1];

		if (!props->text_mode_256c) {
			fg_color = byte1 & 15;
			bg_color = byte1 >> 4;
		} else {
			fg_color = byte1;
			bg_color = 0;
		}

		// offset within tilemap of the current tile
		tile_start = tile_index << props->tile_size_log2;

		// additional bytes to reach the correct column of the tile
		const uint16_t x_add       = xx >> 3;
		const uint32_t tile_offset = tile_start + y_add + x_add;

		s           = video_space_read(props->tile_base + tile_offset);
		color_shift = max_pixels_per_byte - (xx & 0x7);
	}

	// Render tile line.
	for (int x = 0; x < SCREEN_WIDTH; x++) {
		// Scrolling
		const int eff_x = calc_layer_eff_x(props, x);
		const int xx = eff_x & props->tilew_max;

		if ((eff_x & 0x7) == 0) {
			if ((eff_x & props->tilew_max) == 0) {
				// extract all information from the map
				const uint32_t map_addr = calc_layer_map_addr_base2(props, eff_x, eff_y) - map_addr_begin;

				const uint8_t tile_index = tile_bytes[map_addr];
				const uint8_t byte1      = tile_bytes[map_addr + 1];

				if (!props->text_mode_256c) {
					fg_color = byte1 & 15;
					bg_color = byte1 >> 4;
				} else {
					fg_color = byte1;
					bg_color = 0;
				}

				// offset within tilemap of the current tile
				tile_start = tile_index << props->tile_size_log2;
			}

			// additional bytes to reach the correct column of the tile
			const uint16_t x_add       = xx >> 3;
			const uint32_t tile_offset = tile_start + y_add + x_add;

			s           = video_space_read(props->tile_base + tile_offset);
			color_shift = max_pixels_per_byte;
		}

		// convert tile byte to indexed color
		const uint8_t col_index = (s >> color_shift) & 1;
		--color_shift;
		out[x] = col_index ? fg_color : bg_color;
	}
}

static void
render_layer_line_tile(uint8_t layer, uint16_t y)
{
	const struct video_layer_properties *props = &fetch_layer_properties[layer];
	uint8_t *const out = layer_line[layer];

	const uint8_t max_pixels_per_byte = (8 >> props->color_depth) - 1;
	const int     eff_y               = calc_layer_eff_y(props, y);
	const uint8_t yy                  = eff_y & props->tileh_max;
	const uint8_t yy_flip             = yy ^ props->tileh_max;
	const uint32_t y_add              = (yy << ((props->tilew_log2 + props->color_depth - 3) & 31));
	const uint32_t y_add_flip         = (yy_flip << ((props->tilew_log2 + props->color_depth - 3) & 31));

	const uint32_t map_addr_begin = calc_layer_map_addr_base2(props, props->min_eff_x, eff_y);
	const uint32_t map_addr_end   = calc_layer_map_addr_base2(props, props->max_eff_x, eff_y);
	const int      size           = (map_addr_end - map_addr_begin) + 2;

	uint8_t tile_bytes[512]; // max 256 tiles, 2 bytes each.
	video_space_read_range(tile_bytes, map_addr_begin, size);

	uint8_t  palette_offset;
	bool     vflip;
	bool     hflip;
	uint32_t tile_start;
	uint8_t  s;
	uint8_t  color_shift;
	int8_t   color_shift_incr;

	{
		const int eff_x = calc_layer_eff_x(props, 0);

		// extract all information from the map
		const uint32_t map_addr = calc_layer_map_addr_base2(props, eff_x, eff_y) - map_addr_begin;

		const uint8_t byte0 = tile_bytes[map_addr];
		const uint8_t byte1 = tile_bytes[map_addr + 1];

		// Tile Flipping
		vflip = (byte1 >> 3) & 1;
		hflip = (byte1 >> 2) & 1;

		palette_offset = byte1 & 0xf0;

		// offset within tilemap of the current tile
		const uint16_t tile_index = byte0 | ((byte1 & 3) << 8);
		tile_start                = tile_index << props->tile_size_log2;

		color_shift_incr = hflip ? props->bits_per_pixel : -props->bits_per_pixel;

		int xx = eff_x & props->tilew_max;
		if (hflip) {
			xx          = xx ^ (props->tilew_max);
			color_shift = (eff_x & max_pixels_per_byte) * props->bits_per_pixel;
		} else {
			color_shift = props->first_color_pos - (eff_x & max_pixels_per_byte) * props->bits_per_pixel;
		}

		// additional bytes to reach the correct column of the tile
		uint16_t x_add       = (xx << props->color_depth) >> 3;
		uint32_t tile_offset = tile_start + (vflip ? y_add_flip : y_add) + x_add;

		s = video_space_read(props->tile_base + tile_offset);
	}


	// Render tile line.
	for (int x = 0; x < SCREEN_WIDTH; x++) {
		const int eff_x = calc_layer_eff_x(props, x);

		if ((eff_x & max_pixels_per_byte) == 0) {
			if ((eff_x & props->tilew_max) == 0) {
				// extract all information from the map
				const uint32_t map_addr = calc_layer_map_addr_base2(props, eff_x, eff_y) - map_addr_begin;

				const uint8_t byte0 = tile_bytes[map_addr];
				const uint8_t byte1 = tile_bytes[map_addr + 1];

				// Tile Flipping
				vflip = (byte1 >> 3) & 1;
				hflip = (byte1 >> 2) & 1;

				palette_offset = byte1 & 0xf0;

				// offset within tilemap of the current tile
				const uint16_t tile_index = byte0 | ((byte1 & 3) << 8);
				tile_start                = tile_index << props->tile_size_log2;

				color_shift_incr = hflip ? props->bits_per_pixel : -props->bits_per_pixel;
			}

			int xx = eff_x & props->tilew_max;
			if (hflip) {
				xx = xx ^ (props->tilew_max);
				color_shift = 0;
			} else {
				color_shift = props->first_color_pos;
			}

			// additional bytes to reach the correct column of the tile
			const uint16_t x_add       = (xx << props->color_depth) >> 3;
			const uint32_t tile_offset = tile_start + (vflip ? y_add_flip : y_add) + x_add;

			s = video_space_read(props->tile_base + tile_offset);
		}

		// convert tile byte to indexed color
		uint8_t col_index = (s >> color_shift) & props->color_mask;
		color_shift += color_shift_incr;

		// Apply Palette Offset
		if (col_index > 0 && col_index < 16) {
			col_index += palette_offset;
			if (props->text_mode_256c) {
				col_index |= 0x80;
			}
		}
		out[x] = col_index;
	}
}


static void
render_layer_line_bitmap(uint8_t layer, uint16_t y)
{
	const struct video_layer_properties *props = &fetch_layer_properties[layer];
	uint8_t *const out = layer_line[layer];

	int yy = y;
	// additional bytes to reach the correct line of the tile
	uint32_t y_add = (yy * props->tilew * props->bits_per_pixel) >> 3;

	// Render tile line.
	for (int x = 0; x < SCREEN_WIDTH; x++) {
		int xx = x;

		// extract all information from the map
		uint8_t palette_offset = fetch_reg_layer[layer][4] & 0xf;

		// additional bytes to reach the correct column of the tile
		uint16_t x_add = (xx * props->bits_per_pixel) >> 3;
		uint32_t tile_offset = y_add + x_add;
		uint8_t s = video_space_read((props->tile_base + tile_offset) & 0x1ffff);

		// convert tile byte to indexed color
		uint8_t col_index = (s >> (props->first_color_pos - ((xx & props->color_fields_max) << props->color_depth))) & props->color_mask;

		// Apply Palette Offset
		if (col_index > 0 && col_index < 16) {
			col_index += palette_offset << 4;
			if (props->text_mode_256c) {
				col_index |= 0x80;
			}
		}
		out[x] = col_index;
	}
}

static uint8_t calculate_line_col_index(uint8_t spr_zindex, uint8_t spr_col_index, uint8_t l1_col_index, uint8_t l2_col_index)
{
	uint8_t col_index = 0;
	switch (spr_zindex) {
		case 3:
			col_index = spr_col_index ? spr_col_index : (l2_col_index ? l2_col_index : l1_col_index);
			break;
		case 2:
			col_index = l2_col_index ? l2_col_index : (spr_col_index ? spr_col_index : l1_col_index);
			break;
		case 1:
			col_index = l2_col_index ? l2_col_index : (l1_col_index ? l1_col_index : spr_col_index);
			break;
		case 0:
			col_index = l2_col_index ? l2_col_index : l1_col_index;
			break;
	}
	return col_index;
}

// The five things VERA's layer renderer decodes a mode from, lifted out of the
// emulator's much larger property struct. vera_bandwidth.c is unit-tested
// without video.c, so it takes these rather than a pointer to something only
// this file knows how to build.
//
// Read from the properties the line was fetched with, so a raster split is
// followed rather than averaged away.
//
// `scrolled` is the one that is not a straight copy: the RTL starts the line
// buffer at 0 - subtile_hscroll (layer_renderer.v:499-501), so a layer scrolled
// off a tile boundary renders one tile more than the screen strictly needs.
// Bitmap modes force hscroll to zero (refresh_layer_properties()), so they
// never pay it.
static vera_bw_layer_t
vera_bw_layer_of(uint8_t layer)
{
	const struct video_layer_properties *p = &fetch_layer_properties[layer];
	vera_bw_layer_t out;

	out.enabled     = layer_line_enable[layer] != 0;
	out.bitmap_mode = p->bitmap_mode;
	out.color_depth = p->color_depth;
	// tile_width means "the wide one" in both families: 16px tiles, or the
	// 640-wide bitmap.
	out.tile_width  = (uint8_t)(p->bitmap_mode ? (p->tilew > 320) : (p->tilew_log2 > 3));
	out.scrolled    = !p->bitmap_mode && (p->hscroll & p->tilew_max) != 0;

	return out;
}

// Set while the DEBUGGER is writing, so its pokes at $9F23/$9F24 are not
// charged to the guest.
//
// video_read() is told directly, as a debugOn argument, and returns before any
// accounting. video_write() has no such parameter and some eighty call sites
// that would all have to change to give it one, so memory.c raises this flag
// around a debug write instead. The rule it enforces is the same one the read
// path already follows: if opening a memory view moved the guest's numbers,
// the numbers would be worthless.
static bool debug_port_write = false;

void
video_set_debug_write(bool on)
{
	debug_port_write = on;
}

// One byte through the data port. Wrapped rather than called inline because
// the enabled test has to happen BEFORE sprite_trace_current_line() runs: C
// evaluates arguments before the callee can decline them, and that helper is a
// branch and three global loads on VERA's hot path.
static void
note_port_traffic(bool write, uint8_t vram_bytes)
{
	if (!vera_bandwidth_active || debug_port_write)
		return;
	vera_bandwidth_note_port(write, vram_bytes, sprite_trace_current_line());
}

video_vram_tap_fn video_vram_write_tap = NULL;

video_vram_watch_fn video_vram_watch_hook = NULL;
// The data port the store being carried out came through.
static int vram_watch_port = 0;

// A debugger poke through the port is not the guest writing.
static inline void
note_vram_watch(uint32_t address, uint8_t value)
{
	if (video_vram_watch_hook && !debug_port_write)
		video_vram_watch_hook(address & 0x1FFFF, value, vram_watch_port);
}

static bool
in_area(uint32_t a, uint32_t base, uint32_t size)
{
	return a >= base && a < base + size;
}

// Which region a VRAM write lands in, from the live layer registers.
static video_vram_region_t
vram_region(uint32_t a)
{
	a &= 0x1FFFF;
	if (a >= ADDR_SPRDATA_START)
		return VIDEO_VRAM_SPRITES;
	if (a >= ADDR_PALETTE_START)
		return VIDEO_VRAM_PALETTE;
	if (a >= ADDR_PSG_START)
		return VIDEO_VRAM_PSG;
	for (int l = NUM_LAYERS - 1; l >= 0; l--) {
		const struct video_layer_properties *p = &layer_properties[l];
		const uint32_t bpp = 1u << p->color_depth;
		uint32_t map = 0, tiles;
		if (p->bitmap_mode) {
			tiles = (uint32_t)p->tilew * p->tileh * bpp / 8;
		} else {
			map = 2u << (p->mapw_log2 + p->maph_log2);
			// 256 characters in text modes, 1024 tiles otherwise.
			tiles = (uint32_t)p->tilew * p->tileh * bpp / 8 * (p->text_mode ? 256 : 1024);
		}
		if (map && in_area(a, p->map_base, map))
			return l ? VIDEO_VRAM_L1_MAP : VIDEO_VRAM_L0_MAP;
		if (in_area(a, p->tile_base, tiles))
			return l ? VIDEO_VRAM_L1_TILES : VIDEO_VRAM_L0_TILES;
	}
	return VIDEO_VRAM_OTHER;
}

static void
note_vram_write(uint32_t address, uint8_t value, bool fx4)
{
	if (!video_vram_write_tap || debug_port_write)
		return;
	video_vram_write_tap(address, value, vram_region(address), fx4);
}

static void
note_port_fx_cache_write(void)
{
	if (!vera_bandwidth_active || debug_port_write)
		return;
	vera_bandwidth_note_fx_cache_write(sprite_trace_current_line());
}

static void
note_port_fx_affine(void)
{
	if (!vera_bandwidth_active || debug_port_write)
		return;
	vera_bandwidth_note_fx_affine(sprite_trace_current_line());
}

// Where render_line() is within the current line and frame. At file scope
// rather than inside it so a save state can carry the ones that are machine
// state (see video_state_save()).
static uint16_t y_prev;
static uint16_t s_pos_x_p;
static uint32_t eff_y_fp; // 16.16 fixed point
static uint32_t eff_x_fp; // 16.16 fixed point

static uint8_t col_line[SCREEN_WIDTH];

// The RTL dot at which a register written now takes effect. Emulator dot x is
// RTL dot x - 1.
static uint16_t
write_dot(void)
{
	const int d = (int)vga_scan_pos_x - 1;
	return d < 0 ? 0 : d >= VGA_SCAN_WIDTH ? VGA_SCAN_WIDTH - 1 : (uint16_t)d;
}

static void
fetch_note(uint8_t reg, uint8_t value)
{
	if (fetch_log_n < FETCH_LOG_MAX) {
		fetch_log[fetch_log_n++] = (fetch_write_t){ write_dot(), reg, value };
	} else {
		fetch_log_lost = true;
	}
}

static uint32_t
vram_word(uint16_t addr)
{
	const uint8_t *p = &video_ram[(uint32_t)(addr & 0x7FFF) << 2];
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// A layer's registers decoded for one line, layer_renderer.v:39-124 and 190-199.
struct layer_dec {
	uint8_t  depth, bitmap, attr, map_w, map_h, tile_w, tile_h, map_base;
	uint8_t  pix_m1, lines_m1, words_m1, hscroll_hi, hstile;
	uint16_t tbase, hscroll, sli, vmap, bitmap_line_addr, lb_start;
};

static void
layer_decode(const uint8_t *r, uint16_t line_idx, struct layer_dec *d)
{
	static const uint8_t ppw_m1[8][2] = { { 7, 15 }, { 7, 15 }, { 7, 7 }, { 3, 3 }, { 31, 31 }, { 15, 15 }, { 7, 7 }, { 3, 3 } };
	d->depth           = r[0] & 3;
	d->bitmap          = (r[0] >> 2) & 1;
	d->attr            = (r[0] >> 3) & 1;
	d->map_w           = (r[0] >> 4) & 3;
	d->map_h           = r[0] >> 6;
	d->map_base        = r[1];
	d->tile_h          = (r[2] >> 1) & 1;
	d->tile_w          = r[2] & 1;
	d->tbase           = (uint16_t)(r[2] & 0xFC) << 7;
	d->hscroll         = r[3] | (r[4] & 15) << 8;
	d->hscroll_hi      = (uint8_t)(d->hscroll >> 8);
	d->hstile          = (uint8_t)(d->tile_w ? d->hscroll >> 4 : d->hscroll >> 3);
	const uint16_t vscroll = r[5] | (r[6] & 15) << 8;
	const uint8_t  mode    = (uint8_t)(d->bitmap << 2 | d->depth);
	d->pix_m1          = ppw_m1[mode][d->tile_w];
	d->lines_m1        = mode == 0 ? (d->tile_w ? 1 : 3) : mode == 1 ? (d->tile_w ? 0 : 1) : 0;
	d->words_m1        = mode == 2 ? d->tile_w : mode == 3 ? (d->tile_w ? 3 : 1) : 0;
	d->sli             = (line_idx + vscroll) & 0xFFF;
	d->vmap            = ((d->tile_h ? d->sli >> 4 : d->sli >> 3) & 0xFF) & ((32u << d->map_h) - 1);
	const uint16_t mul5 = (line_idx * 5) & 0xFFF;
	uint16_t       bm;
	switch (d->depth) {
		case 0:  bm = d->tile_w ? mul5 << 2 : mul5 << 1; break;
		case 1:  bm = d->tile_w ? mul5 << 3 : mul5 << 2; break;
		case 2:  bm = d->tile_w ? (mul5 & 0x7FF) << 4 : mul5 << 3; break;
		default: bm = d->tile_w ? (mul5 & 0x3FF) << 5 : (mul5 & 0x7FF) << 4; break;
	}
	d->bitmap_line_addr = (d->tbase + bm) & 0x7FFF;
	d->lb_start         = d->bitmap ? 0 : (uint16_t)(0 - (d->tile_w ? d->hscroll & 15 : d->hscroll & 7)) & 0x3FF;
}

// layer_renderer.v:80-124: the map entry the renderer is on.
static uint16_t
layer_map_idx(const struct layer_fetch *s, const struct layer_dec *d)
{
	const uint8_t shtile = (uint8_t)(s->htile + d->hstile);
	return (uint16_t)(d->vmap << (5 + d->map_w) | (shtile & ((32u << d->map_w) - 1)));
}

static uint16_t
layer_map_ent(const struct layer_fetch *s, const struct layer_dec *d)
{
	return (layer_map_idx(s, d) & 1) ? s->map_data >> 16 : s->map_data & 0xFFFF;
}

static uint8_t
layer_vli(const struct layer_dec *d, uint16_t map_ent)
{
	const uint8_t vflip = d->depth ? (map_ent >> 11) & 1 : 0;
	return (vflip ? ~d->sli : d->sli) & 15;
}

// layer_renderer.v:126-188
static uint16_t
layer_tile_addr(const struct layer_fetch *s, const struct layer_dec *d)
{
	const uint16_t map_ent = layer_map_ent(s, d);
	const uint16_t tidx    = d->depth == 0 ? map_ent & 0xFF : map_ent & 0x3FF;
	const uint8_t  hflip   = d->depth ? (map_ent >> 10) & 1 : 0;
	const uint8_t  vli     = layer_vli(d, map_ent);
	const uint8_t  hwc     = (hflip ? ~s->word : s->word) & 3;
	uint16_t       ta;
	switch (d->depth << 2 | d->tile_w << 1 | d->tile_h) {
		case 0x0: ta = tidx << 1 | ((vli >> 2) & 1); break;
		case 0x1: ta = tidx << 2 | ((vli >> 2) & 3); break;
		case 0x2: ta = tidx << 2 | ((vli >> 1) & 3); break;
		case 0x3: ta = tidx << 3 | ((vli >> 1) & 7); break;
		case 0x4: ta = tidx << 2 | ((vli >> 1) & 3); break;
		case 0x5: ta = tidx << 3 | ((vli >> 1) & 7); break;
		case 0x6: ta = tidx << 3 | (vli & 7); break;
		case 0x7: ta = tidx << 4 | vli; break;
		case 0x8: ta = tidx << 3 | (vli & 7); break;
		case 0x9: ta = tidx << 4 | vli; break;
		case 0xA: ta = tidx << 4 | (vli & 7) << 1 | (hwc & 1); break;
		case 0xB: ta = tidx << 5 | vli << 1 | (hwc & 1); break;
		case 0xC: ta = tidx << 4 | (vli & 7) << 1 | (hwc & 1); break;
		case 0xD: ta = tidx << 5 | vli << 1 | (hwc & 1); break;
		case 0xE: ta = tidx << 5 | (vli & 7) << 2 | hwc; break;
		default:  ta = (tidx & 0x1FF) << 6 | vli << 2 | hwc; break;
	}
	return (uint16_t)(d->tbase + ta);
}

static void
layer_fetch_clock(struct layer_fetch *s, const struct layer_dec *d, bool start, uint8_t *lb)
{
	// Pixel renderer, layer_renderer.v:332-563.
	const bool line_done = ((s->lb_idx >> 7) & 7) == 5;
	uint8_t    xcnt_n    = s->xcnt;
	uint8_t    busy_n    = s->busy;
	uint16_t   lb_idx_n  = s->lb_idx;
	if (!line_done && (s->busy || s->render_start)) {
		if (s->xcnt == d->pix_m1) {
			busy_n = 0;
			xcnt_n = 0;
		} else {
			xcnt_n = (s->xcnt + 1) & 31;
		}
		if (s->render_start) {
			xcnt_n = 1;
			busy_n = 1;
		}

		const uint8_t  x   = s->xcnt;
		const uint32_t rd  = s->render_data;
		const uint8_t  rmd = s->render_mapdata;
		const uint8_t  hx  = ((rmd & 4) ? ~x : x) & 15;
		uint8_t        tmp;
		switch (d->depth) {
			case 0: {
				const uint8_t bit = (rd >> ((x >> 3) * 8 + (7 - (x & 7)))) & 1;
				tmp = d->bitmap ? bit : !d->attr ? (bit ? rmd & 15 : rmd >> 4) : (bit ? rmd : 0);
				break;
			}
			case 1: {
				const uint8_t h = hx & ((d->lines_m1 & 1) ? 7 : 15);
				tmp = (rd >> ((h >> 2) * 8 + 6 - 2 * (h & 3))) & 3;
				break;
			}
			case 2:
				tmp = (rd >> (((hx & 7) >> 1) * 8 + ((hx & 1) ? 0 : 4))) & 15;
				break;
			default:
				tmp = (rd >> ((hx & 3) * 8)) & 255;
				break;
		}
		// layer_renderer.v:478-485 leaves 1bpp out; 49.0.6 offsets a 1bpp bitmap.
		uint8_t col = tmp;
		if ((d->depth != 0 || d->bitmap) && (tmp >> 4) == 0 && (tmp & 15) != 0) {
			col = (uint8_t)((tmp & 15) | (((rmd >> 7) | d->attr) & 1) << 7 | (rmd & 0x70));
		}
		if (s->lb_idx < SCREEN_WIDTH) {
			lb[s->lb_idx] = col;
		}
		lb_idx_n = (s->lb_idx + 1) & 0x3FF;
	}
	if (start) {
		xcnt_n   = 0;
		busy_n   = 0;
		lb_idx_n = d->lb_start;
	}

	// Line renderer, layer_renderer.v:224-327.
	const uint8_t state = s->state;
	s->render_start     = 0;
	switch (state) {
		case LR_FETCH_MAP:
			s->bus_addr = (uint16_t)(((uint16_t)d->map_base << 7) + (layer_map_idx(s, d) >> 1)) & 0x7FFF;
			s->strobe   = 1;
			s->state    = LR_WAIT_FETCH_MAP;
			break;
		case LR_WAIT_FETCH_MAP:
			if (s->ack) {
				s->map_data = vram_word(s->ack_addr);
				s->strobe   = 0;
				s->state    = LR_FETCH_TILE;
			}
			break;
		case LR_FETCH_TILE:
			s->bus_addr    = (d->bitmap ? s->bitmap_addr : layer_tile_addr(s, d)) & 0x7FFF;
			s->bitmap_addr = (s->bitmap_addr + 1) & 0x7FFF;
			s->strobe      = 1;
			s->state       = LR_WAIT_FETCH_TILE;
			break;
		case LR_WAIT_FETCH_TILE:
			if (s->ack) {
				s->tile_data           = vram_word(s->ack_addr);
				s->strobe              = 0;
				s->next_render_mapdata = (uint8_t)(layer_map_ent(s, d) >> 8);
				s->state               = LR_RENDER;
			}
			break;
		case LR_RENDER:
			if (!busy_n) {
				const uint8_t vli = d->lines_m1 ? layer_vli(d, layer_map_ent(s, d)) : 0;
				switch (d->lines_m1) {
					case 1:  s->render_data = (vli & 1) ? s->tile_data >> 16 : s->tile_data & 0xFFFF; break;
					case 3:  s->render_data = (s->tile_data >> ((vli & 3) * 8)) & 0xFF; break;
					default: s->render_data = s->tile_data; break;
				}
				s->render_mapdata = d->bitmap ? (uint8_t)(d->hscroll_hi << 4) : s->next_render_mapdata;
				s->render_start   = 1;
				if (d->bitmap) {
					s->state = LR_FETCH_TILE;
				} else if (s->word == d->words_m1) {
					const uint8_t shtile = (uint8_t)(s->htile + d->hstile);
					s->word  = 0;
					s->htile = s->htile + 1;
					s->state = (shtile & 1) ? LR_FETCH_MAP : LR_FETCH_TILE;
				} else {
					s->word  = (s->word + 1) & 3;
					s->state = LR_FETCH_TILE;
				}
			}
			if (line_done) {
				s->state = LR_WAIT_START;
			}
			break;
		default:
			break;
	}
	if (start) {
		s->state       = d->bitmap ? LR_FETCH_TILE : LR_FETCH_MAP;
		s->htile       = 0;
		s->word        = 0;
		s->bitmap_addr = d->bitmap_line_addr;
	}
	s->xcnt   = xcnt_n;
	s->busy   = busy_n;
	s->lb_idx = lb_idx_n;
}

// busy stays set past the end of the line until the next render_start
// (layer_renderer.v:332-345).
static bool
layer_fsm_busy(const struct layer_fetch *s)
{
	return s->busy && ((s->lb_idx >> 7) & 7) != 5;
}

static bool
layer_fsm_unfinished(const struct layer_fetch *s)
{
	if (s->ack || s->render_start || layer_fsm_busy(s)) {
		return true;
	}
	// A request still waiting is granted and its answer dropped by the next
	// render_start, as if there had been none.
	return !(s->state == LR_WAIT_START && !s->strobe) &&
	       !((s->state == LR_WAIT_FETCH_MAP || s->state == LR_WAIT_FETCH_TILE) && s->strobe);
}

// Clocking on changes nothing until the next render_start: 2 when the line is
// done, 1 when a masked request stalls it until the layer is enabled.
static int
layer_fsm_idle(const struct layer_fetch *s, bool enabled)
{
	if (s->ack || s->render_start || layer_fsm_busy(s)) {
		return 0;
	}
	if (s->state == LR_WAIT_START && !s->strobe) {
		return 2;
	}
	return !enabled && s->strobe && (s->state == LR_WAIT_FETCH_MAP || s->state == LR_WAIT_FETCH_TILE);
}

// The renderer after a line it finished, or that a masked strobe kept waiting.
static void
layer_fsm_settle(struct layer_fetch *s, bool enabled)
{
	memset(s, 0, sizeof *s);
	s->state  = enabled ? LR_WAIT_START : LR_WAIT_FETCH_MAP;
	s->strobe = !enabled;
}

// Both layer renderers through the line, clock by clock, replaying the writes
// made during it. render_start rises at dot 1 (composer.v:147-162) and the
// next one ends the line. taken[j] is set when a layer asks for VRAM at
// render_time_r j, the clock after dot j + 1.
static void
layers_run(uint16_t line_idx, struct layer_fetch *s, uint8_t (*lb_this)[SCREEN_WIDTH],
           uint8_t (*lb_next)[SCREEN_WIDTH], uint8_t *taken)
{
	uint8_t r[2][7];
	memcpy(r, fetch_reg_layer, sizeof r);
	struct layer_dec d[2];
	layer_decode(r[0], line_idx, &d[0]);
	layer_decode(r[1], line_idx, &d[1]);
	uint8_t             dc = fetch_reg_comp[0];
	uint16_t            k  = 0;
	if (taken) {
		memset(taken, 0, SPRITE_RENDER_TIME + 1);
	}
	for (int c = 0; c <= VGA_SCAN_WIDTH; c++) {
		for (; k < fetch_log_n && fetch_log[k].dot <= c; k++) {
			const fetch_write_t *e = &fetch_log[k];
			if (e->reg < FL_DC_VIDEO) {
				r[e->reg / 7][e->reg % 7] = e->value;
				layer_decode(r[e->reg / 7], line_idx, &d[e->reg / 7]);
			} else if (e->reg == FL_DC_VIDEO) {
				dc = e->value;
			}
		}
		if (c == 0) {
			continue;
		}
		// Both renderers done, or stalled with nothing left that could enable
		// them: the rest of the line changes nothing.
		if (c > 1) {
			const int i0 = layer_fsm_idle(&s[0], dc & 0x10), i1 = layer_fsm_idle(&s[1], dc & 0x20);
			if (i0 && i1 && (k == fetch_log_n || i0 + i1 == 4)) {
				break;
			}
		}
		// vram_if.v:133-165, top.v:700-706: layer 0 wins over layer 1, a
		// disabled layer's request is masked, and the ack follows a clock later.
		// The read returns what was at the address the clock it was granted.
		const bool     req0  = s[0].strobe && !s[0].ack && (dc & 0x10);
		const bool     req1  = s[1].strobe && !s[1].ack && (dc & 0x20);
		const uint16_t addr0 = s[0].bus_addr, addr1 = s[1].bus_addr;
		// Indexed by sprite_renderer.v's render_time_r, two clocks behind c.
		if (taken && c >= 2 && c - 2 <= SPRITE_RENDER_TIME) {
			taken[c - 2] = req0 || req1;
		}
		// A pixel is written the clock after it is made (layer_renderer.v:546-563),
		// and from dot 0 that is into the other buffer.
		uint8_t(*lb)[SCREEN_WIDTH] = c >= VGA_SCAN_WIDTH - 1 ? lb_next : lb_this;
		layer_fetch_clock(&s[0], &d[0], c == 1, lb[0]);
		layer_fetch_clock(&s[1], &d[1], c == 1, lb[1]);
		s[0].ack = req0;
		s[1].ack = req1 && !req0;
		if (s[0].ack) s[0].ack_addr = addr0;
		if (s[1].ack) s[1].ack_addr = addr1;
	}
}

static void
fetch_layers_timed(uint16_t line_idx)
{
	layers_run(line_idx, layer_fsm, layer_line, layer_lb[line_buf_sel ^ 1], NULL);
}

// The clocks of this line on which a layer holds VRAM, which the sprite
// renderer, last in vram_if.v:124-138, waits out. Timing does not depend on
// line_idx or on VRAM, so a line that starts settled, with no writes during
// it and the layer registers of the last one, reuses its answer.
static uint8_t layer_bus_taken[SPRITE_RENDER_TIME + 1];
static uint8_t layer_bus_key[2 * 7 + 1];
static bool    layer_bus_valid;

static const uint8_t *
layer_bus_schedule(void)
{
	uint8_t key[sizeof layer_bus_key];
	memcpy(key, fetch_reg_layer, 2 * 7);
	key[2 * 7] = fetch_reg_comp[0] & 0x30;
	bool cacheable = fetch_log_n == 0 && !fetch_log_lost;
	for (uint8_t l = 0; l < 2 && cacheable; l++) {
		struct layer_fetch settled;
		layer_fsm_settle(&settled, key[2 * 7] & (0x10 << l));
		cacheable = memcmp(&settled, &layer_fsm[l], sizeof settled) == 0;
	}
	if (cacheable && layer_bus_valid && memcmp(key, layer_bus_key, sizeof key) == 0) {
		return layer_bus_taken;
	}
	if (!(key[2 * 7]) && fetch_log_n == 0) {
		memset(layer_bus_taken, 0, sizeof layer_bus_taken);
	} else {
		static uint8_t     scratch[2][2][SCREEN_WIDTH];
		struct layer_fetch s[2];
		memcpy(s, layer_fsm, sizeof s);
		layers_run(0, s, scratch[0], scratch[1], layer_bus_taken);
	}
	layer_bus_valid = cacheable;
	memcpy(layer_bus_key, key, sizeof key);
	return layer_bus_taken;
}

// Sprite attributes as slot i read them at dot d, undoing later writes.
static const struct video_sprite_properties *
sprite_props_at(int i, uint32_t d, struct video_sprite_properties *out)
{
	bool late = false;
	for (uint16_t k = 0; k < sprite_log_n && !late; k++) {
		late = sprite_log[k].slot == i && sprite_log[k].dot > d;
	}
	if (!late) {
		return &sprite_properties[i];
	}
	uint8_t now[8];
	memcpy(now, sprite_data[i], 8);
	for (int k = sprite_log_n - 1; k >= 0; k--) {
		if (sprite_log[k].slot == i && sprite_log[k].dot > d) {
			sprite_data[i][sprite_log[k].offset] = sprite_log[k].old;
		}
	}
	refresh_sprite_properties(i);
	*out = sprite_properties[i];
	memcpy(sprite_data[i], now, 8);
	refresh_sprite_properties(i);
	return out;
}

// Build display line y's layer and sprite buffers. Timed: at the end of the
// line before it, from fetch_reg_* and the log. Otherwise from the registers
// now.
static void
fetch_line(uint16_t y, bool timed)
{
	if (!timed) {
		memcpy(fetch_reg_layer, reg_layer, sizeof(fetch_reg_layer));
		memcpy(fetch_layer_properties, layer_properties, sizeof(fetch_layer_properties));
		fetch_reg_comp[0] = reg_composer[0];
		fetch_reg_comp[1] = reg_composer[2];
		fetch_reg_comp[2] = reg_composer[6];
		fetch_reg_comp[3] = reg_composer[7];
		fetch_log_n    = 0;
		fetch_log_lost = false;
		sprite_log_n   = 0;
	}
	fetched_y = y;

	// composer.v:147-162 steps the scaled line counter at dot 0.
	uint8_t comp[FL_COMP_N];
	memcpy(comp, fetch_reg_comp, sizeof comp);
	bool layers_written = fetch_log_lost;
	for (uint16_t k = 0; k < fetch_log_n; k++) {
		const fetch_write_t *e = &fetch_log[k];
		if (e->reg >= FL_DC_VIDEO && e->dot == 0) {
			comp[e->reg - FL_DC_VIDEO] = e->value;
		}
		layers_written |= e->reg <= FL_DC_VIDEO;
	}
	const uint8_t  dc_video = comp[0];
	const uint8_t  vscale   = comp[1];
	const uint16_t vstart   = comp[2] << 1;
	const uint16_t vstop    = comp[3] << 1;

	bool     held = false;
	bool     sprite_start;
	uint16_t sprite_y;
	if ((dc_video & 3) > 1) { // 480i or 240p
		if ((y >> 1) == 0) {
			eff_y_fp = y * (vscale << 9);
		} else if (((y & 0xfffe) >= vstart) && ((y & 0xfffe) < vstop)) {
			eff_y_fp += (vscale << 10);
		}
		sprite_y     = eff_y_fp >> 16;
		sprite_start = y < SCREEN_HEIGHT;
	} else {
		const uint16_t prev_y = eff_y_fp >> 16;
		if (y == 0) {
			eff_y_fp = 0;
		} else if ((y > vstart) && (y < vstop)) {
			if ((eff_y_fp >> 16) < 480) {
				eff_y_fp += (vscale << 9);
			} else {
				held = true;
			}
		}
		// composer.v:147-166, a line ahead of the line shown: render_start on the
		// first line at or past VSTART, then on each line after one inside the
		// window while the scaled counter is below 480.
		if (y == 0) {
			comp_vstarted = 0;
		}
		sprite_y = eff_y_fp >> 16;
		if (!comp_vstarted && y >= vstart) {
			comp_vstarted = 1;
			sprite_start  = true;
			sprite_y      = 0;
		} else if (y == vstop && y > vstart) {
			sprite_start = prev_y < 480;
			sprite_y     = (eff_y_fp + (vscale << 9)) >> 16;
		} else {
			sprite_start = y > vstart && y < vstop && !held;
		}
	}

	if ((dc_video & 8) && (dc_video & 3) > 1) { // progressive NTSC/RGB mode
		y &= 0xfffe;
	}
	if ((dc_video & 3) > 1 && sprite_y >= 480) {
		sprite_y = 480 - (y & 1);
	}

	// sprite_renderer.v takes no enable: DC_VIDEO gates only the composer
	// (composer.v:195-199). composer.v:130 erases each buffer after display, so
	// a line without render_start has no sprites below index 640.
	if (sprite_start) {
		render_sprite_line(sprite_y);
	} else {
		memset(sprite_line_col, 0, SCREEN_WIDTH);
		memset(sprite_line_z, 0, SCREEN_WIDTH);
		sprite_apply_carry();
	}

	if (y < SCREEN_HEIGHT) {
		uint16_t eff_y = (eff_y_fp >> 16);
		if ((dc_video & 3) > 1 && eff_y >= 480) eff_y = 480 - (y & 1);

		// Charge this line's VERA fetch traffic. Deliberately ABOVE the warp
		// guard below, unlike everything else here: under -warp the layers
		// are skipped for 63 frames in 64, but VERA would not skip them. The
		// count is modelled from the registers, so it costs a few arithmetic
		// ops either way. Indexed by the DISPLAY line y, not by eff_y: the
		// hardware fetches once per display line regardless of DC_VSCALE.
		if (vera_bandwidth_active) {
			const vera_bw_layer_t l0 = vera_bw_layer_of(0);
			const vera_bw_layer_t l1 = vera_bw_layer_of(1);
			vera_bandwidth_line(y, &l0, &l1);
		}

		if (!(warp_mode && (frame_count & 63))) {
			// Snapshot what this line displayed, for debug views. After the
			// warp guard, so it describes only lines that are really drawn.
			// "Enabled" means the layer reached the screen on this line: outside
			// the active window, or with output off, the composer ignores it.
			const uint8_t out_mode       = dc_video & 3;
			const bool    line_displayed = (out_mode != 0) && (y >= vstart) && (y < vstop);

			memcpy(line_reg_layer[y], fetch_reg_layer, sizeof(line_reg_layer[y]));
			line_eff_y[y]         = eff_y;
			line_layer_enabled[y] = line_displayed
				? (uint8_t)(((dc_video & 0x10) ? 1 : 0) | ((dc_video & 0x20) ? 2 : 0))
				: 0;
			for (uint8_t l = 0; l < 2; l++) {
				// Bitmap mode has no scroll (VERA forces it to 0), so there the
				// row is eff_y.
				line_layer_row[y][l] = fetch_layer_properties[l].bitmap_mode
					? eff_y
					: (uint16_t)calc_layer_eff_y(&fetch_layer_properties[l], eff_y);
			}
			line_state_valid[y] = true;
			// Progressive modes draw only the even line of each pair.
			if ((dc_video & 8) && (dc_video & 3) > 1 && y + 1 < SCREEN_HEIGHT) {
				line_state_valid[y + 1] = false;
			}

			// No render_start on a held line, so its buffers keep what was
			// drawn into them two lines ago.
			if (held) {
				layer_fsm_settle(&layer_fsm[0], dc_video & 0x10);
				layer_fsm_settle(&layer_fsm[1], dc_video & 0x20);
			} else if (timed && !fetch_log_lost &&
			           (layers_written || layer_fsm_unfinished(&layer_fsm[0]) ||
			            layer_fsm_unfinished(&layer_fsm[1]))) {
				fetch_layers_timed(eff_y);
			} else {
				for (uint8_t l = 0; l < 2; l++) {
					layer_fsm_settle(&layer_fsm[l], dc_video & (0x10 << l));
					if (!(dc_video & (0x10 << l))) {
						continue;
					}
					if (fetch_layer_properties[l].text_mode) {
						render_layer_line_text(l, eff_y);
					} else if (fetch_layer_properties[l].bitmap_mode) {
						render_layer_line_bitmap(l, eff_y);
					} else {
						render_layer_line_tile(l, eff_y);
					}
				}
			}
		}
	} else {
		// Blanking has no render_start: a renderer still busy finishes there.
		layer_fsm_settle(&layer_fsm[0], dc_video & 0x10);
		layer_fsm_settle(&layer_fsm[1], dc_video & 0x20);
	}

	memcpy(fetch_reg_layer, reg_layer, sizeof(fetch_reg_layer));
	memcpy(fetch_layer_properties, layer_properties, sizeof(fetch_layer_properties));
	fetch_reg_comp[0] = reg_composer[0];
	fetch_reg_comp[1] = reg_composer[2];
	fetch_reg_comp[2] = reg_composer[6];
	fetch_reg_comp[3] = reg_composer[7];
	fetch_log_n    = 0;
	fetch_log_lost = false;
	sprite_log_n   = 0;
}

// Compose line y up to pixel scan_pos_x, exclusive.
static void
render_line(uint16_t y, float scan_pos_x)
{
	uint8_t dc_video = reg_composer[0];
	uint16_t vstart = reg_composer[6] << 1;
	uint16_t vstop = reg_composer[7] << 1;

	if (y != y_prev) {
		y_prev = y;
		s_pos_x_p = 0;
		if (fetched_y != y) {
			fetch_line(y, false);
		}
	}

	if ((dc_video & 8) && (dc_video & 3) > 1) { // progressive NTSC/RGB mode
		y &= 0xfffe;
	}

	// refresh palette for next entry
	if (video_palette.dirty) {
		refresh_palette();
	}

	if (y >= SCREEN_HEIGHT) {
		return;
	}

	if (scan_pos_x < 0) {
		scan_pos_x = 0;
	}
	uint16_t s_pos_x = round(scan_pos_x);
	if (s_pos_x > SCREEN_WIDTH) {
		s_pos_x = SCREEN_WIDTH;
	}
	if (s_pos_x <= s_pos_x_p) {
		return;
	}

	if (s_pos_x_p == 0) {
		eff_x_fp = 0;
	}

	uint8_t out_mode = reg_composer[0] & 3;

	uint8_t border_color = reg_composer[3];
	uint16_t hstart = reg_composer[4] << 2;
	uint16_t hstop = reg_composer[5] << 2;

	layer_line_enable[0] = dc_video & 0x10;
	layer_line_enable[1] = dc_video & 0x20;
	sprite_line_enable   = dc_video & 0x40;

	if (warp_mode && (frame_count & 63)) {
		s_pos_x_p = s_pos_x;
		return;
	}

	// If video output is enabled, calculate color indices for line.
	if (out_mode != 0) {
		// Add border after if required.
		if (y < vstart || y >= vstop) {
			memset(col_line + s_pos_x_p, border_color, s_pos_x - s_pos_x_p);
		} else {
			hstart = hstart < 640 ? hstart : 640;
			hstop = hstop < 640 ? hstop : 640;

			for (uint16_t x = s_pos_x_p; x < hstart && x < s_pos_x; ++x) {
				col_line[x] = border_color;
			}

			const uint32_t scale = reg_composer[1];
			const uint8_t *l0 = layer_line[0];
			const uint8_t *l1 = layer_line[1];
			const bool     e0 = dc_video & 0x10, e1 = dc_video & 0x20, es = dc_video & 0x40;
			const uint8_t  sel = line_buf_sel;
			uint8_t       *out = col_line;
			uint32_t       fx  = eff_x_fp;
			for (uint16_t x = MAX(hstart, s_pos_x_p); x < hstop && x < s_pos_x; ++x) {
				uint16_t eff_x = fx >> 16;
				if (eff_x < SCREEN_WIDTH) {
					out[x] = calculate_line_col_index(es ? sprite_line_z[eff_x] : 0, sprite_line_col[eff_x],
					                                  e0 ? l0[eff_x] : 0, e1 ? l1[eff_x] : 0);
					// composer.v:178 holds the counter once it reaches 640.
					fx += (scale << 9);
				} else if (es && eff_x < SCREEN_WIDTH + 2) {
					const uint8_t e = eff_x - SCREEN_WIDTH;
					out[x] = calculate_line_col_index(sprite_edge_z[sel][e], sprite_edge_col[sel][e], 0, 0);
				} else {
					out[x] = 0;
				}
			}
			eff_x_fp = fx;
			for (uint16_t x = MAX(hstop, s_pos_x_p); x < s_pos_x; ++x) {
				col_line[x] = border_color;
			}
		}
	}

	// Look up all color indices.
	uint32_t* framebuffer4_begin = ((uint32_t*)framebuffer) + (y * SCREEN_WIDTH) + s_pos_x_p;
	{
		uint32_t* framebuffer4 = framebuffer4_begin;
		for (uint16_t x = s_pos_x_p; x < s_pos_x; x++) {
			*framebuffer4++ = video_palette.entries[col_line[x]];
		}
	}

	// NTSC overscan
	if (out_mode == 2) {
		uint32_t* framebuffer4 = framebuffer4_begin;
		for (uint16_t x = s_pos_x_p; x < s_pos_x; x++)
		{
			if (x < SCREEN_WIDTH * TITLE_SAFE_X ||
				x > SCREEN_WIDTH * (1 - TITLE_SAFE_X) ||
				y < SCREEN_HEIGHT * TITLE_SAFE_Y ||
				y > SCREEN_HEIGHT * (1 - TITLE_SAFE_Y)) {

				// Divide RGB elements by 4.
				*framebuffer4 &= 0x00fcfcfc;
				*framebuffer4 >>= 2;
			}
			framebuffer4++;
		}
	}

	s_pos_x_p = s_pos_x;
}

// Compose the current line up to pixel x, exclusive: what the beam has shown
// before a composer or palette write lands.
static void
scan_out_to(int x)
{
	if (reg_composer[0] & 2) {
		video_step(MHZ, 0, true);
		return;
	}
	render_line(vga_scan_pos_y - VGA_Y_OFFSET, (float)(x < 0 ? 0 : x));
}

static void
vsync_isr(void)
{
	if (sprite_line_collisions != 0) {
		isr |= 4;
	}
	isr = (isr & 0xf) | sprite_line_collisions;
	sprite_line_collisions = 0;
	isr |= 1; // VSYNC IRQ
}

static void
update_isr_and_coll(uint16_t y, uint16_t compare)
{
	if (y == SCREEN_HEIGHT) {
		vsync_isr();
	}
	if (y == compare) { // LINE IRQ
		isr |= 2;
	}
}

// composer.v:70-92: y_counter_r resets with the VGA counter, so it equals the
// line until the first next_frame (end of line 523) and runs one ahead after.
static uint16_t
comp_line_counter(uint16_t y)
{
	if (comp_first_frame) {
		return y;
	}
	return y == SCAN_HEIGHT - 1 ? 0 : y + 1;
}

// composer.v:126-127: y_counter_rr is the previous line's y_counter_r.
static uint16_t
comp_scanline(uint16_t y)
{
	const uint16_t prev = y == 0 ? 0 : comp_first_frame ? y - 1 : y;
	return prev >= 512 ? 511 : comp_line_counter(y) & 0x1FF;
}

void
video_catch_up(unsigned offset)
{
	if (offset > step_consumed) {
		step_consumed_frame |= video_step(MHZ, (float)(offset - step_consumed), false);
		step_consumed = offset;
	}
}

bool
video_step_rest(float mhz, unsigned clocks)
{
	const unsigned rest = clocks > step_consumed ? clocks - step_consumed : 0;
	const bool new_frame = video_step(mhz, (float)rest, false) | step_consumed_frame;
	step_consumed = 0;
	step_consumed_frame = false;
	return new_frame;
}

bool
video_step(float mhz, float steps, bool midline)
{
	uint16_t y = 0;
	bool ntsc_mode = reg_composer[0] & 2;
	bool new_frame = false;
	sprite_trace_cpu_cycles += (uint32_t)steps;
	vga_scan_pos_x += PIXEL_FREQ * steps / mhz;
	if (vga_scan_pos_x > VGA_SCAN_WIDTH) {
		vga_scan_pos_x -= VGA_SCAN_WIDTH;
		if (!ntsc_mode) {
			render_line(vga_scan_pos_y - VGA_Y_OFFSET, VGA_SCAN_WIDTH);
			line_buf_sel ^= 1;
			// composer.v:99-104: compared at the line's last clock.
			if (comp_line_counter(vga_scan_pos_y) == irq_line) {
				line_irq_pending = 1;
			}
		}
		if (vga_scan_pos_y == SCAN_HEIGHT - 2) {
			comp_first_frame = 0;
		}
		vga_scan_pos_y++;
		if (vga_scan_pos_y == SCAN_HEIGHT) {
			vga_scan_pos_y = 0;
			if (!ntsc_mode) {
				new_frame = true;
				frame_count++;
			}
		}
		// Line 480's sprites are drawn during line 479, before frame_done.
		if (!ntsc_mode) {
			fetch_line(vga_scan_pos_y - VGA_Y_OFFSET, true);
		}
		if (!ntsc_mode && vga_scan_pos_y - VGA_Y_OFFSET == SCREEN_HEIGHT) {
			vsync_isr();
		}
	} else if (midline) {
		if (!ntsc_mode) {
			// Pixel x is composed at dot x + 1, so only pixels below the
			// current dot - 1 are final.
			render_line(vga_scan_pos_y - VGA_Y_OFFSET, floorf(vga_scan_pos_x) - 2);
		}
	}
	// top.v:451-452: line_irq is registered, so the status shows a clock later.
	if (line_irq_pending && vga_scan_pos_x >= 2) {
		line_irq_pending = 0;
		isr |= 2;
	}
	ntsc_half_cnt += PIXEL_FREQ * steps / mhz;
	if (ntsc_half_cnt > NTSC_HALF_SCAN_WIDTH) {
		ntsc_half_cnt -= NTSC_HALF_SCAN_WIDTH;
		if (ntsc_mode) {
			if (ntsc_scan_pos_y < SCAN_HEIGHT) {
				y = ntsc_scan_pos_y - NTSC_Y_OFFSET_LOW;
				if ((y & 1) == 0) {
					render_line(y, NTSC_HALF_SCAN_WIDTH);
				}
			} else {
				y = ntsc_scan_pos_y - NTSC_Y_OFFSET_HIGH;
				if ((y & 1) == 0) {
					render_line(y | 1, NTSC_HALF_SCAN_WIDTH);
				}
			}
		}
		ntsc_scan_pos_y++;
		if (ntsc_scan_pos_y == SCAN_HEIGHT) {
			reg_composer[0] |= 0x80;
			if (ntsc_mode) {
				new_frame = true;
				frame_count++;
			}
		}
		if (ntsc_scan_pos_y == SCAN_HEIGHT*2) {
			reg_composer[0] &= ~0x80;
			ntsc_scan_pos_y = 0;
			if (ntsc_mode) {
				new_frame = true;
				frame_count++;
			}
		}
		if (ntsc_mode) {
			// this is correct enough for even screen heights
			if (ntsc_scan_pos_y < SCAN_HEIGHT) {
				update_isr_and_coll(ntsc_scan_pos_y - NTSC_Y_OFFSET_LOW, irq_line & ~1);
			} else {
				update_isr_and_coll(ntsc_scan_pos_y - NTSC_Y_OFFSET_HIGH, irq_line & ~1);
			}
		}
	} else if (midline) {
		if (ntsc_mode) {
			if (ntsc_scan_pos_y < SCAN_HEIGHT) {
				y = ntsc_scan_pos_y - NTSC_Y_OFFSET_LOW;
				if ((y & 1) == 0) {
					render_line(y, ntsc_half_cnt);
				}
			} else {
				y = ntsc_scan_pos_y - NTSC_Y_OFFSET_HIGH;
				if ((y & 1) == 0) {
					render_line(y | 1, ntsc_half_cnt);
				}
			}
		}
	}

	// Roll the sprite trace at the frame boundary: finish the frame just ended,
	// publish it for the debugger, and seed the next one from the attribute
	// table as it stands now, which is what the beam will start out drawing.
	if (new_frame && sprite_trace_active) {
		sprite_trace_frame_advance((uint32_t)frame_count, sprite_trace_cpu_cycles, sprite_data);
	}

	// And the bandwidth frame, on the same boundary and for the same reason:
	// this is VERA's own frame, which is the one its bus traffic belongs to.
	// perf_budget closes its frame from main.c instead, because CPU cycles are
	// the CPU's business -- the two are driven by the same vsync, so the panel
	// can show them side by side.
	//
	// frame_count has already been incremented above, and this closes the frame
	// that just ENDED, so it is stamped with the ordinal one below -- matching
	// sprite_trace, which takes frame_count as the frame it is about to start.
	if (new_frame) {
		vera_bandwidth_frame_end((uint32_t)(frame_count - 1));
	}

	return new_frame;
}

void
video_set_random_seed(uint32_t seed)
{
	vram_seed   = seed;
	vram_seeded = true;
}

void
video_set_ignore_host_input(bool on)
{
	ignore_host_input = on;
}

// ─── Save states ─────────────────────────────────────────────────────────────
//
// What VERA needs to carry on exactly: VRAM, palette, sprite attributes, every
// register, the FX unit, the scan position (the floats bitwise), the frame
// counter that paces the warp render skip, and the renderer's pipeline -- the
// two-line register history raster effects are timed by, the derived layer and
// sprite properties, and the sprite line buffers that collisions come from.
//
// Left out on purpose, because they are output and not state: everything
// render_line() writes after its warp guard (the composited line, the layer
// line buffers, eff_x_fp, the framebuffer and the per-line debug history).
// Under -warp those are skipped 63 frames in 64, so including them would make
// a warped run's state differ from an unwarped one. At a frame boundary,
// where states are taken, each is rebuilt before it is next read.

// The layer and sprite property structs as byte images with fixed offsets,
// which are the layouts the structs have always had on every target: states
// from when they were saved whole read the same. The asserts say so, and fail
// when a field is added without a place in its image.
#define LAYER_IMAGE  56
#define SPRITE_IMAGE 24
_Static_assert(sizeof(struct video_layer_properties) == LAYER_IMAGE &&
               offsetof(struct video_layer_properties, min_eff_x) == 44 &&
               offsetof(struct video_layer_properties, color_fields_max) == 55,
               "struct video_layer_properties changed: update its save-state image");
_Static_assert(sizeof(struct video_sprite_properties) == SPRITE_IMAGE &&
               offsetof(struct video_sprite_properties, sprite_address) == 16 &&
               offsetof(struct video_sprite_properties, palette_offset) == 20,
               "struct video_sprite_properties changed: update its save-state image");

static void
layer_image(struct video_layer_properties *p, uint8_t *b, bool to_image)
{
	if (to_image) {
		memset(b, 0, LAYER_IMAGE);
		b[0] = p->color_depth;
		ss_le32(b + 4, p->map_base);
		ss_le32(b + 8, p->tile_base);
		b[12] = p->text_mode;
		b[13] = p->text_mode_256c;
		b[14] = p->tile_mode;
		b[15] = p->bitmap_mode;
		ss_le16(b + 16, p->hscroll);
		ss_le16(b + 18, p->vscroll);
		b[20] = p->mapw_log2;
		b[21] = p->maph_log2;
		ss_le16(b + 22, p->tilew);
		ss_le16(b + 24, p->tileh);
		b[26] = p->tilew_log2;
		b[27] = p->tileh_log2;
		ss_le16(b + 28, p->mapw_max);
		ss_le16(b + 30, p->maph_max);
		ss_le16(b + 32, p->tilew_max);
		ss_le16(b + 34, p->tileh_max);
		ss_le16(b + 36, p->layerw_max);
		ss_le16(b + 38, p->layerh_max);
		b[40] = p->tile_size_log2;
		ss_le32(b + 44, (uint32_t)p->min_eff_x);
		ss_le32(b + 48, (uint32_t)p->max_eff_x);
		b[52] = p->bits_per_pixel;
		b[53] = p->first_color_pos;
		b[54] = p->color_mask;
		b[55] = p->color_fields_max;
	} else {
		p->color_depth      = b[0];
		p->map_base         = ss_rd32(b + 4);
		p->tile_base        = ss_rd32(b + 8);
		p->text_mode        = b[12] != 0;
		p->text_mode_256c   = b[13] != 0;
		p->tile_mode        = b[14] != 0;
		p->bitmap_mode      = b[15] != 0;
		p->hscroll          = ss_rd16(b + 16);
		p->vscroll          = ss_rd16(b + 18);
		p->mapw_log2        = b[20];
		p->maph_log2        = b[21];
		p->tilew            = ss_rd16(b + 22);
		p->tileh            = ss_rd16(b + 24);
		p->tilew_log2       = b[26];
		p->tileh_log2       = b[27];
		p->mapw_max         = ss_rd16(b + 28);
		p->maph_max         = ss_rd16(b + 30);
		p->tilew_max        = ss_rd16(b + 32);
		p->tileh_max        = ss_rd16(b + 34);
		p->layerw_max       = ss_rd16(b + 36);
		p->layerh_max       = ss_rd16(b + 38);
		p->tile_size_log2   = b[40];
		p->min_eff_x        = (int32_t)ss_rd32(b + 44);
		p->max_eff_x        = (int32_t)ss_rd32(b + 48);
		p->bits_per_pixel   = b[52];
		p->first_color_pos  = b[53];
		p->color_mask       = b[54];
		p->color_fields_max = b[55];
	}
}

static void
sprite_image(struct video_sprite_properties *p, uint8_t *b, bool to_image)
{
	if (to_image) {
		memset(b, 0, SPRITE_IMAGE);
		b[0]  = (uint8_t)p->sprite_zdepth;
		b[1]  = p->sprite_collision_mask;
		ss_le16(b + 2, (uint16_t)p->sprite_x);
		ss_le16(b + 4, (uint16_t)p->sprite_y);
		b[6]  = p->sprite_width_log2;
		b[7]  = p->sprite_height_log2;
		b[8]  = p->sprite_width;
		b[9]  = p->sprite_height;
		b[10] = p->hflip;
		b[11] = p->vflip;
		b[12] = p->color_mode;
		ss_le32(b + 16, p->sprite_address);
		ss_le16(b + 20, p->palette_offset);
	} else {
		p->sprite_zdepth         = (int8_t)b[0];
		p->sprite_collision_mask = b[1];
		p->sprite_x              = (int16_t)ss_rd16(b + 2);
		p->sprite_y              = (int16_t)ss_rd16(b + 4);
		p->sprite_width_log2     = b[6];
		p->sprite_height_log2    = b[7];
		p->sprite_width          = b[8];
		p->sprite_height         = b[9];
		p->hflip                 = b[10] != 0;
		p->vflip                 = b[11] != 0;
		p->color_mode            = b[12];
		p->sprite_address        = ss_rd32(b + 16);
		p->palette_offset        = ss_rd16(b + 20);
	}
}

static void layer_image_v(void *p, uint8_t *b, bool to) { layer_image(p, b, to); }
static void sprite_image_v(void *p, uint8_t *b, bool to) { sprite_image(p, b, to); }

// n structs through their images, written or read in one piece.
static void
props_io(ss_writer *w, ss_reader *r, void *structs, size_t struct_size, int n, size_t image_size,
         void (*image)(void *, uint8_t *, bool))
{
	uint8_t img[128 * SPRITE_IMAGE]; // the largest: every sprite
	const size_t len = (size_t)n * image_size;
	if (len > sizeof img) {
		if (w) w->failed = true; else r->failed = true;
		return;
	}
	if (w) {
		for (int i = 0; i < n; i++) image((uint8_t *)structs + i * struct_size, img + i * image_size, true);
		ss_put(w, img, len);
	} else {
		ss_get(r, img, len);
		if (r->failed) return;
		for (int i = 0; i < n; i++) image((uint8_t *)structs + i * struct_size, img + i * image_size, false);
	}
}

#define VERA_CHUNK_VERSION 6

// Bytes only version 4 has (previous composer and layer registers, old line
// enables, the held line) and bytes only version 5 has (fetch state). A chunk
// of another length does not load, so version 5 pads to version 4's.
enum {
	VERA_V4_ONLY = 2 * COMPOSER_SLOTS + 2 * NUM_LAYERS * LAYER_IMAGE + 3 + 1 + 2 * 2 * SCREEN_WIDTH,
	VERA_V5_ONLY = NUM_LAYERS * LAYER_IMAGE + 2 * 2 * SCREEN_WIDTH + 2 * 7 + FL_COMP_N + 1 + 2 + 2 * (10 + 2 * 4 + 4 * 3),
	VERA_V6_ONLY = 2 * 2 + 6 + 1,
	VERA_V5_PAD  = VERA_V4_ONLY - VERA_V5_ONLY,
	VERA_V6_PAD  = VERA_V5_PAD - VERA_V6_ONLY,
};
_Static_assert(VERA_V6_PAD > 0, "the version 6 VERA chunk outgrew version 4's");
_Static_assert(sizeof(sprite_edge_mask) == 2 * 2, "VERA_V6_ONLY no longer counts the version 6 fields");
_Static_assert(sizeof(bool) == 1 && sizeof(layer_lb) == 2 * 2 * SCREEN_WIDTH && sizeof(fetch_reg_layer) == 2 * 7 &&
               sizeof(fetched_y) == 2, "VERA_V5_ONLY no longer counts the version 5 fields");

static void
video_state_io(ss_writer *w, ss_reader *r, int version)
{
#define SS_BLOCK(x) do { if (w) ss_put(w, &(x), sizeof(x)); else ss_get(r, &(x), sizeof(x)); } while (0)
	SS_BLOCK(video_ram);
	SS_BLOCK(palette);
	SS_BLOCK(sprite_data);
	SS_BLOCK(io_addr);
	SS_BLOCK(io_rddata);
	SS_BLOCK(io_inc);
	SS_BLOCK(io_addrsel);
	SS_BLOCK(io_dcsel);
	SS_BLOCK(ien);
	SS_BLOCK(isr);
	SS_BLOCK(irq_line);
	SS_BLOCK(reg_layer);
	SS_BLOCK(reg_composer);
	if (r && version < 5) {
		uint8_t prev_reg_composer[2][COMPOSER_SLOTS];
		SS_BLOCK(prev_reg_composer);
	}
	props_io(w, r, layer_properties, sizeof layer_properties[0], NUM_LAYERS, LAYER_IMAGE, layer_image_v);
	if (w || version >= 5) {
		props_io(w, r, fetch_layer_properties, sizeof fetch_layer_properties[0], NUM_LAYERS, LAYER_IMAGE,
		         layer_image_v);
	} else {
		struct video_layer_properties prev[2][NUM_LAYERS];
		props_io(w, r, prev, sizeof prev[0][0], 2 * NUM_LAYERS, LAYER_IMAGE, layer_image_v);
	}
	props_io(w, r, sprite_properties, sizeof sprite_properties[0], 128, SPRITE_IMAGE, sprite_image_v);
	SS_BLOCK(sprite_line_col);
	SS_BLOCK(sprite_line_z);
	SS_BLOCK(sprite_line_mask);
	SS_BLOCK(sprite_line_collisions);
	SS_BLOCK(layer_line_enable);
	if (r && version < 5) {
		bool old_layer_line_enable[2] = {0}, old_sprite_line_enable = 0;
		SS_BLOCK(old_layer_line_enable);
		SS_BLOCK(old_sprite_line_enable);
	}
	SS_BLOCK(sprite_line_enable);

	SS_BLOCK(fx_addr1_mode);
	SS_BLOCK(fx_x_pixel_increment);
	SS_BLOCK(fx_y_pixel_increment);
	SS_BLOCK(fx_x_pixel_position);
	SS_BLOCK(fx_y_pixel_position);
	// v1 held the fill length here, now derived; v2 keeps the slot so sizes match.
	if (w) {
		ss_put_u16(w, fx_pending_pos_calc ? 1 : 0);
	} else {
		uint16_t slot = ss_get_u16(r);
		fx_pending_pos_calc = version >= 2 && slot != 0;
	}
	SS_BLOCK(fx_affine_tile_base);
	SS_BLOCK(fx_affine_map_base);
	SS_BLOCK(fx_affine_map_size);
	SS_BLOCK(fx_4bit_mode);
	SS_BLOCK(fx_16bit_hop);
	SS_BLOCK(fx_cache_byte_cycling);
	SS_BLOCK(fx_cache_fill);
	SS_BLOCK(fx_cache_write);
	SS_BLOCK(fx_trans_writes);
	SS_BLOCK(fx_2bit_poly);
	SS_BLOCK(fx_2bit_poking);
	SS_BLOCK(fx_cache_increment_mode);
	SS_BLOCK(fx_cache_nibble_index);
	SS_BLOCK(fx_cache_byte_index);
	SS_BLOCK(fx_multiplier);
	SS_BLOCK(fx_subtract);
	SS_BLOCK(fx_affine_clip);
	SS_BLOCK(fx_16bit_hop_align);
	SS_BLOCK(fx_nibble_bit);
	SS_BLOCK(fx_nibble_incr);
	SS_BLOCK(fx_cache);
	SS_BLOCK(fx_mult_accumulator);

	SS_BLOCK(vga_scan_pos_x);
	SS_BLOCK(vga_scan_pos_y);
	SS_BLOCK(ntsc_half_cnt);
	SS_BLOCK(ntsc_scan_pos_y);
	SS_BLOCK(frame_count);
	SS_BLOCK(y_prev);
	SS_BLOCK(s_pos_x_p);
	SS_BLOCK(eff_y_fp);
	if (w || version >= 3) {
		SS_BLOCK(line_buf_sel);
		if (w || version >= 5) {
			SS_BLOCK(layer_lb);
		} else {
			uint8_t line_render_held = 0, held_layer_line[2][2][SCREEN_WIDTH] = {{{0}}};
			SS_BLOCK(line_render_held);
			SS_BLOCK(held_layer_line);
			memset(layer_lb, 0, sizeof(layer_lb));
		}
		SS_BLOCK(sprite_edge_col);
		SS_BLOCK(sprite_edge_z);
	} else {
		line_buf_sel = 0;
		memset(layer_lb, 0, sizeof(layer_lb));
		memset(sprite_edge_col, 0, sizeof(sprite_edge_col));
		memset(sprite_edge_z, 0, sizeof(sprite_edge_z));
		memset(sprite_edge_mask, 0, sizeof(sprite_edge_mask));
		memset(&sprite_carry, 0, sizeof(sprite_carry));
	}
	if (w || version >= 4) {
		SS_BLOCK(comp_first_frame);
		SS_BLOCK(line_irq_pending);
	} else {
		comp_first_frame = 0;
		line_irq_pending = 0;
	}
	if (w || version >= 5) {
		SS_BLOCK(fetch_reg_layer);
		SS_BLOCK(fetch_reg_comp);
		if (w) {
			ss_put_u8(w, fetch_log_lost || fetch_log_n || sprite_log_n);
		} else {
			fetch_log_lost = ss_get_u8(r) != 0;
		}
		SS_BLOCK(fetched_y);
		for (int l = 0; l < 2; l++) {
			struct layer_fetch *f = &layer_fsm[l];
			uint8_t *b8[] = { &f->state, &f->strobe, &f->ack, &f->render_start, &f->busy, &f->xcnt, &f->word,
			                  &f->htile, &f->render_mapdata, &f->next_render_mapdata };
			uint16_t *b16[] = { &f->bus_addr, &f->ack_addr, &f->bitmap_addr, &f->lb_idx };
			uint32_t *b32[] = { &f->map_data, &f->tile_data, &f->render_data };
			for (size_t k = 0; k < sizeof b8 / sizeof b8[0]; k++) {
				if (w) ss_put_u8(w, *b8[k]); else *b8[k] = ss_get_u8(r);
			}
			for (size_t k = 0; k < sizeof b16 / sizeof b16[0]; k++) {
				if (w) ss_put_u16(w, *b16[k]); else *b16[k] = ss_get_u16(r);
			}
			for (size_t k = 0; k < sizeof b32 / sizeof b32[0]; k++) {
				if (w) ss_put_u32(w, *b32[k]); else *b32[k] = ss_get_u32(r);
			}
		}
		// The write logs are empty at a frame boundary, where states are saved.
		// Pads the chunk to version 4's length, which a loader requires.
		if (w || version >= 6) {
			SS_BLOCK(sprite_edge_mask);
			uint8_t *b8[] = { &sprite_carry.on, &sprite_carry.col, &sprite_carry.z, &sprite_carry.mask };
			for (size_t k = 0; k < sizeof b8 / sizeof b8[0]; k++) {
				if (w) ss_put_u8(w, *b8[k]); else *b8[k] = ss_get_u8(r);
			}
			if (w) ss_put_u16(w, sprite_carry.x); else sprite_carry.x = ss_get_u16(r);
			SS_BLOCK(comp_vstarted);
			uint8_t pad[VERA_V6_PAD] = { 0 };
			SS_BLOCK(pad);
		} else {
			memset(sprite_edge_mask, 0, sizeof(sprite_edge_mask));
			memset(&sprite_carry, 0, sizeof(sprite_carry));
			comp_vstarted = vga_scan_pos_y >= (reg_composer[6] << 1);
			uint8_t pad[VERA_V5_PAD] = { 0 };
			SS_BLOCK(pad);
		}
		if (r) {
			fetch_log_n  = 0;
			sprite_log_n = 0;
		}
	} else {
		memcpy(fetch_reg_layer, reg_layer, sizeof(fetch_reg_layer));
		memcpy(fetch_layer_properties, layer_properties, sizeof(fetch_layer_properties));
		fetch_reg_comp[0] = reg_composer[0];
		fetch_reg_comp[1] = reg_composer[2];
		fetch_reg_comp[2] = reg_composer[6];
		fetch_reg_comp[3] = reg_composer[7];
		fetch_log_n    = 0;
		fetch_log_lost = false;
		sprite_log_n   = 0;
		fetched_y      = FETCH_NONE;
		memset(layer_fsm, 0, sizeof(layer_fsm));
		memset(sprite_edge_mask, 0, sizeof(sprite_edge_mask));
		memset(&sprite_carry, 0, sizeof(sprite_carry));
		comp_vstarted = vga_scan_pos_y >= (reg_composer[6] << 1);
	}
#undef SS_BLOCK

	if (r && version < 2) {
		io_addr[0] &= 0x1ffff;
		io_addr[1] &= 0x1ffff;
	}
}

// A hash of the picture as last drawn. Output only, so not part of a state:
// under -warp it is redrawn one frame in 64, on frames the machine's own frame
// counter picks.
uint64_t
video_screen_hash(void)
{
	return x16_hash_bytes(X16_HASH_INIT, framebuffer, sizeof framebuffer);
}

void
video_state_save(ss_writer *w)
{
	ss_begin(w, "VERA", VERA_CHUNK_VERSION);
	video_state_io(w, NULL, VERA_CHUNK_VERSION);
	ss_end(w);
}

void
video_state_load(ss_reader *r)
{
	int version = ss_open(r, "VERA");
	video_state_io(NULL, r, version);
	ss_close(r);

	// The colours are derived from the palette; the per-line debug history
	// describes the machine that was replaced.
	video_palette.dirty = true;
	memset(line_state_valid, 0, sizeof(line_state_valid));
	step_consumed = 0;
	step_consumed_frame = false;
}

bool
video_get_irq_out()
{
	uint8_t tmp_isr = isr | (pcm_is_fifo_almost_empty() ? 8 : 0);
	return (tmp_isr & ien) != 0;
}

//
// saves the video memory and register content into a file
//

void
video_save(SDL_RWops *f)
{
	SDL_RWwrite(f, &video_ram[0], sizeof(uint8_t), sizeof(video_ram));
	SDL_RWwrite(f, &reg_composer[0], sizeof(uint8_t), sizeof(reg_composer));
	SDL_RWwrite(f, &palette[0], sizeof(uint8_t), sizeof(palette));
	SDL_RWwrite(f, &reg_layer[0][0], sizeof(uint8_t), sizeof(reg_layer));
	SDL_RWwrite(f, &sprite_data[0], sizeof(uint8_t), sizeof(sprite_data));
}

// The last finished frame, averaged down to w x h RGBA8888 words (0xAABBGGRR).
// w and h divide the screen evenly.
void
video_thumbnail(uint32_t *out, int w, int h)
{
	const int sx = SCREEN_WIDTH / w;
	const int sy = SCREEN_HEIGHT / h;
	const int n  = sx * sy;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t r = 0, g = 0, b = 0;
			for (int j = 0; j < sy; j++) {
				const uint8_t *p = &framebuffer[((y * sy + j) * SCREEN_WIDTH + x * sx) * 4];
				for (int i = 0; i < sx; i++, p += 4) {
					b += p[0];
					g += p[1];
					r += p[2];
				}
			}
			out[y * w + x] = 0xFF000000u | ((b / n) << 16) | ((g / n) << 8) | (r / n);
		}
	}
}

// Render the current framebuffer and present it. Deliberately does not touch the
// SDL event queue, so it is safe to call from inside the OS modal move/resize
// loop.
//
// `new_frame` says whether this is a newly produced frame rather than a repaint
// of the one already there. Both the activity-LED blend and the GIF write are
// once-per-frame effects -- the LED is blended into the framebuffer in place,
// and the recorder builds its timeline from the frames it is handed -- so a
// repaint must do neither.
static void video_render_all_body(bool new_frame);

// Everything below is the "present" host section; the debugger UI it ends with
// nests inside as ui_build / ui_draw, so it is not double-counted.
static void
video_render_all_ex(bool new_frame)
{
	if (input_log_seeking) {
		return;
	}
	host_prof_begin(HP_PRESENT);
	video_render_all_body(new_frame);
	host_prof_end(HP_PRESENT);
}

static void
video_render_all_body(bool new_frame)
{
	// for activity LED, overlay red 8x4 square into top right of framebuffer
	// for progressive modes, draw LED only on even scanlines
	//
	// Blended into the framebuffer in place, so it must happen once per frame
	// and not once per present. A repaint between throttle slices re-presents
	// the frame that is already there, LED included; blending again would drive
	// the square toward pure red a slice at a time.
	for (int y = 0; new_frame && y < 4; y+=1+!!((reg_composer[0] & 0x0b) > 0x09)) {
		for (int x = SCREEN_WIDTH - 8; x < SCREEN_WIDTH; x++) {
			uint8_t b = framebuffer[(y * SCREEN_WIDTH + x) * 4 + 0];
			uint8_t g = framebuffer[(y * SCREEN_WIDTH + x) * 4 + 1];
			uint8_t r = framebuffer[(y * SCREEN_WIDTH + x) * 4 + 2];
			r = (uint32_t)r * (255 - activity_led) / 255 + activity_led;
			g = (uint32_t)g * (255 - activity_led) / 255;
			b = (uint32_t)b * (255 - activity_led) / 255;
			framebuffer[(y * SCREEN_WIDTH + x) * 4 + 0] = b;
			framebuffer[(y * SCREEN_WIDTH + x) * 4 + 1] = g;
			framebuffer[(y * SCREEN_WIDTH + x) * 4 + 2] = r;
			framebuffer[(y * SCREEN_WIDTH + x) * 4 + 3] = 0x00;
		}
	}

	// -hidden windows are never shown, so drawing into one is work nobody sees:
	// the upload and present below are about an eighth of a warped frame, and
	// their worst spikes. The framebuffer itself, the LED above, a GIF recording
	// and the debugger window are all kept.
	const bool draw = !hidden_windows;
	if (draw) {
		render_backend_texture_update(sdlTexture, NULL, framebuffer, SCREEN_WIDTH * 4);
	}

	// Skipped for a repaint that is not a new emulated frame. The throttle
	// repaints between sleep slices to keep the window alive, and there can be
	// hundreds of those per frame at a slow target; recording them would bloat
	// the file and stretch its timeline against the emulated one.
	if (new_frame && record_gif > RECORD_GIF_PAUSED) {
		if(!GifWriteFrame(&gif_writer, framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, 2, 8, false)) {
			// if that failed, stop recording
			GifEnd(&gif_writer);
			record_gif = RECORD_GIF_DISABLED;
			printf("Unexpected end of recording.\n");
		}
		if (record_gif == RECORD_GIF_SINGLE) { // if single-shot stop recording
			record_gif = RECORD_GIF_PAUSED;  // need to close in video_end()
		}
	}

	if (draw) {
		video_clear_output();
		render_backend_copy(renderer, sdlTexture, NULL, NULL);

		if (debugger_enabled && showDebugOnRender != 0) {
			DEBUGRenderDisplay(SCREEN_WIDTH, SCREEN_HEIGHT);
		}

		render_backend_present(renderer);
		video_fallback_to_sdl();
	}

#ifdef HAS_IMGUI
	// Rebuild the debugger window's frame. Deliberately decoupled from the
	// emulated frame rate, which spans four orders of magnitude here: under
	// -warp or a 100MHz target this is reached far more often than a display
	// can show, and at 25kHz a new frame may be seconds apart -- so pacing the
	// UI off it would either burn the CPU rebuilding invisible frames or leave
	// the debugger unresponsive. Wall-clock 60Hz is right at both ends. The
	// throttle's repaint path (new_frame == false) calls in too, which is what
	// keeps the UI live while the machine is running slowly.
	if (imgui_window != NULL && (imgui_renderer != NULL || imgui_native_dx12)) {
		static Uint32 last_ui_ticks = 0;
		const Uint32 now = SDL_GetTicks();
		// Unsigned subtraction, so this stays correct across the 49-day wrap.
		if (now - last_ui_ticks >= 16) {
			last_ui_ticks = now;
			host_prof_begin(HP_UI_BUILD);
			const bool ui_ok = debug_ui_render();
			host_prof_end(HP_UI_BUILD);
			if (!ui_ok && imgui_native_dx12) {
				// Only the debugger's own swap chain failed. The main DX12
				// backend is still healthy, so leave it alone and move just
				// this window back to SDL.
				fprintf(stderr,
				        "Warning: The native DX12 debugger renderer failed; switching that "
				        "window to SDL.\n");
				debug_ui_shutdown();
				imgui_native_dx12 = false;
				video_debug_ui_use_sdl_renderer();
			}
		}
	}
#endif
}

// Present a newly completed frame without pumping the event queue. Called from
// the Windows modal move/resize loop, which owns the message queue for the
// duration of a drag; polling events from there would re-enter SDL's own
// dispatch. This is a real frame, so it records and blends the activity LED
// like any other -- only the event handling differs.
void
video_present_no_input(void)
{
	if (renderer == NULL || sdlTexture == NULL) {
		return;
	}
	video_render_all_ex(true);
}

// Re-present the frame that is already there. No new frame has been produced,
// so this must not record one or blend the activity LED again -- the throttle
// calls it between sleep slices, hundreds of times per frame at a slow target.
void
video_repaint_only(void)
{
	if (renderer == NULL || sdlTexture == NULL) {
		return;
	}
	video_render_all_ex(false);
}

static void
video_render_all(void)
{
	video_render_all_ex(true);
}

#ifdef HAS_IMGUI
// True only once the debugger window AND renderer were actually created.
//
// This is the authority on whether the UI exists; imgui_debugger_enabled is the
// authority on whether it was *asked* for. The two agree from video_init()
// onward -- both the creation-failure path below and the headless path in
// main.c clear the flag -- but callers that must not park the machine still
// test this one, so the answer does not depend on that agreement holding.
bool
video_debug_ui_available(void)
{
	return imgui_window != NULL && (imgui_renderer != NULL || imgui_native_dx12);
}

static void video_imgui_debug_key(SDL_Keysym ks);

// Whether an SDL window id belongs to the debugger window. Callers outside
// video.c use this to tell "close the debugger" from "close the emulator".
bool
video_is_debug_ui_window(Uint32 window_id)
{
	return imgui_window != NULL && window_id == imgui_window_id;
}

// Whether an event is addressed to the debugger window, and so must not also
// reach the emulated machine or the text debugger's command line. Events with
// no window id (SDL_QUIT, joystick, audio) are never the debugger's.
//
// Shared by both event loops rather than copied into each: the running loop in
// video_update() and the paused loop in DEBUGGetCurrentStatus() have to agree
// about which events belong to the debugger, and two copies of this switch
// would be two places to forget an event type.
bool
video_event_targets_debug_ui(const SDL_Event *ev)
{
	if (imgui_window == NULL) {
		return false;
	}
	switch (ev->type) {
		case SDL_WINDOWEVENT:      return ev->window.windowID == imgui_window_id;
		case SDL_KEYDOWN:
		case SDL_KEYUP:            return ev->key.windowID    == imgui_window_id;
		case SDL_TEXTINPUT:        return ev->text.windowID   == imgui_window_id;
		case SDL_TEXTEDITING:      return ev->edit.windowID   == imgui_window_id;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:    return ev->button.windowID == imgui_window_id;
		case SDL_MOUSEMOTION:      return ev->motion.windowID == imgui_window_id;
		case SDL_MOUSEWHEEL:       return ev->wheel.windowID  == imgui_window_id;
		default:                   return false;
	}
}

// Hand an event to the debugger UI. Wrapped here so debugger.c can feed the
// graphical debugger without including its C++ headers -- video.c already owns
// the window and is the only place that knows the UI exists.
void
video_debug_ui_feed_event(const SDL_Event *ev)
{
	if (imgui_window != NULL) {
		debug_ui_process_event(ev);
	}
}

// Apply the VS-style debug shortcuts for a keydown already known to target the
// debugger window. A focused text field (goto/search) suppresses them; a merely
// focused panel does not.
void
video_debug_ui_shortcut_key(const SDL_Event *ev)
{
	if (ev->type == SDL_KEYDOWN && !debug_ui_want_text_input()) {
		video_imgui_debug_key(ev->key.keysym);
	}
}

// Map a keydown on the debugger window to a Visual-Studio-style debug action.
// Shared by the running event loop (video_update) and the paused pump
// (video_debug_ui_pump_paused) so the shortcuts behave identically whether the
// machine is running or halted. The caller has already confirmed the event
// targets the debugger window and that no text field has focus.
//   F5   continue          Ctrl+Shift+F5  restart (reset)
//   F9   toggle bp at PC   Ctrl+F10       run to cursor
//   F10  step over         F11            step into
//   Shift+F11 step out     Break/Pause    pause
// Step/continue are inert while running; pause is inert while halted.
static void
video_imgui_debug_key(SDL_Keysym ks)
{
	const bool shift = (ks.mod & KMOD_SHIFT) != 0;
	const bool ctrl  = (ks.mod & KMOD_CTRL) != 0;
	switch (ks.scancode) {
		case SDL_SCANCODE_F5:
			if (ctrl && shift) {
				input_log_end("machine reset");
				smc_requested_reset = true;      // Ctrl+Shift+F5 = restart
			} else if (DEBUGIsPaused()) {
				DEBUGContinue();                 // F5 = continue
			}
			break;
		case SDL_SCANCODE_F9:
			debug_ui_toggle_breakpoint_at_pc();
			break;
		case SDL_SCANCODE_F10:
			if (ctrl) {
				debug_ui_run_to_cursor();        // Ctrl+F10 = run to cursor
			} else if (DEBUGIsPaused()) {
				DEBUGStepOverAuto(DEBUG_OWNER_UI);
			}
			break;
		case SDL_SCANCODE_F11:
			if (DEBUGIsPaused()) {
				if (shift) {
					DEBUGStepOut(DEBUG_OWNER_UI);
				} else {
					DEBUGStepIntoAuto(DEBUG_OWNER_UI);
				}
			}
			break;
		case SDL_SCANCODE_PAUSE:
			if (DEBUGIsRunning()) {
				DEBUGPause();
			}
			break;
		default:
			break;
	}
}

// Keep the debugger window live and interactive while the machine is halted
// under -imgui. Called from DEBUGGetCurrentStatus() when it owns the pause loop.
// Mirrors the SDL debugger's stopped behaviour -- pump events, re-render,
// throttle -- but drives the ImGui window instead. Returns the
// DEBUGGetCurrentStatus contract:
//   -1 = quit, 1 = stay paused, 0 = a control-bar action resumed/stepped the CPU.
int
video_debug_ui_pump_paused(void)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		// Feed everything to ImGui so its buttons and inputs work while paused.
		debug_ui_process_event(&event);

		if (event.type == SDL_QUIT) {
			return -1;
		}
		if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
			// Closing the debugger window must not kill the emulator; only a
			// close of the main emulator window quits.
			if (imgui_window && event.window.windowID == imgui_window_id) {
				continue;
			}
			return -1;
		}

		if (video_host_key_event(&event)) {
			continue;
		}
		// Only an active text field (goto/search) suppresses the shortcuts; a
		// merely focused panel does not, so F5/F10/F11 work while just viewing.
		if (event.type == SDL_KEYDOWN && imgui_window &&
		    event.key.windowID == imgui_window_id &&
		    !debug_ui_want_text_input()) {
			video_imgui_debug_key(event.key.keysym);
		} else if (event.type == SDL_KEYDOWN && !(imgui_window && event.key.windowID == imgui_window_id) &&
		           ctrl_shortcuts && !(disable_emu_cmd_keys || mouse_grabbed) &&
		           (event.key.keysym.mod & SHORTCUT_MOD) != 0) {
			// -ctrlkeys: the save-state slot keys work while paused too, as
			// with the host key; a load is the usual reason to want one there.
			state_slots_hotkey(event.key.keysym.sym);
		}
	}

	// Re-present the main window's existing frame and rebuild the debugger's.
	// The machine is halted, so no new emulated frame exists -- rendering one
	// would re-blend the activity LED and record a duplicate GIF frame. This is
	// also where the control-bar buttons are evaluated and may change
	// currentMode.
	video_repaint_only();

	// ~60fps; don't spin hot while parked. Waiting on the DAP socket instead
	// of sleeping, when there is one, answers a request to the halted machine
	// as soon as it arrives rather than after the rest of the 16 ms.
	if (!debug_server_wait(16)) {
		SDL_Delay(16);
	}

	// If a control action resumed or single-stepped, tell emulator_loop to
	// execute rather than stay parked. DMODE_STOP is the only mode that keeps
	// us here; run and step both mean "go".
	if (!DEBUGIsPaused()) {
		return 0;
	}
	return 1;
}
#else
bool video_debug_ui_available(void) { return false; }
int  video_debug_ui_pump_paused(void) { return 1; }
bool video_is_debug_ui_window(Uint32 window_id) { (void)window_id; return false; }
bool video_event_targets_debug_ui(const SDL_Event *ev) { (void)ev; return false; }
void video_debug_ui_shortcut_key(const SDL_Event *ev) { (void)ev; }
void video_debug_ui_feed_event(const SDL_Event *ev) { (void)ev; }
#endif

// ─── The host key ────────────────────────────────────────────────────────────
// See video.h and hostkey.h.

static const struct { const char *name; SDL_Scancode sc; } host_key_aliases[] = {
	{ "Scroll Lock", SDL_SCANCODE_SCROLLLOCK },
	{ "Right Cmd", SDL_SCANCODE_RGUI },
	{ "Right Command", SDL_SCANCODE_RGUI },
	{ "Right Win", SDL_SCANCODE_RGUI },
	{ "Menu", SDL_SCANCODE_APPLICATION },
};

bool
video_set_host_key(const char *name, bool from_cli)
{
	if (!name || !name[0])
		return false;
	SDL_Scancode sc = SDL_GetScancodeFromName(name);
	for (size_t i = 0; sc == SDL_SCANCODE_UNKNOWN && i < sizeof host_key_aliases / sizeof host_key_aliases[0]; i++)
		if (SDL_strcasecmp(name, host_key_aliases[i].name) == 0)
			sc = host_key_aliases[i].sc;
	if (sc == SDL_SCANCODE_UNKNOWN)
		return false;
	if (from_cli || !host_key_from_cli)
		hostkey_set((int)sc);
	host_key_from_cli |= from_cli;
	return true;
}

void
video_set_ctrl_shortcuts(bool on, bool from_cli)
{
	if (from_cli || !ctrl_keys_from_cli)
		ctrl_shortcuts = on;
	ctrl_keys_from_cli |= from_cli;
}

const char *
video_host_key_name(void)
{
	switch (hostkey_scancode()) {
		case SDL_SCANCODE_SCROLLLOCK:  return "Scroll Lock";
		case SDL_SCANCODE_APPLICATION: return "Menu";
#ifdef __APPLE__
		case SDL_SCANCODE_RGUI:        return "Right Cmd";
		case SDL_SCANCODE_LGUI:        return "Left Cmd";
#else
		case SDL_SCANCODE_RGUI:        return "Right Win";
		case SDL_SCANCODE_LGUI:        return "Left Win";
#endif
		default:                       return SDL_GetScancodeName((SDL_Scancode)hostkey_scancode());
	}
}

// The title's last part: that the host key is latched, and how to end a
// mouse capture.
static const char *
video_title_status(void)
{
	static char buf[160];
	buf[0] = 0;
	if (hostkey_latched())
		snprintf(buf, sizeof buf, " [host key on: %s or Esc to leave]", video_host_key_name());
	if (mouse_grabbed) {
		size_t n = strlen(buf);
		snprintf(buf + n, sizeof buf - n, " (%s+M to end mouse/keyboard capture)", video_host_key_name());
	}
	return buf;
}

// One emulator command: the host key plus this key, or with -ctrlkeys Ctrl
// (Cmd) plus it. False for a key that is no command.
static bool
video_host_command(SDL_Keysym ks)
{
	switch (ks.sym) {
		case SDLK_s:         machine_dump("user keyboard request"); return true;
		case SDLK_r:         input_log_end("machine reset"); machine_reset(); return true;
		case SDLK_BACKSPACE: machine_nmi(); return true;
		case SDLK_v:         machine_paste(SDL_GetClipboardText(), true); return true;
		case SDLK_f:
		case SDLK_RETURN:
			is_fullscreen = !is_fullscreen;
			SDL_SetWindowFullscreen(window, is_fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
			return true;
		case SDLK_PLUS:
		case SDLK_EQUALS:    machine_toggle_warp(); return true;
		case SDLK_a:         sdcard_attach(); return true;
		case SDLK_d:         sdcard_detach(); return true;
		case SDLK_m:         mousegrab_toggle(); return true;
#ifndef __EMSCRIPTEN__
		case SDLK_p:         screenshot(); return true;
#endif
		default:
			break;
	}
	// The debugger's keys, which this way reach it from the emulator window
	// without the guest seeing them.
	switch (ks.scancode) {
		case SDL_SCANCODE_F5:
		case SDL_SCANCODE_F9:
		case SDL_SCANCODE_F10:
		case SDL_SCANCODE_F11:
		case SDL_SCANCODE_F12:
		case SDL_SCANCODE_PAUSE:
#ifdef HAS_IMGUI
			if (video_debug_ui_available()) {
				if (ks.scancode == SDL_SCANCODE_F12)
					ks.scancode = SDL_SCANCODE_PAUSE;
				video_imgui_debug_key(ks);
				return true;
			}
#endif
			if ((ks.scancode == SDL_SCANCODE_F12 || ks.scancode == SDL_SCANCODE_PAUSE) && debugger_enabled &&
			    DEBUGIsRunning()) {
				DEBUGPause();
				return true;
			}
			return false;
		default:
			break;
	}
	return state_slots_hotkey(ks.sym); // F1 load, F3 save, 0-9 select a save-state slot
}

bool
video_host_key_event(const SDL_Event *ev)
{
	if (ev->type != SDL_KEYDOWN && ev->type != SDL_KEYUP)
		return false;
	const bool     was_latched = hostkey_latched();
	hostkey_action a = hostkey_event(ev->type == SDL_KEYDOWN, (int)ev->key.keysym.scancode, ev->key.repeat != 0);
	if (a == HOSTKEY_COMMAND)
		video_host_command(ev->key.keysym);
	if (hostkey_latched() != was_latched)
		timing_refresh_title();
	return a != HOSTKEY_PASS;
}

bool
video_update()
{
	static bool cmd_down = false;
	static bool alt_down = false;
	bool mouse_changed = false;

	// The debug overlay path historically skipped input handling entirely; the
	// debugger owns the keyboard while it is on screen.
	bool debug_overlay = (debugger_enabled && showDebugOnRender != 0);

	video_render_all();

	// Without the debugger nothing else watches the main window; keep its
	// normal rect current so a maximize just before quitting cannot lose it.
	if (placement_owned_here()) {
		video_window_geometry_poll();
	}

	if (debug_overlay) {
		return true;
	}

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
#ifdef HAS_IMGUI
		// Feed every event to the debugger UI first. If it targets the debugger
		// window, consume it here so it never reaches the emulated machine --
		// otherwise typing in a search box would also type into BASIC.
		if (imgui_window) {
			debug_ui_process_event(&event);
			if (video_event_targets_debug_ui(&event)) {
				// Closing the debugger window must not kill the emulator.
				if (event.type == SDL_WINDOWEVENT &&
				    event.window.event == SDL_WINDOWEVENT_CLOSE) {
					continue;
				}
				if (video_host_key_event(&event)) {
					continue;
				}
				// The VS-style shortcuts also work while the machine is running
				// and the debugger window has focus (F9 to set a breakpoint,
				// Break to pause). Step/continue are inert until paused.
				video_debug_ui_shortcut_key(&event);
				continue;
			}
		}
#endif
		if (event.type == SDL_QUIT) {
			return false;
		}
#ifdef HAS_IMGUI
		// SDL2 only synthesises SDL_QUIT for a window close when that window is
		// the last one open. With the debugger window up there are two, so
		// closing the emulator window delivers a bare SDL_WINDOWEVENT_CLOSE and
		// nothing else -- which used to fall through here and be dropped,
		// leaving the [X] button inert. A close of the debugger window has
		// already been consumed above, so anything reaching this point is a
		// close of the emulator window and does mean quit.
		if (event.type == SDL_WINDOWEVENT &&
		    event.window.event == SDL_WINDOWEVENT_CLOSE) {
			return false;
		}
#endif
		if (ignore_host_input) {
			switch (event.type) {
				case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_TEXTEDITING:
				case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
				case SDL_MOUSEWHEEL:
				case SDL_JOYDEVICEADDED: case SDL_JOYDEVICEREMOVED:
				case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
					continue;
				default:
					break;
			}
		}
		if (video_host_key_event(&event)) {
			continue;
		}
		if (event.type == SDL_KEYDOWN) {
			bool consumed = false;
			// -ctrlkeys: upstream's Ctrl (Cmd) shortcuts, the same commands
			// as the host key's. They take those Ctrl combinations from the
			// guest, which the host key does not, so they are off by default,
			// and a program can turn them off too (-noemucmdkeys, $9FB7).
			if (ctrl_shortcuts && cmd_down) {
				if (event.key.keysym.sym == SDLK_m) {
					consumed = video_host_command(event.key.keysym);
				} else if (!(disable_emu_cmd_keys || mouse_grabbed)) {
					SDL_Keysym ks = event.key.keysym;
					// Only the commands upstream had, and the slot keys: the
					// debugger's F-keys stay plain F-keys here.
					if (!(ks.scancode >= SDL_SCANCODE_F5 && ks.scancode <= SDL_SCANCODE_F12) &&
					    ks.scancode != SDL_SCANCODE_PAUSE) {
						consumed = video_host_command(ks);
					}
				}
			}
			if (alt_down && !disable_emu_cmd_keys) {
				// Capture ALT+F4 key sequence to exit emulator if it is not handled by SDL
				if (event.key.keysym.sym == SDLK_F4) {
					printf("Shutdown requested by ALT+F4\n");
					main_shutdown();
#ifdef __EMSCRIPTEN__
					emscripten_force_exit(0);
#endif
					exit(0);
				}
			}
			if (!consumed) {
				if (event.key.keysym.scancode == LSHORTCUT_KEY || event.key.keysym.scancode == RSHORTCUT_KEY) {
					cmd_down = true;
				}
				if (event.key.keysym.scancode == SDL_SCANCODE_LALT || event.key.keysym.scancode == SDL_SCANCODE_RALT) {
					alt_down = true;
				}
				handle_keyboard(true, event.key.keysym.sym, event.key.keysym.scancode);
			}
			continue;
		}
		if (event.type == SDL_KEYUP) {
			if (event.key.keysym.scancode == LSHORTCUT_KEY || event.key.keysym.scancode == RSHORTCUT_KEY) {
				cmd_down = false;
			}
			if (event.key.keysym.scancode == SDL_SCANCODE_LALT || event.key.keysym.scancode == SDL_SCANCODE_RALT) {
				alt_down = false;
			}
			handle_keyboard(false, event.key.keysym.sym, event.key.keysym.scancode);
			continue;
		}
		if (event.type == SDL_MOUSEBUTTONDOWN) {
			switch (event.button.button) {
				case SDL_BUTTON_LEFT:
					mouse_button_down(0);
					mouse_changed = true;
					break;
				case SDL_BUTTON_RIGHT:
					mouse_button_down(1);
					mouse_changed = true;
					break;
				case SDL_BUTTON_MIDDLE:
					mouse_button_down(2);
					mouse_changed = true;
					break;
			}
		}
		if (event.type == SDL_MOUSEBUTTONUP) {
			switch (event.button.button) {
				case SDL_BUTTON_LEFT:
					mouse_button_up(0);
					mouse_changed = true;
					break;
				case SDL_BUTTON_RIGHT:
					mouse_button_up(1);
					mouse_changed = true;
					break;
				case SDL_BUTTON_MIDDLE:
					mouse_button_up(2);
					mouse_changed = true;
					break;
			}
		}
		if (event.type == SDL_MOUSEMOTION) {
			static int mouse_x;
			static int mouse_y;
			if (mouse_grabbed) {
				mouse_move(event.motion.xrel, event.motion.yrel);
			} else {
				mouse_move(event.motion.x - mouse_x, event.motion.y - mouse_y);
			}
			mouse_x = event.motion.x;
			mouse_y = event.motion.y;
			mouse_changed = true;
		}
		if (event.type == SDL_MOUSEWHEEL) {
			mouse_set_wheel(event.wheel.y);
			mouse_changed = true;
		}

		if (event.type == SDL_JOYDEVICEADDED) {
			joystick_add(event.jdevice.which);
		}
	    if (event.type == SDL_JOYDEVICEREMOVED) {
		    joystick_remove(event.jdevice.which);
	    }
	    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
		    joystick_button_down(event.cbutton.which, event.cbutton.button);
	    }
		if (event.type == SDL_CONTROLLERBUTTONUP) {
		    joystick_button_up(event.cbutton.which, event.cbutton.button);
	    }

	}
	if (mouse_changed) {
		mouse_send_state();
	}
	return true;
}

// ─── Debug view accessors ──────────────────────────────────────────────────

// Report the layer registers and effective layer Y that were used to render a
// given display scanline. Debug views use this to reproduce mid-frame register
// changes (raster splits) instead of assuming one snapshot covers the frame.
bool
video_get_layer_line_state(uint8_t layer, uint16_t line, uint8_t out_regs[7],
                           uint16_t *out_eff_y, bool *out_enabled,
                           uint16_t *out_layer_row)
{
	if (layer >= 2 || line >= SCREEN_HEIGHT || !line_state_valid[line]) {
		return false;
	}
	if (out_regs) {
		memcpy(out_regs, line_reg_layer[line][layer], 7);
	}
	if (out_eff_y) {
		*out_eff_y = line_eff_y[line];
	}
	if (out_enabled) {
		*out_enabled = (line_layer_enabled[line] & (1 << layer)) != 0;
	}
	if (out_layer_row) {
		*out_layer_row = line_layer_row[line][layer];
	}
	return true;
}

uint16_t
video_get_scanline_count(void)
{
	return SCREEN_HEIGHT;
}

uint32_t
video_frame_dot_clocks(void)
{
	const bool ntsc_mode = reg_composer[0] & 2;
	// The counters video_step() compares against: a VGA line is a whole
	// scan width, an NTSC line is a half-scan, and either way a frame is
	// signalled after SCAN_HEIGHT of them.
	return ntsc_mode ? (uint32_t)NTSC_HALF_SCAN_WIDTH * SCAN_HEIGHT
	                 : (uint32_t)VGA_SCAN_WIDTH * SCAN_HEIGHT;
}

uint16_t
video_frame_scanlines(void)
{
	return SCAN_HEIGHT;
}

// Size, in layer pixels, of the image the composer is actually putting on
// screen: the active display window (DC_HSTART/HSTOP, DC_VSTART/VSTOP) scaled
// by DC_HSCALE/DC_VSCALE. The composer advances the layer by scale/128 of a
// pixel per output pixel, so this is what a 320x200-style mode really shows.
// Debug viewers use it to size themselves to the current video mode.
void
video_get_active_layer_size(int *out_w, int *out_h)
{
	int hstart = reg_composer[4] << 2;
	int hstop  = reg_composer[5] << 2;
	int vstart = reg_composer[6] << 1;
	int vstop  = reg_composer[7] << 1;

	if (hstart > SCREEN_WIDTH)  hstart = SCREEN_WIDTH;
	if (hstop  > SCREEN_WIDTH)  hstop  = SCREEN_WIDTH;
	if (vstart > SCREEN_HEIGHT) vstart = SCREEN_HEIGHT;
	if (vstop  > SCREEN_HEIGHT) vstop  = SCREEN_HEIGHT;

	const int screen_w = hstop > hstart ? hstop - hstart : 0;
	const int screen_h = vstop > vstart ? vstop - vstart : 0;

	if (out_w) {
		*out_w = (screen_w * reg_composer[1]) >> 7;
	}
	if (out_h) {
		*out_h = (screen_h * reg_composer[2]) >> 7;
	}
}

// Predict the next VERA interrupt.
//
// VERA raises VSYNC when the scan reaches line SCREEN_HEIGHT and LINE when it
// reaches IRQLINE, so with the current scan position both are predictable. The
// debugger uses this for its "cycles until the next IRQ" readout. Returns false
// when neither a line nor a vsync interrupt is enabled in IEN. out_cycles
// receives CPU cycles (at `mhz`) until the next one; out_source receives the
// ISR bit that will be set (1 = VSYNC, 2 = LINE).
bool
video_next_irq(float mhz, uint32_t *out_cycles, uint8_t *out_source)
{
	const bool ntsc_mode = reg_composer[0] & 2;
	const bool vsync_en  = (ien & 1) != 0;
	const bool line_en   = (ien & 2) != 0;

	if (!vsync_en && !line_en) {
		return false;
	}

	// Scanline the renderer is currently on, in the same space as the compares
	// in update_isr_and_coll().
	const int cur = ntsc_mode ? (int)(ntsc_scan_pos_y % SCAN_HEIGHT) - NTSC_Y_OFFSET_LOW
	                          : (int)vga_scan_pos_y - VGA_Y_OFFSET;

	// Lines from `cur` to `target`, wrapping through the end of the frame.
	#define LINES_UNTIL(target) \
		((((int)(target) - cur) + SCAN_HEIGHT) % SCAN_HEIGHT)

	int     best_lines  = SCAN_HEIGHT + 1;
	uint8_t best_source = 0;

	if (vsync_en) {
		const int d = LINES_UNTIL(SCREEN_HEIGHT);
		if (d < best_lines) {
			best_lines  = d;
			best_source = 1;
		}
	}
	if (line_en) {
		const int d = LINES_UNTIL(irq_line);
		if (d < best_lines) {
			best_lines  = d;
			best_source = 2;
		}
	}
	#undef LINES_UNTIL

	if (!best_source) {
		return false;
	}

	// video_step() advances the scan by PIXEL_FREQ * cycles / mhz pixels, so a
	// whole scanline costs scan_width * mhz / PIXEL_FREQ CPU cycles. Subtract
	// how far into the current line we already are.
	const float scan_width      = ntsc_mode ? NTSC_HALF_SCAN_WIDTH : VGA_SCAN_WIDTH;
	const float cycles_per_line = scan_width * mhz / (float)PIXEL_FREQ;
	const float into_line       = ntsc_mode ? ntsc_half_cnt : vga_scan_pos_x;
	float       cycles          = best_lines * cycles_per_line
	                              - into_line * mhz / (float)PIXEL_FREQ;
	if (cycles < 0) {
		cycles = 0;
	}

	if (out_cycles) {
		*out_cycles = (uint32_t)cycles;
	}
	if (out_source) {
		*out_source = best_source;
	}
	return true;
}

// Raw VERA interrupt state, for debug views: enable mask, pending mask and the
// programmed IRQ line.
void
video_get_irq_state(uint8_t *out_ien, uint8_t *out_isr, uint16_t *out_irq_line)
{
	if (out_ien)
		*out_ien = ien;
	if (out_isr)
		*out_isr = isr | (pcm_is_fifo_almost_empty() ? 8 : 0);
	if (out_irq_line)
		*out_irq_line = irq_line;
}

void
video_end()
{
	if (legacy_debug_ui_initialized) {
		DEBUGFreeUI();
		legacy_debug_ui_initialized = false;
	}

#ifdef HAS_IMGUI
	// Tear the UI down before its renderer and window: the ImGui SDL backends
	// hold both and free renderer-owned textures on shutdown, and the native
	// DX12 path holds descriptors in the main backend's shared heap, which is
	// destroyed further down. Safe no-ops if -imgui was never passed or the
	// window failed to open.
	if (imgui_window && (imgui_renderer || imgui_native_dx12)) {
		debug_ui_shutdown();
		imgui_native_dx12 = false;
	}
	if (imgui_renderer) {
		SDL_DestroyRenderer(imgui_renderer);
		imgui_renderer = NULL;
	}
	if (imgui_window) {
#ifdef _WIN32
		extern void video_win32_remove_move_hook(SDL_Window *window);
		video_win32_remove_move_hook(imgui_window);
#endif
		SDL_DestroyWindow(imgui_window);
		imgui_window = NULL;
	}
#endif

	if (record_gif != RECORD_GIF_DISABLED) {
		GifEnd(&gif_writer);
		record_gif = RECORD_GIF_DISABLED;
	}

	// While still fullscreen if it is: the poll skips a fullscreen window and
	// keeps the windowed placement from before.
	placement_save_main();

	is_fullscreen = false;
	SDL_SetWindowFullscreen(window, 0);
	SDL_DelEventWatch(video_move_resize_watch, NULL);
#ifdef _WIN32
	extern void video_win32_remove_move_hook(SDL_Window *window);
	video_win32_remove_move_hook(window);
#endif
	render_backend_texture_destroy(sdlTexture);
	sdlTexture = NULL;
	render_backend_destroy(renderer);
	renderer = NULL;
	SDL_DestroyWindow(window);
	window = NULL;
}


static const int increments[32] = {
	0,   0,
	1,   -1,
	2,   -2,
	4,   -4,
	8,   -8,
	16,  -16,
	32,  -32,
	64,  -64,
	128, -128,
	256, -256,
	512, -512,
	40,  -40,
	80,  -80,
	160, -160,
	320, -320,
	640, -640,
};

void
video_get_state(struct video_state *out)
{
	memset(out, 0, sizeof *out);
	for (int i = 0; i < 2; i++) {
		struct video_port_state *p = &out->port[i];
		p->address          = io_addr[i] & 0x1ffff;
		p->increment_code   = io_inc[i];
		p->increment        = increments[io_inc[i]];
		p->prefetch         = io_rddata[i];
		p->nibble           = fx_nibble_bit[i];
		p->nibble_increment = fx_nibble_incr[i];
	}
	out->addrsel = io_addrsel;
	out->dcsel   = io_dcsel;
	video_get_irq_state(&out->ien, &out->isr, &out->irq_line);
	out->irq_out  = video_get_irq_out();
	out->ntsc     = reg_composer[0] & 2;
	out->scanline = out->ntsc ? ntsc_scan_pos_y % SCAN_HEIGHT : vga_scan_pos_y;

	out->fx_addr1_mode           = fx_addr1_mode;
	out->fx_4bit_mode            = fx_4bit_mode;
	out->fx_16bit_hop            = fx_16bit_hop;
	out->fx_cache_byte_cycling   = fx_cache_byte_cycling;
	out->fx_cache_fill           = fx_cache_fill;
	out->fx_cache_write          = fx_cache_write;
	out->fx_trans_writes         = fx_trans_writes;
	out->fx_2bit_poly            = fx_2bit_poly;
	out->fx_2bit_poking          = fx_2bit_poking;
	out->fx_cache_increment_mode = fx_cache_increment_mode;
	out->fx_cache_nibble_index   = fx_cache_nibble_index;
	out->fx_multiplier           = fx_multiplier;
	out->fx_subtract             = fx_subtract;
	out->fx_affine_clip          = fx_affine_clip;
	out->fx_cache_byte_index     = fx_cache_byte_index;
	out->fx_16bit_hop_align      = fx_16bit_hop_align;
	out->fx_x_increment          = (int32_t)fx_x_pixel_increment;
	out->fx_y_increment          = (int32_t)fx_y_pixel_increment;
	out->fx_x_position           = fx_x_pixel_position;
	out->fx_y_position           = fx_y_pixel_position;
	out->fx_poly_fill_length     = fx_fill_length();
	out->fx_affine_tile_base     = fx_affine_tile_base;
	out->fx_affine_map_base      = fx_affine_map_base;
	out->fx_affine_map_size      = fx_affine_map_size;
	memcpy(out->fx_cache, fx_cache, sizeof out->fx_cache);
	out->fx_accumulator          = fx_mult_accumulator;
}

uint32_t
video_get_address(uint8_t sel)
{
	uint32_t address = io_addr[sel];
	return address & 0x1ffff;
}

void fx_affine_prefetch(void);

// addr_data.v:872-880, 934-938
static void
fx_poly_addr1(bool fetch)
{
	int32_t x = (int32_t)(fx_x_pixel_position << 5) >> 5;
	if (fx_4bit_mode) {
		io_addr[1] = (io_addr[0] + (x >> 17)) & 0x1ffff;
		fx_nibble_bit[1] = (fx_x_pixel_position >> 16) & 1;
	} else {
		io_addr[1] = (io_addr[0] + (x >> 16)) & 0x1ffff;
		fx_nibble_bit[1] = 0;
	}
	if (fetch) {
		io_rddata[1] = video_space_read(io_addr[1]);
	}
}

// addr_data.v:888-910: requested in any mode, held until mode 2 or 3.
static void
fx_resolve_pending(void)
{
	if (!fx_pending_pos_calc) {
		return;
	}
	if (fx_addr1_mode == 3) {
		fx_pending_pos_calc = false;
		fx_affine_prefetch();
	} else if (fx_addr1_mode == 2) {
		fx_pending_pos_calc = false;
		fx_poly_addr1(true);
	}
}

// mult_accum.v OLOAD: with the multiplier off, accumulate loads the cache.
static void
fx_accum_step(void)
{
	if (fx_multiplier) {
		int32_t m = (int16_t)((fx_cache[1] << 8) | fx_cache[0]) * (int16_t)((fx_cache[3] << 8) | fx_cache[2]);
		if (fx_subtract) {
			fx_mult_accumulator -= m;
		} else {
			fx_mult_accumulator += m;
		}
	} else {
		fx_mult_accumulator = (int32_t)((uint32_t)fx_cache[0] | (uint32_t)fx_cache[1] << 8 |
			(uint32_t)fx_cache[2] << 16 | (uint32_t)fx_cache[3] << 24);
	}
}

// PSG, palette and sprite attributes: addr_data.v:691-697.
static inline bool
fx_special_addr(uint32_t a)
{
	a &= 0x1ffff;
	return (a >> 6) == 0x7E7 || (a >> 9) == 0xFD || (a >> 10) == 0x7F;
}

uint32_t
get_and_inc_address(uint8_t sel, bool write)
{
	uint32_t address = io_addr[sel];
	int16_t incr = increments[io_inc[sel]];

	if (sel == 1 && fx_addr1_mode == 2 && !write && fx_2bit_poking) {
		return address;
	}

	if (fx_4bit_mode && fx_nibble_incr[sel] && !incr) {
		if (fx_nibble_bit[sel]) {
			if ((io_inc[sel] & 1) == 0) io_addr[sel] += 1;
			fx_nibble_bit[sel] = 0;
		} else {
			if (io_inc[sel] & 1) io_addr[sel] -= 1;
			fx_nibble_bit[sel] = 1;
		}
	}

	if (sel == 1 && fx_16bit_hop) {
		if (incr == 4) {
			if (fx_16bit_hop_align == (address & 0x3))
				incr = 1;
			else
				incr = 3;
		} else if (incr == 320) {
			if (fx_16bit_hop_align == (address & 0x3))
				incr = 1;
			else
				incr = 319;
		}
	}

	io_addr[sel] += incr;

	if (sel == 1 && fx_addr1_mode == 1) { // FX line draw mode
		fx_x_pixel_position += fx_x_pixel_increment;
		fx_y_pixel_position += fx_y_pixel_increment;
		if (fx_x_pixel_position & 0x10000) {
			fx_x_pixel_position &= ~0x10000;
			int32_t step0 = increments[io_inc[0]];
			if (fx_4bit_mode && fx_nibble_incr[0] && !step0) {
				bool dec = io_inc[0] & 1;
				// VERA 49: the step follows ADDR1's nibble, and flips it.
				if (fx_nibble_bit[1]) {
					if (!dec) {
						io_addr[1] += 1;
					}
					fx_nibble_bit[1] = 0;
				} else {
					if (dec) {
						io_addr[1] -= 1;
					}
					fx_nibble_bit[1] = 1;
				}
			} else {
				io_addr[1] += step0;
			}
		}
	} else if (fx_addr1_mode == 2 && write == false) { // FX polygon fill mode
		if (!(sel == 1 && fx_2bit_poking)) {
			fx_x_pixel_position += fx_x_pixel_increment;
			fx_y_pixel_position += fx_y_pixel_increment;
			if (sel == 0 && fx_cache_byte_cycling && !fx_cache_fill) {
				if (fx_cache_increment_mode) {
					fx_cache_byte_index = (fx_cache_byte_index & 2) | ((fx_cache_byte_index + 1) & 1);
				} else {
					fx_cache_byte_index = (fx_cache_byte_index + 1) & 3;
				}
			}
			if (sel == 1) {
				fx_poly_addr1(false);
			}
		}
	} else if (sel == 1 && fx_addr1_mode == 3) { // FX affine mode
		fx_x_pixel_position += fx_x_pixel_increment;
		fx_y_pixel_position += fx_y_pixel_increment;
	}
	io_addr[0] &= 0x1ffff;
	io_addr[1] &= 0x1ffff;
	return address;
}

void
fx_affine_prefetch(void)
{
	if (fx_addr1_mode != 3) return; // only if affine mode is selected

	uint32_t address;
	uint8_t affine_x_tile = (fx_x_pixel_position >> 19) & 0xff;
	uint8_t affine_y_tile = (fx_y_pixel_position >> 19) & 0xff;
	uint8_t affine_x_sub_tile = (fx_x_pixel_position >> 16) & 0x07;
	uint8_t affine_y_sub_tile = (fx_y_pixel_position >> 16) & 0x07;

	if (!fx_affine_clip) { // wrap
		affine_x_tile &= fx_affine_map_size - 1;
		affine_y_tile &= fx_affine_map_size - 1;
	}

	if (affine_x_tile >= fx_affine_map_size || affine_y_tile >= fx_affine_map_size) {
		// We clipped, return value for tile 0
		address = fx_affine_tile_base + (affine_y_sub_tile << (3 - fx_4bit_mode)) + (affine_x_sub_tile >> (uint8_t)fx_4bit_mode);
		fx_nibble_bit[1] = (affine_x_sub_tile & 1) >> (1 - fx_4bit_mode);
	} else {
		// Get the address within the tile map
		address = fx_affine_map_base + (affine_y_tile * fx_affine_map_size) + affine_x_tile;
		// Now translate that to the tile base address
		uint8_t affine_tile_idx = video_space_read(address);
		address = fx_affine_tile_base + (affine_tile_idx << (6 - fx_4bit_mode));
		// Now add the sub-tile address
		address += (affine_y_sub_tile << (3 - fx_4bit_mode)) + (affine_x_sub_tile >> (uint8_t)fx_4bit_mode);
		fx_nibble_bit[1] = (affine_x_sub_tile & 1) >> (1 - fx_4bit_mode);
	}
	io_addr[1] = address & 0x1ffff;
	io_rddata[1] = video_space_read(io_addr[1]);
}

//
// Vera: Internal Video Address Space
//

uint8_t
video_space_read(uint32_t address)
{
	return video_ram[address & 0x1FFFF];
}

static void
video_space_read_range(uint8_t* dest, uint32_t address, uint32_t size)
{
	if (address >= ADDR_VRAM_START && (address+size) <= ADDR_VRAM_END) {
		memcpy(dest, &video_ram[address], size);
	} else {
		for(int i = 0; i < size; ++i) {
			*dest++ = video_space_read(address + i);
		}
	}
}

void
video_space_write(uint32_t address, uint8_t value)
{
	video_ram[address & 0x1FFFF] = value;

	if (address >= ADDR_PSG_START && address < ADDR_PSG_END) {
		audio_render();
		psg_writereg(address & 0x3f, value);
	} else if (address >= ADDR_PALETTE_START && address < ADDR_PALETTE_END) {
		palette[address & 0x1ff] = value;
		video_palette.dirty = true;
	} else if (address >= ADDR_SPRDATA_START && address < ADDR_SPRDATA_END) {
		sprite_attr_write(address, value);
	}
}


void
fx_video_space_write(uint32_t address, bool nibble, uint8_t value)
{
	bool stored = false;
	if (fx_4bit_mode) {
		if (nibble) {
			if (!fx_trans_writes || (value & 0x0f) > 0) {
				video_ram[address & 0x1FFFF] = (video_ram[address & 0x1FFFF] & 0xf0) | (value & 0x0f);
				stored = true;
			}
		} else {
			if (!fx_trans_writes || (value & 0xf0) > 0) {
				video_ram[address & 0x1FFFF] = (video_ram[address & 0x1FFFF] & 0x0f) | (value & 0xf0);
				stored = true;
			}
		}
	} else {
		if (!fx_trans_writes || value > 0) {
			video_ram[address & 0x1FFFF] = value;
			stored = true;
		}
	}
	bool side_effect = true;
	if (address >= ADDR_PSG_START && address < ADDR_PSG_END) {
		audio_render();
		psg_writereg(address & 0x3f, value);
	} else if (address >= ADDR_PALETTE_START && address < ADDR_PALETTE_END) {
		palette[address & 0x1ff] = value;
		video_palette.dirty = true;
	} else if (address >= ADDR_SPRDATA_START && address < ADDR_SPRDATA_END) {
		sprite_attr_write(address, value);
	} else {
		side_effect = false;
	}
	// A transparent write skips VRAM but still reaches the PSG, palette and
	// sprite registers, so it is reported there with the value they received.
	if (stored)
		note_vram_watch(address, video_ram[address & 0x1FFFF]);
	else if (side_effect)
		note_vram_watch(address, value);
}

void
fx_vram_cache_write(uint32_t address, uint8_t value, uint8_t mask)
{
	if (!fx_trans_writes || value > 0) {
		switch (mask) {
			case 0:
				video_ram[address & 0x1FFFF] = value;
				break;
			case 1:
				video_ram[address & 0x1FFFF] = (video_ram[address & 0x1FFFF] & 0x0f) | (value & 0xf0);
				break;
			case 2:
				video_ram[address & 0x1FFFF] = (video_ram[address & 0x1FFFF] & 0xf0) | (value & 0x0f);
				break;
			case 3:
				// Do nothing
				break;
		}
		if (mask != 3)
			note_vram_watch(address, video_ram[address & 0x1FFFF]);
	}
}

void
video_debug_get_palette(uint8_t out[512])
{
	memcpy(out, palette, sizeof palette);
}

void
video_debug_get_sprite_attr(int sprite, uint8_t out[8])
{
	memcpy(out, sprite_data[sprite & 0x7f], sizeof sprite_data[0]);
}

uint32_t video_get_fx_accum() {
	return fx_mult_accumulator;
}
uint8_t video_get_dc_value(uint8_t reg) {
	switch (reg & 0x1F) {
		case 0x00:
		case 0x01:
		case 0x02:
		case 0x03:
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0c:
		case 0x0d:
		case 0x0e:
		case 0x0f:
			return reg_composer[reg];
			break;
		case 0x0b:
			return reg_composer[reg] & 0x3f;
			break;
		case 0x10: // DCSEL=4, $9F29
			return (fx_x_pixel_position >> 16) & 0xff;
			break;
		case 0x11: // DCSEL=4, $9F2A
			return ((fx_x_pixel_position >> 24) & 0x07) | (fx_x_pixel_position & 0x80);
			break;
		case 0x12: // DCSEL=4, $9F2B
			return (fx_y_pixel_position >> 16) & 0xff;
			break;
		case 0x13: // DCSEL=4, $9F2C
			return ((fx_y_pixel_position >> 24) & 0x07) | (fx_y_pixel_position & 0x80);
			break;
		case 0x14: // DCSEL=5, $9F29
			return (fx_x_pixel_position >> 8) & 0xff;
			break;
		case 0x15: // DCSEL=4, $9F2A
			return (fx_y_pixel_position >> 8) & 0xff;
			break;
		case 0x16:
		case 0x17: {
			uint32_t xp = (fx_x_pixel_position >> 7) & 0xfffff, yp = (fx_y_pixel_position >> 7) & 0xfffff;
			uint32_t fill = (((yp >> 9) & 0x3ff) - ((xp >> 9) & 0x3ff)) & 0x3ff;
			bool ovf = (fill >> 8) == 3;
			bool m2 = fx_addr1_mode == 2 && fx_4bit_mode && fx_2bit_poly;
			if ((reg & 0x1f) == 0x17) return (uint8_t)((fill >> 3) << 1);
			uint8_t l = m2 && ((xp >> 8) & 1);
			if (!ovf) l |= (fill & 7) << 1;
			if (!ovf && ((!fx_4bit_mode && ((fill >> 3) & 1)) || (fx_4bit_mode && ((xp >> 11) & 1)))) l |= 0x10;
			if (!ovf) l |= ((xp >> 9) & 3) << 5;
			if ((!fx_4bit_mode && (fill >> 4)) || (!m2 && fx_4bit_mode && (fill >> 3)) || (m2 && ((yp >> 8) & 1))) l |= 0x80;
			return l;
		}
		case 0x18: // DCSEL=6, 0x9F29
			return fx_cache[0];
			break;
		case 0x19: // DCSEL=6, 0x9F2A
			return fx_cache[1];
			break;
		case 0x1a: // DCSEL=6, 0x9F2B
			return fx_cache[2];
			break;
		case 0x1b: // DCSEL=6, 0x9F2C
			return fx_cache[3];
			break;
		default:
			break;
	}

	return vera_version_string[reg % 4];
}

//
// Vera: 6502 I/O Interface
//
// if debugOn, read without any side effects (registers & memory unchanged)

static void check_not_readonly(uint8_t reg) {
	bool wrong = false;
	switch(io_dcsel) {
		case 5: {
			switch(reg) {
				case 0x0b:		// DCSEL=5 FX_POLY_FILL_L
				case 0x0c:		// DCSEL=5 FX_POLY_FILL_H
					wrong=true;
					break;
			}
			break;
		}
		case 63: {
			switch(reg) {
				case 0x09:		// DCSEL=63 DC_VER0
				case 0x0a:		// DCSEL=63 DC_VER1
				case 0x0b:		// DCSEL=63 DC_VER2
				case 0x0c:		// DCSEL=63 DC_VER3
					wrong=true;
					break;
			}
			break;
		}
	}

	if(wrong)
		printf("Warning: %04X wrote to read-only VERA register at 9F%02X (DCSEL=%d)\n", opcode_addr, reg+0x20, io_dcsel);
}

static void check_not_writeonly(uint8_t reg) {
	bool wrong = false;
	switch(io_dcsel) {
		case 2: {
			switch(reg) {
				case 0x0a:		// DCSEL=2 FX_TILEBASE
				case 0x0b:		// DCSEL=2 FX_MAPBASE
				case 0x0c:		// DCSEL=2 FX_MULT
					wrong=true;
					break;
			}
			break;
		}
		case 3:
		case 4: {
			switch(reg) {
				case 0x09:		// DCSEL=3/4 FX_X_INCR_L/FX_X_POS_L
				case 0x0a:		// DCSEL=3/4 FX_X_INCR_H/FX_X_POS_H
				case 0x0b:		// DCSEL=3/4 FX_Y_INCR_L/FX_Y_POS_L
				case 0x0c:		// DCSEL=3/4 FX_Y_INCR_H/FX_Y_POS_H
					wrong=true;
					break;
			}
			break;
		}
		case 5: {
			switch(reg) {
				case 0x09:		// DCSEL=5 FX_X_POS_S
				case 0x0a:		// DCSEL=5 FX_Y_POS_S
					wrong=true;
					break;
			}
			break;
		}
		case 6: {
			switch(reg) {
				case 0x0b:		// DCSEL=6 FX_CACHE_H
				case 0x0c:		// DCSEL=6 FX_CACHE_U
					wrong=true;
					break;
			}
			break;
		}
	}

	if(wrong)
		printf("Warning: %04X read from write-only VERA register at 9F%02X (DCSEL=%d)\n", opcode_addr, reg+0x20, io_dcsel);
}

static uint8_t video_read_inner(uint8_t reg, bool debugOn) {
	bool ntsc_mode = reg_composer[0] & 2;
	uint16_t scanline = ntsc_mode ? ntsc_scan_pos_y % SCAN_HEIGHT : comp_scanline(vga_scan_pos_y);
	if (scanline >= 512) scanline=511;

	// A debug read is the debugger looking, not the program reading: it must
	// neither warn about the guest's behaviour nor change any state. The rest
	// of this function already honours debugOn for the data ports; SPI did not,
	// and the warning fired for the debugger's own reads.
	if (!debugOn) {
		check_not_writeonly(reg);
	}

	switch (reg & 0x1F) {
		case 0x00: return io_addr[io_addrsel] & 0xff;
		case 0x01: return (io_addr[io_addrsel] >> 8) & 0xff;
		case 0x02: return (io_addr[io_addrsel] >> 16) | (fx_nibble_bit[io_addrsel] << 1) | (fx_nibble_incr[io_addrsel] << 2) | (io_inc[io_addrsel] << 3);
		case 0x03:
		case 0x04: {
			if (debugOn) {
				return io_rddata[reg - 3];
			}

			bool addr_nibble = fx_nibble_bit[reg - 3];
			uint32_t address = get_and_inc_address(reg - 3, false);

			uint8_t value = io_rddata[reg - 3];

			// One byte off the data port, charged to VERA's bus. The debugOn
			// return above means the debugger's own reads never reach here,
			// which matters: opening a memory view must not change the number
			// the developer is trying to read.
			note_port_traffic(false, 1);

			if (reg == 4 && fx_addr1_mode == 3) {
				// Affine mode translates through the tile map first, so this
				// read costs a second fetch the guest never asked for.
				note_port_fx_affine();
				fx_affine_prefetch();
			} else
				io_rddata[reg - 3] = video_space_read(io_addr[reg - 3]);

			if (fx_cache_fill) {
				if (fx_4bit_mode) {
					uint8_t nibble_read = (addr_nibble ? ((value & 0x0f) << 4) : (value & 0xf0));

					if (fx_cache_nibble_index) {
						fx_cache[fx_cache_byte_index] = (fx_cache[fx_cache_byte_index] & 0xf0) | (nibble_read >> 4);
						fx_cache_nibble_index = 0;
						fx_cache_byte_index = ((fx_cache_byte_index + 1) & 0x3);
					} else {
						fx_cache[fx_cache_byte_index] = (fx_cache[fx_cache_byte_index] & 0x0f) | (nibble_read);
						fx_cache_nibble_index = 1;
					}
				} else {
					fx_cache[fx_cache_byte_index] = value;
					if (fx_cache_increment_mode)
						fx_cache_byte_index = (fx_cache_byte_index & 0x2) | ((fx_cache_byte_index + 1) & 0x1);
					else
						fx_cache_byte_index = ((fx_cache_byte_index + 1) & 0x3);
				}
			}

			if (log_video) {
				printf("READ  video_space[$%X] = $%02X\n", address, value);
			}
			return value;
		}
		case 0x05: return (io_dcsel << 1) | io_addrsel;
		case 0x06: return ((irq_line & 0x100) >> 1) | ((scanline & 0x100) >> 2) | (ien & 0xF);
		case 0x07: return isr | (pcm_is_fifo_almost_empty() ? 8 : 0);
		case 0x08: return scanline & 0xFF;

		case 0x09:
		case 0x0A:
		case 0x0B:
		case 0x0C: {
			int i = reg - 0x09 + (io_dcsel << 2);
			switch (i) {
				case 0x00:
				case 0x01:
				case 0x02:
				case 0x03:
				case 0x04:
				case 0x05:
				case 0x06:
				case 0x07:
				case 0x08:
				case 0x16: // DCSEL=5, 0x9F2B
				case 0x17: // DCSEL=5, 0x9F2C
					return video_get_dc_value(i);
					break;
				case 0x18: // DCSEL=6, 0x9F29
					if (!debugOn) fx_mult_accumulator = 0;
					// fall out of the switch
					break;
				case 0x19: // DCSEL=6, 0x9F2A
					if (!debugOn) fx_accum_step();
					// fall out of the switch
					break;
				default:
					// The rest of the space is write-only,
					// so reading the values out instead returns the version string.
					// fall out of the switch
					break;
			}
			return vera_version_string[i % 4];
			break;
		}
		case 0x0D:
		case 0x0E:
		case 0x0F:
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13: return reg_layer[0][reg - 0x0D];

		case 0x14:
		case 0x15:
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x1A: return reg_layer[1][reg - 0x14];

		case 0x1B:
			// Same rule as the YM status read: a debug read must not flush
			// audio, so the debugger sees the control byte as of the last
			// render rather than advancing the render position itself.
			if (!debugOn) {
				audio_render();
			}
			return pcm_read_ctrl();
		case 0x1C: return pcm_read_rate();
		case 0x1D: return 0;

		case 0x1E:
		case 0x1F:
			// Reading $9F3E is not passive: with autotx armed and SS asserted
			// it clocks another byte out to the SD card. A debug read must not,
			// or simply having the debugger's memory view open steals bytes
			// from the block the KERNAL is in the middle of reading.
			if (debugOn) {
				return vera_spi_peek(reg & 1);
			}
			return vera_spi_read(reg & 1);
	}
	return 0;
}

static void video_write_inner(uint8_t reg, uint8_t value) {
	// if (reg > 4) {
	// 	printf("ioregisters[0x%02X] = 0x%02X\n", reg, value);
	// }
	//	printf("ioregisters[%d] = $%02X\n", reg, value);

	check_not_readonly(reg);

	switch (reg & 0x1F) {
		case 0x00:
			if (fx_16bit_hop && io_addrsel == 1)
				fx_16bit_hop_align = value & 3;
			if (fx_2bit_poly && fx_4bit_mode && fx_addr1_mode == 2 && io_addrsel == 1) {
				fx_2bit_poking = true;
				io_addr[1] = (io_addr[1] & 0x1fffc) | (value & 0x3);
			} else {
				io_addr[io_addrsel] = (io_addr[io_addrsel] & 0x1ff00) | value;
				if (fx_16bit_hop && io_addrsel == 1)
					fx_16bit_hop_align = value & 3;
			}
			io_rddata[io_addrsel] = video_space_read(io_addr[io_addrsel]);
			break;
		case 0x01:
			io_addr[io_addrsel] = (io_addr[io_addrsel] & 0x100ff) | (value << 8);
			io_rddata[io_addrsel] = video_space_read(io_addr[io_addrsel]);
			break;
		case 0x02:
			io_addr[io_addrsel] = (io_addr[io_addrsel] & 0x0ffff) | ((value & 0x1) << 16);
			fx_nibble_bit[io_addrsel] = (value >> 1) & 0x1;
			fx_nibble_incr[io_addrsel] = (value >> 2) & 0x1;
			io_inc[io_addrsel]  = value >> 3;
			io_rddata[io_addrsel] = video_space_read(io_addr[io_addrsel]);
			break;
		case 0x03:
		case 0x04: {
			if (fx_2bit_poking && fx_addr1_mode == 2 && reg == 4) {
				fx_2bit_poking = false;
				// Still a guest byte through the port reaching VRAM, even
				// though it takes its own path out of this switch below.
				note_port_traffic(true, 1);
				uint8_t mask = value >> 6;
				switch (mask) {
					case 0x00:
						video_ram[io_addr[1] & 0x1FFFF] = (fx_cache[fx_cache_byte_index] & 0xc0) | (io_rddata[1] & 0x3f);
						break;
					case 0x01:
						video_ram[io_addr[1] & 0x1FFFF] = (fx_cache[fx_cache_byte_index] & 0x30) | (io_rddata[1] & 0xcf);
						break;
					case 0x02:
						video_ram[io_addr[1] & 0x1FFFF] = (fx_cache[fx_cache_byte_index] & 0x0c) | (io_rddata[1] & 0xf3);
						break;
					case 0x03:
						video_ram[io_addr[1] & 0x1FFFF] = (fx_cache[fx_cache_byte_index] & 0x03) | (io_rddata[1] & 0xfc);
						break;
				}
				// The poke is an addr1 mode, so it always goes out on port 1.
				vram_watch_port = 1;
				note_vram_watch(io_addr[1], video_ram[io_addr[1] & 0x1FFFF]);
				break; // break out of the enclosing switch statement early, too
			}

			if (enable_midline)
				video_step(MHZ, 0, true); // potential midline raster effect
			bool nibble = fx_nibble_bit[reg - 3];
			uint32_t address = get_and_inc_address(reg - 3, true);
			vram_watch_port = reg - 3;
			if (log_video) {
				printf("WRITE video_space[$%X] = $%02X\n", address, value);
			}

			bool special = fx_special_addr(address);
			if (address >= ADDR_PALETTE_START && address < ADDR_PALETTE_END) {
				// The palette is read as each pixel leaves.
				scan_out_to(write_dot());
			}
			uint8_t wrdata_to_use;
			uint8_t ram_wrdata[4];
			uint8_t nibble_mask[4];
			uint8_t cache_to_use[4];

			if (fx_multiplier) {
				int32_t m_result = (int16_t)((fx_cache[1] << 8) | fx_cache[0]) * (int16_t)((fx_cache[3] << 8) | fx_cache[2]);
				if (fx_subtract)
					m_result = fx_mult_accumulator - m_result;
				else
					m_result = fx_mult_accumulator + m_result;
				cache_to_use[0] = (m_result) & 0xff;
				cache_to_use[1] = (m_result >> 8) & 0xff;
				cache_to_use[2] = (m_result >> 16) & 0xff;
				cache_to_use[3] = (m_result >> 24) & 0xff;
			} else {
				memcpy(cache_to_use, fx_cache, sizeof(fx_cache));
			}

			if (fx_cache_byte_cycling && !special) {
				wrdata_to_use = fx_cache[fx_cache_byte_index];
			} else {
				wrdata_to_use = value;
			}

			if (fx_cache_write && !fx_cache_byte_cycling && !special) {
				ram_wrdata[0] = cache_to_use[0];
				ram_wrdata[1] = cache_to_use[1];
				ram_wrdata[2] = cache_to_use[2];
				ram_wrdata[3] = cache_to_use[3];
			} else {
				ram_wrdata[0] = wrdata_to_use;
				ram_wrdata[1] = wrdata_to_use;
				ram_wrdata[2] = wrdata_to_use;
				ram_wrdata[3] = wrdata_to_use;
			}

			if (fx_cache_write && !special) {
				address &= 0x1fffc;
				// One store, four bytes into VRAM. This is the whole reason FX
				// is worth charging separately: the CPU issued a single write
				// and the bus moved four times as much as an ordinary one.
				note_port_traffic(true, 4);
				note_port_fx_cache_write();
				note_vram_write(address, ram_wrdata[0], true);
				if (fx_trans_writes) {
					if (fx_4bit_mode) {
						nibble_mask[0] = (((ram_wrdata[0] & 0xf0) == 0) << 1) | ((ram_wrdata[0] & 0x0f) == 0);
						nibble_mask[1] = (((ram_wrdata[1] & 0xf0) == 0) << 1) | ((ram_wrdata[1] & 0x0f) == 0);
						nibble_mask[2] = (((ram_wrdata[2] & 0xf0) == 0) << 1) | ((ram_wrdata[2] & 0x0f) == 0);
						nibble_mask[3] = (((ram_wrdata[3] & 0xf0) == 0) << 1) | ((ram_wrdata[3] & 0x0f) == 0);
					} else {
						nibble_mask[0] = (ram_wrdata[0] != 0) ? 0 : 3;
						nibble_mask[1] = (ram_wrdata[1] != 0) ? 0 : 3;
						nibble_mask[2] = (ram_wrdata[2] != 0) ? 0 : 3;
						nibble_mask[3] = (ram_wrdata[3] != 0) ? 0 : 3;
					}
				} else {
					nibble_mask[0] = value & 0x3;
					nibble_mask[1] = (value >> 2) & 0x3;
					nibble_mask[2] = (value >> 4) & 0x3;
					nibble_mask[3] = (value >> 6) & 0x3;
				}

				fx_vram_cache_write(address+0, ram_wrdata[0], nibble_mask[0]);
				fx_vram_cache_write(address+1, ram_wrdata[1], nibble_mask[1]);
				fx_vram_cache_write(address+2, ram_wrdata[2], nibble_mask[2]);
				fx_vram_cache_write(address+3, ram_wrdata[3], nibble_mask[3]);
			} else {
				note_port_traffic(true, 1);
				note_vram_write(address, wrdata_to_use, false);
				if (special) {
					video_space_write(address, value);
					note_vram_watch(address, value);
				} else {
					fx_video_space_write(address, nibble, wrdata_to_use);
				}
			}

			if (reg == 4 && fx_addr1_mode == 3) fx_affine_prefetch();
			else io_rddata[reg - 3] = video_space_read(io_addr[reg - 3]);
			break;
		}
		case 0x05:
			if (value & 0x80) {
				video_reset();
			}
			io_dcsel = (value >> 1) & 0x3f;
			io_addrsel = value & 1;
			break;
		case 0x06:
			irq_line = (irq_line & 0xFF) | ((value >> 7) << 8);
			ien = value & 0xF;
			break;
		case 0x07:
			// top.v:439-444: a write clears only the three latched bits.
			isr &= ~(value & 0x07);
			break;
		case 0x08:
			irq_line = (irq_line & 0x100) | value;
			break;

		case 0x09:
		case 0x0A:
		case 0x0B:
		case 0x0C: {
			int i = reg - 0x09 + (io_dcsel << 2);
			if (i < 8) {
				// composer.v:189-201: the border and window are applied a
				// clock before the pixel leaves; composer.v:172-187 steps the
				// scaled X counter a clock later.
				scan_out_to(write_dot() - (i == 1 ? 0 : 1));
			}
			if (i == 0) {
				// if progressive mode field goes from 0 to 1
				// or if mode goes from vga to something else with
				// progressive mode on, clear the framebuffer
				if (((reg_composer[0] & 0x8) == 0 && (value & 0x8)) ||
					((reg_composer[0] & 0x3) == 1 && (value & 0x3) > 1 && (value & 0x8))) {
					memset(framebuffer, 0x00, SCREEN_WIDTH * SCREEN_HEIGHT * 4);
					// The picture is gone, so the per-scanline history no
					// longer describes anything on screen. Dropped with it,
					// or a debugger paused right after the mode change would
					// decode rows for a blank framebuffer.
					memset(line_state_valid, 0, sizeof(line_state_valid));
				}

				// interlace field bit is read-only
				reg_composer[0] = (reg_composer[0] & ~0x7f) | (value & 0x7f);
				video_palette.dirty = true;
			} else {
				reg_composer[i] = value;
			}
			switch (i) {
				case 0: fetch_note(FL_DC_VIDEO, reg_composer[0]); break;
				case 2: fetch_note(FL_VSCALE, value); break;
				case 6: fetch_note(FL_VSTART, value); break;
				case 7: fetch_note(FL_VSTOP, value); break;
			}

			switch (i) {
				case 0x08: // DCSEL=2, $9F29
					fx_addr1_mode = value & 0x03;
					fx_4bit_mode = (value & 0x04) >> 2;
					fx_16bit_hop = (value & 0x08) >> 3;
					fx_cache_byte_cycling = (value & 0x10) >> 4;
					fx_cache_fill = (value & 0x20) >> 5;
					fx_cache_write = (value & 0x40) >> 6;
					fx_trans_writes = (value & 0x80) >> 7;
					break;
				case 0x09: // DCSEL=2, $9F2A
					fx_affine_tile_base = (value & 0xfc) << 9;
					fx_affine_clip = (value & 0x02) >> 1;
					fx_2bit_poly = (value & 0x01);
					break;
				case 0x0a: // DCSEL=2, $9F2B
					fx_affine_map_base = (value & 0xfc) << 9;
					fx_affine_map_size = 2 << ((value & 0x03) << 1);
					break;
				case 0x0b: // DCSEL=2, $9F2C
					fx_cache_increment_mode = value & 0x01;
					fx_cache_nibble_index = (value & 0x02) >> 1;
					fx_cache_byte_index = (value & 0x0c) >> 2;
					fx_multiplier = (value & 0x10) >> 4;
					fx_subtract = (value & 0x20) >> 5;
					if (value & 0x40) fx_accum_step();
					if (value & 0x80) { // reset accumulator
						fx_mult_accumulator = 0;
					}
					break;
				case 0x0c: // DCSEL=3, $9F29
					fx_x_pixel_increment = ((((reg_composer[0x0d] & 0x7f) << 15) + (reg_composer[0x0c] << 7)) // base value
						| ((reg_composer[0x0d] & 0x40) ? 0xffc00000 : 0)) // sign extend if negative
						<< 5*(!!(reg_composer[0x0d] & 0x80)); // multiply by 32 if flag set
					break;
				case 0x0d: // DCSEL=3, $9F2A
					fx_x_pixel_increment = ((((reg_composer[0x0d] & 0x7f) << 15) + (reg_composer[0x0c] << 7)) // base value
						| ((reg_composer[0x0d] & 0x40) ? 0xffc00000 : 0)) // sign extend if negative
						<< 5*(!!(reg_composer[0x0d] & 0x80)); // multiply by 32 if flag set
					if (fx_addr1_mode == 1 || fx_addr1_mode == 2) {
						// Reset subpixel to 0.5
						if (fx_addr1_mode == 1) fx_x_pixel_position &= ~0x10000;
						fx_x_pixel_position = (fx_x_pixel_position & 0x07ff0000) | 0x00008000;
					}
					break;
				case 0x0e: // DCSEL=3, $9F2B
					fx_y_pixel_increment = ((((reg_composer[0x0f] & 0x7f) << 15) + (reg_composer[0x0e] << 7)) // base value
						| ((reg_composer[0x0f] & 0x40) ? 0xffc00000 : 0)) // sign extend if negative
						<< 5*(!!(reg_composer[0x0f] & 0x80)); // multiply by 32 if flag set
					break;
				case 0x0f: // DCSEL=3, $9F2C
					fx_y_pixel_increment = ((((reg_composer[0x0f] & 0x7f) << 15) + (reg_composer[0x0e] << 7)) // base value
						| ((reg_composer[0x0f] & 0x40) ? 0xffc00000 : 0)) // sign extend if negative
						<< 5*(!!(reg_composer[0x0f] & 0x80)); // multiply by 32 if flag set
					if (fx_addr1_mode == 1 || fx_addr1_mode == 2) {
						// Reset subpixel to 0.5
						fx_y_pixel_position = (fx_y_pixel_position & 0x07ff0000) | 0x00008000;
					}
					break;
				case 0x10: // DCSEL=4, $9F29
					fx_x_pixel_position = (fx_x_pixel_position & 0x0700ff80) | (value << 16);
					fx_pending_pos_calc = true;
					break;
				case 0x11: // DCSEL=4, $9F2A
					fx_x_pixel_position = (fx_x_pixel_position & 0x00ffff00) | ((value & 0x7) << 24) | (value & 0x80);
					fx_pending_pos_calc = true;
					break;
				case 0x12: // DCSEL=4, $9F2B
					fx_y_pixel_position = (fx_y_pixel_position & 0x0700ff80) | (value << 16);
					fx_pending_pos_calc = true;
					break;
				case 0x13: // DCSEL=4, $9F2C
					fx_y_pixel_position = (fx_y_pixel_position & 0x00ffff00) | ((value & 0x7) << 24) | (value & 0x80);
					fx_pending_pos_calc = true;
					break;
				case 0x14: // DCSEL=5, $9F29
					fx_x_pixel_position = (fx_x_pixel_position & 0x07ff0080) | (value << 8);
					break;
				case 0x15: // DCSEL=5, $9F2A
					fx_y_pixel_position = (fx_y_pixel_position & 0x07ff0080) | (value << 8);
					break;
				case 0x18: // DCSEL=6, $9F29
					fx_cache[0] = value;
					break;
				case 0x19: // DCSEL=6, $9F2A
					fx_cache[1] = value;
					break;
				case 0x1a: // DCSEL=6, $9F2B
					fx_cache[2] = value;
					break;
				case 0x1b: // DCSEL=6, $9F2C
					fx_cache[3] = value;
					break;
			}
			break;
		}

		case 0x0D:
		case 0x0E:
		case 0x0F:
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
			// top.v:393-401: scroll high bytes hold four bits.
			reg_layer[0][reg - 0x0D] = (reg == 0x11 || reg == 0x13) ? (value & 0x0f) : value;
			fetch_note(reg - 0x0D, reg_layer[0][reg - 0x0D]);
			refresh_layer_properties(0);
			break;

		case 0x14:
		case 0x15:
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x1A:
			reg_layer[1][reg - 0x14] = (reg == 0x18 || reg == 0x1A) ? (value & 0x0f) : value;
			fetch_note(7 + reg - 0x14, reg_layer[1][reg - 0x14]);
			refresh_layer_properties(1);
			break;

		case 0x1B: audio_render(); pcm_write_ctrl(value); break;
		case 0x1C: audio_render(); pcm_write_rate(value); break;
		case 0x1D:
			audio_render();
			if (audio_events_on && !debug_port_write && pcm_is_fifo_empty()) {
				audio_events_pcm_start(); // x16/step "audio"
			}
			pcm_write_fifo(value);
			break;

		case 0x1E:
		case 0x1F:
			vera_spi_write(reg & 1, value);
			break;
	}
}

void
video_update_title(const char* window_title)
{
	SDL_SetWindowTitle(window, window_title);
}

bool video_is_tilemap_address(int addr)
{
	for (int l = 0; l < 2; ++l) {
		struct video_layer_properties *props = &layer_properties[l];
		if (addr < props->map_base) {
			continue;
		}
		if (addr >= props->map_base + (2 << (props->mapw_log2 + props->maph_log2))) {
			continue;
		}

		return true;
	}
	return false;
}

bool video_is_tiledata_address(int addr)
{
	for (int l = 0; l < 2; ++l) {
		struct video_layer_properties *props = &layer_properties[l];
		if (addr < props->tile_base) {
			continue;
		}
		int tile_size = props->tilew * props->tileh * props->bits_per_pixel / 8;
		if (addr >= props->tile_base + tile_size * (props->bits_per_pixel == 1 ? 256 : 1024)) {
			continue;
		}

		return true;
	}
	return false;
}

bool video_is_special_address(int addr)
{
	return addr >= 0x1F9C0;
}

void
stop6502(uint16_t address, uint8_t bank) {
	// Testbench first: it is headless and scripted, so an STP there must report
	// the result on stdout, not stop for a human. Putting the debugger check
	// ahead of it made -testbench -imgui park invisibly with nothing to resume
	// it and no "STP" line for the harness to read.
	if (testbench) {
		printf("STP\n");
		fflush(stdout);
	} else if (!DEBUGBreakOnStp(address, bank, debugger_enabled || imgui_debugger_enabled)) {
		// No debugger wants it. One that is up must never land here: this modal
		// box blocks inside instruction dispatch, and its "Reset Machine" button
		// resets the machine mid-instruction.
		int return_btn;
		char error_message[80];
		const SDL_MessageBoxButtonData btns[2] = {
			{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Reset Machine"},
			{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -1, "Ignore"}
		};
		const SDL_MessageBoxData msg_box = {
			SDL_MESSAGEBOX_ERROR, window, "Error", error_message,
			2, btns, NULL
		};

		if (is_gen2) {
			sprintf(error_message, "Encountered stop instruction at address $%06X. CPU cannot continue.", (regs.k << 16) | address);
		} else {
			sprintf(error_message, "Encountered stop instruction at address $%04X. CPU cannot continue.", address);
		}
		if (SDL_ShowMessageBox(&msg_box, &return_btn) == 0 && return_btn == 0) {
			input_log_end("machine reset");
			machine_reset();
		};
	}
}


void
video_write(uint8_t reg, uint8_t value)
{
	video_write_inner(reg, value);
	fx_resolve_pending();
}

uint8_t
video_read(uint8_t reg, bool debugOn)
{
	uint8_t value = video_read_inner(reg, debugOn);
	if (!debugOn) {
		fx_resolve_pending();
	}
	return value;
}
