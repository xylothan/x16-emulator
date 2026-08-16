// Commander X16 Emulator
// Copyright (c) 2021 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef _SMC_H_
#define _SMC_H_

#define SMC_VERSION_MAJOR 47
#define SMC_VERSION_MINOR 0
#define SMC_VERSION_PATCH 0

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void nmi6502();
void smc_i2c_data(uint8_t v);
uint8_t smc_read();
void smc_write();

extern bool smc_requested_reset;

// ─── Debugger accessor ───────────────────────────────────────────────────────
// Side-effect-free snapshot of SMC state. Safe to call every frame from the
// debugger while the emulation is running.

#define SMC_I2C_DATA_LEN 16

typedef struct {
	uint8_t default_read_op;                  // current default read operation register
	uint8_t activity_led;                     // activity LED brightness (0 or 255)
	uint8_t i2c_data[SMC_I2C_DATA_LEN];      // pending I2C data buffer
	uint8_t i2c_data_pos;                     // write position in i2c_data[]
	uint8_t mse_count;                        // mouse packet byte counter
	bool    requested_reset;                  // SMC has requested a machine reset
	uint8_t kbd_fill;                         // keyboard buffer occupancy (bytes pending)
	uint8_t mse_fill;                         // mouse buffer occupancy (bytes pending)
} smc_debug_state_t;

void smc_debug_get_state(smc_debug_state_t *out);

#ifdef __cplusplus
}
#endif

#endif
