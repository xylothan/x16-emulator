// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#include <inttypes.h>

void vera_spi_init();
void vera_spi_step(int mhz, int clocks);
uint8_t vera_spi_read(uint8_t address);
// Side-effect-free read of the same registers, for debug views. Unlike
// vera_spi_read(), does not start an autotx transfer.
uint8_t vera_spi_peek(uint8_t address);
void vera_spi_write(uint8_t address, uint8_t value);
