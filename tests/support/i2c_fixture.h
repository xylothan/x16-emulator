// A bus controller for the real i2c.c, driving SDA and SCL the way the KERNAL's
// i2c.s does: SDA changes while SCL is low.

#ifndef X16_I2C_FIXTURE_H
#define X16_I2C_FIXTURE_H

#include "i2c.h"
#include "rtc.h"
#include "smc.h"

#include <stdbool.h>
#include <stdint.h>

extern int i2c_fixture_nmis;
extern bool pwr_long_press;
extern uint8_t activity_led;

void i2c_fixture_reset(void);
void i2c_bus_start(void);
void i2c_bus_stop(void);
bool i2c_bus_send(uint8_t v);          // true if acknowledged
uint8_t i2c_bus_recv(bool ack);

#endif
