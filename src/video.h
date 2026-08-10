// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef _VIDEO_H_
#define _VIDEO_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <SDL.h>
#include "glue.h"

#ifdef __cplusplus
extern "C" {
#endif

bool video_init(int window_scale, float screen_x_scale, char *quality, bool fullscreen, float opacity);
void video_reset(void);
bool video_step(float mhz, float steps, bool midline);
bool video_update(void);
void video_present_no_input(void);
void video_repaint_only(void);
// The ImGui debugger window, in builds configured with -DENABLE_IMGUI.
// Both are always defined: without the UI, available() is false and the pump
// reports "stay paused", so call sites need no #ifdef.
bool video_debug_ui_available(void);
int  video_debug_ui_pump_paused(void);
bool video_is_debug_ui_window(Uint32 window_id);
bool video_event_targets_debug_ui(const SDL_Event *ev);
void video_debug_ui_shortcut_key(const SDL_Event *ev);
void video_debug_ui_feed_event(const SDL_Event *ev);

// Size in layer pixels of the image the composer is actually displaying: the
// active window (DC_HSTART/HSTOP, DC_VSTART/VSTOP) scaled by DC_HSCALE/VSCALE.
// Viewers use this to size themselves to the current video mode.
void video_get_active_layer_size(int *out_w, int *out_h);

// Cycles until the next enabled VERA interrupt and the ISR bit that will cause
// it (1 = VSYNC, 2 = LINE); false when neither is enabled.
bool video_next_irq(float mhz, uint32_t *out_cycles, uint8_t *out_source);
void video_get_irq_state(uint8_t *out_ien, uint8_t *out_isr, uint16_t *out_irq_line);
void video_end(void);
bool video_get_irq_out(void);
void video_save(SDL_RWops *f);
uint8_t video_read(uint8_t reg, bool debugOn);
void video_write(uint8_t reg, uint8_t value);
void video_update_title(const char* window_title);

uint8_t via1_read(uint8_t reg, bool debug);
void via1_write(uint8_t reg, uint8_t value);

// For debugging purposes only:
uint8_t video_space_read(uint32_t address);
void video_space_write(uint32_t address, uint8_t value);

bool video_is_tilemap_address(int addr);
bool video_is_tiledata_address(int addr);
bool video_is_special_address(int addr);

uint32_t video_get_address(uint8_t sel);
uint32_t video_get_fx_accum(void);
uint8_t video_get_dc_value(uint8_t reg);

#ifdef __cplusplus
}
#endif

#endif
