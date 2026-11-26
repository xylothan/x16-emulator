// Commander X16 Emulator
// Copyright (c) 2021 Michael Steil
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <stdbool.h>
#include "i2c.h"
#include "savestate.h"
#include "smc.h"
#include "rtc.h"
#include "io_trace.h"
#include "input_log.h"

#define LOG_LEVEL 0

#define DEVICE_SMC 0x42
#define DEVICE_RTC 0x6F

i2c_port_t i2c_port;

// Debug-only session counters; pure increments, no effect on emulation.
static uint32_t i2c_debug_transactions_started   = 0;
static uint32_t i2c_debug_transactions_completed = 0;
static uint32_t i2c_debug_bytes_read             = 0;
static uint32_t i2c_debug_bytes_written          = 0;

#define KBD_SIZE 16
uint8_t kbd_buffer[KBD_SIZE];						//Ring buffer for key codes

#define MSE_SIZE 16
uint8_t mse_buffer[MSE_SIZE];						//Ring buffer for mouse movement data

// The bus, after MCP7940N DS20005010J Section 4.1.1 (bus characteristics): START
// is SDA falling while SCL is high and STOP is SDA rising while SCL is high,
// at any point; data is taken while SCL is high and changes while it is low.
// A target drives its acknowledge and its data bits from the SCL falling edge
// before the clock that carries them. After a NACK the bus is ignored until
// the next START or STOP.

typedef struct {
	uint8_t address;
	bool    (*start)(bool read);   // addressed: acknowledge?
	bool    (*write)(uint8_t v);   // a byte from the controller: acknowledge?
	uint8_t (*read)(void);         // the next byte for the controller
	void    (*stop)(void);         // STOP, or a START that ends the transaction
} i2c_target_t;

static const i2c_target_t targets[] = {
	{ DEVICE_SMC, smc_i2c_start, smc_i2c_write, smc_i2c_read, smc_i2c_stop },
	{ DEVICE_RTC, rtc_i2c_start, rtc_i2c_write, rtc_i2c_read, rtc_i2c_stop },
};

enum {
	BUS_IDLE,        // no transaction
	BUS_RX,          // the controller sends bits
	BUS_RX_DONE,     // eight bits in, the acknowledge goes out at SCL low
	BUS_TARGET_ACK,  // the target drives its acknowledge
	BUS_TX,          // the target sends bits
	BUS_CTRL_ACK,    // the controller acknowledges the byte
	BUS_IGNORE,      // not acknowledged: wait for START or STOP
};

static int bus = BUS_IDLE;
static int bits;
static uint8_t shift;
static bool address_byte;
static bool ack_out;
static bool read_mode;
static int count;
static uint8_t device;
static const i2c_target_t *target;
static int last_scl = 1, last_sda = 1;

static int
sda_line(void)
{
	return i2c_port.data_in & i2c_port.data_out;
}

static void
end_transaction(void)
{
	if (io_trace_wants(IO_DEV_I2C) && target && count > 0) {
		const char *dname = (device == DEVICE_SMC) ? "SMC" : (device == DEVICE_RTC) ? "RTC" : "?";
		io_trace_event(IO_DEV_I2C, "i2c STOP $%02X(%s) %s %d byte%s",
			device, dname, read_mode ? "read" : "write", count, count == 1 ? "" : "s");
	}
	if (target) {
		target->stop();
	}
	target = NULL;
	count = 0;
	read_mode = false;
	i2c_port.data_out = 1;
}

static void
bus_start(void)
{
	if (bus == BUS_IDLE) {
		i2c_debug_transactions_started++;
	}
	end_transaction();
	bus = BUS_RX;
	bits = 0;
	shift = 0;
	address_byte = true;
}

static void
bus_stop(void)
{
	end_transaction();
	if (bus != BUS_IDLE) {
		i2c_debug_transactions_completed++;
	}
	bus = BUS_IDLE;
}

static void
byte_received(void)
{
	if (address_byte) {
		device = shift >> 1;
		read_mode = shift & 1;
		target = NULL;
		for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
			if (targets[i].address == device) {
				target = &targets[i];
			}
		}
		ack_out = target && target->start(read_mode);
		if (!target && io_trace_wants(IO_DEV_I2C)) {
			io_trace_event(IO_DEV_I2C, "i2c NACK $%02X unknown device", device);
		}
		address_byte = false;
	} else {
		ack_out = target->write(shift);
		if (ack_out) {
			count++;
			i2c_debug_bytes_written++;
		}
	}
}

static void
tx_byte(void)
{
	shift = target->read();
	i2c_debug_bytes_read++;
	count++;
	bits = 0;
	i2c_port.data_out = shift >> 7;
	bus = BUS_TX;
}

