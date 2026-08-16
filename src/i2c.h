// Commander X16 Emulator
// Copyright (c) 2021 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef _I2C_H_
#define _I2C_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_DATA_MASK 1
#define I2C_CLK_MASK 2

typedef struct {
	int clk_in;
	int data_in;
	int data_out;
} i2c_port_t;

extern i2c_port_t i2c_port;

void i2c_reset_state();
void i2c_step();

void i2c_kbd_buffer_add(uint8_t value);
uint8_t i2c_kbd_buffer_next();
void i2c_kbd_buffer_flush();
uint8_t i2c_kbd_buffer_count();

void i2c_mse_buffer_add(uint8_t value);
uint8_t i2c_mse_buffer_next();
void i2c_mse_buffer_flush();
uint8_t i2c_mse_buffer_count();

// fake mouse
void mouse_button_down(int num);
void mouse_button_up(int num);
void mouse_move(int x, int y);
uint8_t mouse_read(uint8_t reg);
void mouse_send_state(void);
void mouse_set_wheel(int8_t y);
uint8_t mouse_get_device_id();
void mouse_set_device_id(uint8_t d);

// ─── Debugger accessor ───────────────────────────────────────────────────────
// Side-effect-free snapshot of I2C bus state. Safe to call every frame from
// the debugger while the emulation is running.
//
// Known device addresses: 0x42 = SMC, 0x6F = RTC.
// device_name is a short static string label ("SMC", "RTC", or "" for unknown).
// Session counters (transactions_started etc.) are cumulative since boot and
// are never decremented.

typedef struct {
	int      state;                  // bus state machine: -1=STOP, 0=START, 1-8=bit index
	bool     read_mode;              // current byte is being read from the device
	uint8_t  value;                  // byte currently being assembled/shifted
	int      count;                  // byte count within the current transaction
	uint8_t  device;                 // I2C device address for the current transaction
	char     device_name[8];         // human-readable name for the device address
	int      clk_in;                 // i2c_port.clk_in
	int      data_in;                // i2c_port.data_in
	int      data_out;               // i2c_port.data_out
	uint32_t transactions_started;   // cumulative START conditions seen
	uint32_t transactions_completed; // cumulative STOP conditions seen
	uint32_t bytes_read;             // cumulative bytes read from devices
	uint32_t bytes_written;          // cumulative bytes written to devices
	uint8_t  kbd_fill;               // keyboard ring buffer occupancy
	uint8_t  mse_fill;               // mouse ring buffer occupancy
} i2c_debug_state_t;

void i2c_debug_get_state(i2c_debug_state_t *out);

#ifdef __cplusplus
}
#endif

#endif
