#pragma once
#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JOY_LATCH_MASK 0x04
#define JOY_CLK_MASK 0x08

#define NUM_JOYSTICKS 4

// ─── Button bit layout (button_mask, 16 bits, 0=pressed) ────────────────────
// Derived from the button_map[] table in joystick.c. The ImGui panel uses
// these defines to label each bit in the mask display.
#define JOY_BIT_A             (1 << 0)   // A button
#define JOY_BIT_X             (1 << 1)   // X button
#define JOY_BIT_SELECT        (1 << 2)   // Back / Select
#define JOY_BIT_START         (1 << 3)   // Start
#define JOY_BIT_DPAD_UP       (1 << 4)   // D-Pad Up
#define JOY_BIT_DPAD_DOWN     (1 << 5)   // D-Pad Down
#define JOY_BIT_DPAD_LEFT     (1 << 6)   // D-Pad Left
#define JOY_BIT_DPAD_RIGHT    (1 << 7)   // D-Pad Right
#define JOY_BIT_B             (1 << 8)   // B button
#define JOY_BIT_Y             (1 << 9)   // Y button
#define JOY_BIT_LEFT_SHOULDER (1 << 10)  // Left shoulder / L
#define JOY_BIT_RIGHT_SHOULDER (1 << 11) // Right shoulder / R
// Bits 12-15 are forced to 1 when the latch is loaded (0xF000 mask applied).

extern uint8_t Joystick_data;
extern bool Joystick_slots_enabled[NUM_JOYSTICKS];

bool joystick_init(void); //initialize SDL controllers
void joystick_add(int index);
void joystick_remove(int index);

void joystick_button_down(int instance_id, uint8_t button);
void joystick_button_up(int instance_id, uint8_t button);

void joystick_set_latch(bool value);
void joystick_set_clock(bool value);

// ─── Debugger accessor ───────────────────────────────────────────────────────
// Side-effect-free snapshot of joystick state. Safe to call every frame from
// the debugger while the emulation is running.

typedef struct {
	bool     enabled;          // slot is enabled (Joystick_slots_enabled[i])
	bool     controller_bound; // a real SDL controller is mapped to this slot
	uint16_t button_mask;      // current button state (0=pressed, see JOY_BIT_*)
	uint16_t shift_mask;       // current shift register value being clocked out
} joystick_slot_debug_t;

typedef struct {
	joystick_slot_debug_t slots[NUM_JOYSTICKS];
	bool    latch;             // current latch line state
	uint8_t data;              // current Joystick_data byte
} joystick_debug_state_t;

void joystick_debug_get_state(joystick_debug_state_t *out);

#ifdef __cplusplus
}
#endif

#endif