static void
scl_rising(int sda)
{
	switch (bus) {
		case BUS_RX:
			shift = (uint8_t)(shift << 1 | sda);
			if (++bits == 8) {
				byte_received();
				bus = BUS_RX_DONE;
			}
			break;
		case BUS_TX:
			bits++;
			break;
		case BUS_CTRL_ACK:
			ack_out = !sda;
			break;
	}
}

static void
scl_falling(void)
{
	switch (bus) {
		case BUS_RX_DONE:
			i2c_port.data_out = ack_out ? 0 : 1;
			bus = BUS_TARGET_ACK;
			break;
		case BUS_TARGET_ACK:
			i2c_port.data_out = 1;
			if (!ack_out) {
				bus = BUS_IGNORE;
			} else if (read_mode) {
				tx_byte();
			} else {
				bus = BUS_RX;
				bits = 0;
				shift = 0;
			}
			break;
		case BUS_TX:
			if (bits < 8) {
				i2c_port.data_out = (shift >> (7 - bits)) & 1;
			} else {
				i2c_port.data_out = 1;
				bus = BUS_CTRL_ACK;
			}
			break;
		case BUS_CTRL_ACK:
			if (ack_out) {
				tx_byte();
			} else {
				bus = BUS_IGNORE;
			}
			break;
	}
}

void
i2c_step()
{
	const int scl = i2c_port.clk_in;
	const int sda = sda_line();
	if (scl == last_scl && sda == last_sda) {
		return;
	}
	if (scl && last_scl) {
		if (!sda) {
			bus_start();
		} else {
			bus_stop();
		}
	} else if (scl) {
		scl_rising(sda);
	} else if (last_scl) {
		scl_falling();
	}
	last_scl = scl;
	last_sda = sda_line();
}

void i2c_reset_state() {
	bus = BUS_IDLE;
	target = NULL;
	count = 0;
	read_mode = false;
	i2c_port.data_out = 1;
	smc_requested_reset = false;
	smc_reset();
	i2c_mse_buffer_flush();
	i2c_kbd_buffer_flush();
}


/**
 * Keyboard buffer functions
 **/
uint8_t kbd_head=0;
uint8_t kbd_tail=0;

/**
 * Adds value to the keyboard ring buffer; the value is discarded if the buffer is full
 **/
void i2c_kbd_buffer_add(uint8_t value) {
	if (input_log_host_blocked()) return;
	if (input_log_is_recording()) input_log_note(INPUT_EV_KBD, value, 0, 0);
	uint8_t next = (kbd_head + 1) & (KBD_SIZE - 1);		//Next available index
    if (next != kbd_tail) {								//next = tail => buffer full
    	kbd_buffer[kbd_head] = value;					//Set new value
        kbd_head = next;								//Update buffer head pointer
    }
}

/**
 * Returns the tail value from the keyboard ring buffer; if the buffer is empty, it returns 0
 **/
uint8_t i2c_kbd_buffer_next() {
	uint8_t value = 0;									//Prepare to return 0 of buffer is empty
	if (kbd_head != kbd_tail) {							//head = tail => empty
    	value = kbd_buffer[kbd_tail];					//Get value
        kbd_tail = (kbd_tail + 1) & (KBD_SIZE - 1);		//Update buffer tail pointer
    }
	return value;
}

/**
 * Clears the keyboard ring buffer
 **/
void i2c_kbd_buffer_flush() {
	kbd_tail = kbd_head = 0;
}

/**
 * Returns number of values currently stored in the keyboard ring buffer
 **/
uint8_t i2c_kbd_buffer_count() {
	return (KBD_SIZE + kbd_head - kbd_tail) & (KBD_SIZE - 1);
}

/**
 * Mouse buffer functions
 **/
uint8_t mse_head=0;
uint8_t mse_tail=0;

/**
 * Adds value to the mouse ring buffer; discards the value if the buffer is full
 **/
void i2c_mse_buffer_add(uint8_t value) {
	uint8_t next = (mse_head + 1) & (MSE_SIZE - 1);				//Next available index
    if (next != mse_tail) {										//next = tail => buffer full
    	mse_buffer[mse_head] = value;							//Set value
        mse_head = next;										//Update head pointer
    }
}

/**
 * Returns the tail value from the mouse ring buffer; if the buffer is empty, it returns 0
 **/
uint8_t i2c_mse_buffer_next() {
	uint8_t value = 0;											//Prepare to return 0 if buffer empty
	if (mse_head != mse_tail) {									//head = tail => buffer empty
    	value = mse_buffer[mse_tail];							//Get tail value
        mse_tail = (mse_tail + 1) & (MSE_SIZE - 1);				//Update tail pointer
    }
	return value;
}

/**
 * Clears the mouse ring buffer
 **/
void i2c_mse_buffer_flush() {
	mse_tail = mse_head = 0;
}

