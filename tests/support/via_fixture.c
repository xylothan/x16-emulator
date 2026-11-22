// Link seam for testing the real via.c.
//
// via.c: a generic half shared by both VIAs, and VIA 1's port wiring.
// Supplies VIA 1's references so it links; ports are inert and read an idle bus.

#include "i2c.h"
#include "joystick.h"
#include "serial.h"

#include <stdbool.h>
#include <stdint.h>

// ─── I2C ────────────────────────────────────────────────────────────────────

i2c_port_t i2c_port;

void i2c_step(void) { }

// ─── IEC serial ─────────────────────────────────────────────────────────────

serial_port_t serial_port;

bool serial_port_read_clk(void) { return true; }
bool serial_port_read_data(void) { return true; }

// ─── NES controllers ────────────────────────────────────────────────────────

uint8_t Joystick_data;

void joystick_set_latch(bool value) { (void)value; }
void joystick_set_clock(bool value) { (void)value; }
