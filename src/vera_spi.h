// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#pragma once

#include <inttypes.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void vera_spi_init();
void vera_spi_step(int mhz, int clocks);
uint8_t vera_spi_read(uint8_t address);
// Side-effect-free read of the same registers, for debug views. Unlike
// vera_spi_read(), does not start an autotx transfer.
uint8_t vera_spi_peek(uint8_t address);
void vera_spi_write(uint8_t address, uint8_t value);

// ─── Debugger accessor ───────────────────────────────────────────────────────
// Side-effect-free snapshot of VERA SPI state. Safe to call every frame from
// the debugger while the emulation is running.

typedef struct {
	bool    ss;            // slave-select (chip-select) asserted
	bool    busy;          // transfer currently in flight
	bool    autotx;        // auto-transmit mode active
	uint8_t sending_byte;  // byte currently being clocked out
	uint8_t received_byte; // last byte received from card
	float   outcounter;    // clock-cycle progress toward transfer completion
} vera_spi_debug_state_t;

void vera_spi_debug_get_state(vera_spi_debug_state_t *out);

#ifdef __cplusplus
}
#endif