/**
 * Returns number of values currently stored in the ring buffer
 **/
uint8_t i2c_mse_buffer_count() {
    return (MSE_SIZE + mse_head - mse_tail) & (MSE_SIZE - 1);
}


/**
 *  fake mouse
 **/

static uint8_t buttons;
static int16_t mouse_diff_x = 0;
static int16_t mouse_diff_y = 0;
static int8_t wheel = 0;
static uint8_t mouse_device_id = 3;

// byte 0, bit 7: Y overflow
// byte 0, bit 6: X overflow
// byte 0, bit 5: Y sign bit
// byte 0, bit 4: X sign bit
// byte 0, bit 3: Always 1
// byte 0, bit 2: Middle Btn
// byte 0, bit 1: Right Btn
// byte 0, bit 0: Left Btn
// byte 2:        X Movement
// byte 3:        Y Movement

static bool
mouse_send(int x, int y, int b, uint8_t w)
{
	uint8_t psize;
	if (mouse_device_id == 3 || mouse_device_id == 4) {
		psize = 4;
	}
	else {
		psize  = 3;
	}

	if (i2c_mse_buffer_count() < MSE_SIZE-psize) {
		uint8_t byte0 =
		    ((y >> 9) & 1) << 5 |
		    ((x >> 9) & 1) << 4 |
		    1 << 3 |
		    b;
		i2c_mse_buffer_add(byte0);
		i2c_mse_buffer_add(x);
		i2c_mse_buffer_add(y);

		if (mouse_device_id == 3 || mouse_device_id == 4) {
			i2c_mse_buffer_add(w);
		}

		return true;
	} else {
		return false;
	}
}

void
mouse_send_state(void)
{
	if (input_log_host_blocked()) return;
	if (input_log_is_recording()) input_log_note(INPUT_EV_MOUSE_SEND, 0, 0, 0);
	do {
		int send_diff_x = mouse_diff_x > 255 ? 255 : (mouse_diff_x < -256 ? -256 : mouse_diff_x);
		int send_diff_y = mouse_diff_y > 255 ? 255 : (mouse_diff_y < -256 ? -256 : mouse_diff_y);

		mouse_send(send_diff_x, send_diff_y, buttons, wheel);

		mouse_diff_x -= send_diff_x;
		mouse_diff_y -= send_diff_y;
		wheel = 0;
	} while (mouse_diff_x != 0 && mouse_diff_y != 0);
}

void
mouse_button_down(int num)
{
	if (input_log_host_blocked()) return;
	if (input_log_is_recording()) input_log_note(INPUT_EV_MOUSE_DOWN, (uint8_t)num, 0, 0);
	buttons |= 1 << num;
}

void
mouse_button_up(int num)
{
	if (input_log_host_blocked()) return;
	if (input_log_is_recording()) input_log_note(INPUT_EV_MOUSE_UP, (uint8_t)num, 0, 0);
	buttons &= (1 << num) ^ 0xff;
}

uint8_t
mouse_buttons(void)
{
	return buttons;
}

void
mouse_move(int x, int y)
{
	if (input_log_host_blocked()) return;
	if (input_log_is_recording()) input_log_note(INPUT_EV_MOUSE_MOVE, 0, x, y);
	mouse_diff_x += x;
	mouse_diff_y -= y;
}

uint8_t
mouse_read(uint8_t reg)
{
	return 0xff;
}

void 
mouse_set_wheel(int8_t y)
{
	if (input_log_host_blocked()) return;
	if (input_log_is_recording()) input_log_note(INPUT_EV_MOUSE_WHEEL, 0, y, 0);
	if (mouse_device_id == 3 || mouse_device_id == 4) {
		if (y < -7) {
			wheel = 7;
		}
		else if (y > 8) {
			wheel = -8;
		}
		else {
			wheel = -y;
		}
	}
}

uint8_t 
mouse_get_device_id() {
	return mouse_device_id;
}

void 
mouse_set_device_id(uint8_t d) {
	switch(d) {
		case 0:
		case 3:
			mouse_device_id = d;
			break;
		case 4:
			mouse_device_id =3;
			break;
		default:
			mouse_device_id = 0;
			break;
	}
	i2c_mse_buffer_flush();
}

