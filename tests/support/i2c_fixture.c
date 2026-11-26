// Link seam for testing the real i2c.c, smc.c and rtc.c.

#include "i2c_fixture.h"

#include <stdbool.h>
#include <stdint.h>

uint8_t MHZ = 8;
bool pwr_long_press = false;
int i2c_fixture_nmis = 0;

void main_shutdown(void) { }
void nmi6502(void) { i2c_fixture_nmis++; }

static void
lines(int scl, int sda)
{
	i2c_port.clk_in = scl;
	i2c_port.data_in = sda;
	i2c_step();
}

void
i2c_bus_start(void)
{
	lines(i2c_port.clk_in, 1);
	lines(1, 1);
	lines(1, 0);
	lines(0, 0);
}

void
i2c_bus_stop(void)
{
	lines(0, 0);
	lines(1, 0);
	lines(1, 1);
}

static void
bit_out(int b)
{
	lines(0, b);
	lines(1, b);
	lines(0, b);
}

static int
bit_in(void)
{
	lines(0, 1);
	lines(1, 1);
	const int b = i2c_port.data_out;
	lines(0, 1);
	return b;
}

bool
i2c_bus_send(uint8_t v)
{
	for (int i = 7; i >= 0; i--) {
		bit_out((v >> i) & 1);
	}
	return bit_in() == 0;
}

uint8_t
i2c_bus_recv(bool ack)
{
	uint8_t v = 0;
	for (int i = 0; i < 8; i++) {
		v = (uint8_t)(v << 1 | bit_in());
	}
	bit_out(ack ? 0 : 1);
	return v;
}

void
i2c_fixture_reset(void)
{
	i2c_port.clk_in = 1;
	i2c_port.data_in = 1;
	i2c_port.data_out = 1;
	i2c_step();
	i2c_reset_state();
	i2c_fixture_nmis = 0;
}
