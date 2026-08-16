// Link seam for testing the real via.c.
//
// via.c holds two 65C22s. The generic half -- the registers, the interrupt
// flags and enables, the timers and the shift register -- is shared by both
// and touches nothing outside the chip. The other half wires VIA 1's ports to
// the machine: I2C to the SMC and RTC, the IEC serial bus, and the NES
// controllers.
//
// VIA 2 is the user port and via2_read()/via2_write() pass straight through to
// the generic half, so a test of the chip itself drives VIA 2 and needs none
// of that. This supplies what VIA 1's half refers to so the translation unit
// links.
//
// Every port here is inert and reads as an idle bus. Nothing records: a test
// that depends on one of these has left the 65C22 and gone into the machine
// around it, and wants rewriting rather than accommodating.

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