// Side-effect-free snapshot of I2C bus state for the ImGui debugger.
void
i2c_debug_get_state(i2c_debug_state_t *out)
{
	out->state    = bus == BUS_IDLE ? -1 : bits;
	out->read_mode = read_mode;
	out->value    = shift;
	out->count    = count;
	out->device   = device;

	switch (device) {
		case 0x42: out->device_name[0] = 'S'; out->device_name[1] = 'M'; out->device_name[2] = 'C'; out->device_name[3] = '\0'; break;
		case 0x6F: out->device_name[0] = 'R'; out->device_name[1] = 'T'; out->device_name[2] = 'C'; out->device_name[3] = '\0'; break;
		default:   out->device_name[0] = '\0'; break;
	}

	out->clk_in  = i2c_port.clk_in;
	out->data_in = i2c_port.data_in;
	out->data_out = i2c_port.data_out;

	out->transactions_started   = i2c_debug_transactions_started;
	out->transactions_completed = i2c_debug_transactions_completed;
	out->bytes_read             = i2c_debug_bytes_read;
	out->bytes_written          = i2c_debug_bytes_written;

	// keyboard buffer fill = (KBD_SIZE + head - tail) & (KBD_SIZE - 1)
	out->kbd_fill = i2c_kbd_buffer_count();
	out->mse_fill = i2c_mse_buffer_count();
}

// ─── Save states ─────────────────────────────────────────────────────────────
//
// The bus state machine and its lines, the keyboard and mouse queues the SMC
// serves, and the mouse's accumulated movement. The debug counters are left.
// Version 1 held another bus state machine; it loads with the bus idle.

void
i2c_state_save(ss_writer *w)
{
	ss_begin(w, "I2C ", 2);
	ss_put_u32(w, (uint32_t)i2c_port.clk_in);
	ss_put_u32(w, (uint32_t)i2c_port.data_in);
	ss_put_u32(w, (uint32_t)i2c_port.data_out);
	ss_put_u8(w, (uint8_t)bus);
	ss_put_u8(w, (uint8_t)bits);
	ss_put_u8(w, shift);
	ss_put_bool(w, address_byte);
	ss_put_bool(w, ack_out);
	ss_put_bool(w, read_mode);
	ss_put_u32(w, (uint32_t)count);
	ss_put_u8(w, device);
	ss_put_bool(w, target != NULL);
	ss_put_u8(w, (uint8_t)last_scl);
	ss_put_u8(w, (uint8_t)last_sda);
	ss_put(w, kbd_buffer, sizeof kbd_buffer);
	ss_put_u8(w, kbd_head);
	ss_put_u8(w, kbd_tail);
	ss_put(w, mse_buffer, sizeof mse_buffer);
	ss_put_u8(w, mse_head);
	ss_put_u8(w, mse_tail);
	ss_put_u8(w, buttons);
	ss_put_u16(w, (uint16_t)mouse_diff_x);
	ss_put_u16(w, (uint16_t)mouse_diff_y);
	ss_put_u8(w, (uint8_t)wheel);
	ss_put_u8(w, mouse_device_id);
	ss_end(w);
}

void
i2c_state_load(ss_reader *r)
{
	const int version = ss_open(r, "I2C ");
	i2c_port.clk_in   = (int)ss_get_u32(r);
	i2c_port.data_in  = (int)ss_get_u32(r);
	i2c_port.data_out = (int)ss_get_u32(r);
	target = NULL;
	if (version < 2) {
		(void)ss_get_u32(r);
		(void)ss_get_bool(r);
		(void)ss_get_u8(r);
		(void)ss_get_u32(r);
		(void)ss_get_u8(r);
		bus = BUS_IDLE;
		bits = 0;
		shift = 0;
		address_byte = false;
		ack_out = false;
		read_mode = false;
		count = 0;
		i2c_port.data_out = 1;
		last_scl = i2c_port.clk_in;
		last_sda = sda_line();
	} else {
		bus          = ss_get_u8(r);
		bits         = ss_get_u8(r);
		shift        = ss_get_u8(r);
		address_byte = ss_get_bool(r);
		ack_out      = ss_get_bool(r);
		read_mode    = ss_get_bool(r);
		count        = (int)ss_get_u32(r);
		device       = ss_get_u8(r);
		const bool active = ss_get_bool(r);
		last_scl     = ss_get_u8(r) & 1;
		last_sda     = ss_get_u8(r) & 1;
		if (bus > BUS_IGNORE || bits > 8) {
			bus = BUS_IDLE;
			bits = 0;
		}
		for (size_t i = 0; active && i < sizeof targets / sizeof targets[0]; i++) {
			if (targets[i].address == device) {
				target = &targets[i];
			}
		}
	}
	ss_get(r, kbd_buffer, sizeof kbd_buffer);
	kbd_head = ss_get_u8(r);
	kbd_tail = ss_get_u8(r);
	ss_get(r, mse_buffer, sizeof mse_buffer);
	mse_head = ss_get_u8(r);
	mse_tail = ss_get_u8(r);
	buttons         = ss_get_u8(r);
	mouse_diff_x    = (int16_t)ss_get_u16(r);
	mouse_diff_y    = (int16_t)ss_get_u16(r);
	wheel           = (int8_t)ss_get_u8(r);
	mouse_device_id = ss_get_u8(r);
	ss_close(r);
}
