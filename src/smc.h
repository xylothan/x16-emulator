// Commander X16 Emulator
// Copyright (c) 2021 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef _SMC_H_
#define _SMC_H_

#define SMC_VERSION_MAJOR 48
#define SMC_VERSION_MINOR 0
#define SMC_VERSION_PATCH 0

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void nmi6502();

// I2C target at $42: see i2c.h.
bool smc_i2c_start(bool read);
bool smc_i2c_write(uint8_t v);
uint8_t smc_i2c_read(void);
void smc_i2c_stop(void);
void smc_reset(void);

extern bool smc_requested_reset;

// ─── Debugger accessor ───────────────────────────────────────────────────────
// Side-effect-free snapshot of SMC state. Safe to call every frame from the
// debugger while the emulation is running.

#define SMC_I2C_DATA_LEN 16

typedef struct {
	uint8_t default_read_op;                  // current default read operation register
	uint8_t activity_led;                     // activity LED brightness (0 or 255)
	uint8_t i2c_data[SMC_I2C_DATA_LEN];      // bytes of the write in progress
	uint8_t i2c_data_pos;                     // bytes written so far
	uint8_t mse_count;                        // reply bytes read so far
	bool    requested_reset;                  // SMC has requested a machine reset
	uint8_t kbd_fill;                         // keyboard buffer occupancy (bytes pending)
	uint8_t mse_fill;                         // mouse buffer occupancy (bytes pending)
} smc_debug_state_t;

void smc_debug_get_state(smc_debug_state_t *out);

#ifdef __cplusplus
}
#endif

#endif
