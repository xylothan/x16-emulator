// Commander X16 Emulator
// Copyright (c) 2021 Michael Steil
// All rights reserved. License: 2-clause BSD

// System Management Controller, after x16-smc r48.0.0 (0a2684d):
// x16-smc.ino I2C_Receive()/I2C_Send() and smc_wire.cpp.
//
// A write is acted on once, when the transaction ends, from its first three
// bytes. A read's reply is built when the read address arrives; an empty reply
// is not acknowledged, and bytes past its end read $FF.

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "smc.h"
#include "savestate.h"
#include "glue.h"
#include "i2c.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define CMD_POW_OFF            0x01
#define CMD_RESET              0x02
#define CMD_NMI                0x03
#define CMD_SET_ACT_LED        0x05
#define CMD_GET_KEYCODE        0x07
#define CMD_ECHO               0x08
#define CMD_GET_LONGPRESS      0x09
#define CMD_GET_KBD_STATUS     0x18
#define CMD_KBD_CMD1           0x19
#define CMD_KBD_CMD2           0x1a
#define CMD_KBD_INIT_STATE     0x1b
#define CMD_SET_MOUSE_ID       0x20
#define CMD_GET_MOUSE_MOV      0x21
#define CMD_GET_MOUSE_ID       0x22
#define CMD_GET_VER1           0x30
#define CMD_GET_VER2           0x31
#define CMD_GET_VER3           0x32
#define CMD_SET_DFLT_READ_OP   0x40
#define CMD_GET_KEYCODE_FAST   0x41
#define CMD_GET_MOUSE_MOV_FAST 0x42
#define CMD_GET_PS2DATA_FAST   0x43
#define CMD_GET_BOOTLDR_VER    0x8e
#define CMD_SELF_PROGRAMMING   0x93

#define PS2_CMD_ACK     0xfa   // ps2.h PS2_CMD_STATUS::CMD_ACK
#define KBD_STATE_READY 0x01   // setup_ps2.h

#define BUFSIZE 32             // smc_wire.cpp

uint8_t activity_led;
bool smc_requested_reset = false;

static uint8_t default_request = CMD_GET_KEYCODE_FAST;
static uint8_t cmd[3];                // I2C_Data
static uint8_t echo_byte;
static uint8_t kbd_status = PS2_CMD_ACK;
static bool    powered;

static bool    master_read;
static uint8_t rx[BUFSIZE], rx_len;
static uint8_t tx[BUFSIZE], tx_len, tx_idx;

static void
tx_put(uint8_t v)
{
	if (tx_len < BUFSIZE) {
		tx[tx_len++] = v;
	}
}

static bool
send_key_code(void)
{
	if (i2c_kbd_buffer_count()) {
		tx_put(i2c_kbd_buffer_next());
		return true;
	}
	tx_put(0);
	return false;
}

static uint8_t
mouse_packet_size(void)
{
	return mouse_get_device_id() ? 4 : 3;
}

// A packet starts with bit 3 set; one flagging overflow is dropped whole.
static bool
send_mouse_packet(void)
{
	const uint8_t size = mouse_packet_size();
	if (i2c_mse_buffer_count() >= size) {
		const uint8_t first = i2c_mse_buffer_next();
		if (first & 0x08) {
			if (!(first & 0xc0)) {
				tx_put(first);
				for (uint8_t i = 1; i < size; i++) {
					tx_put(i2c_mse_buffer_next());
				}
				return true;
			}
			for (uint8_t i = 1; i < size; i++) {
				i2c_mse_buffer_next();
			}
		}
	}
	tx_put(0);
	return false;
}

static void
smc_send(void)
{
	switch (cmd[0]) {
		case CMD_GET_KEYCODE_FAST:
			if (!send_key_code()) tx_len = 0;
			break;
		case CMD_GET_PS2DATA_FAST: {
			const bool key = send_key_code();
			const bool mouse = send_mouse_packet();
			if (!key && !mouse) tx_len = 0;
			break;
		}
		case CMD_GET_MOUSE_MOV_FAST:
			if (!send_mouse_packet()) tx_len = 0;
			break;
		case CMD_GET_KEYCODE:
			send_key_code();
			break;
		case CMD_GET_MOUSE_MOV:
			send_mouse_packet();
			break;
		case CMD_ECHO:
			tx_put(echo_byte);
			break;
		case CMD_GET_LONGPRESS:
			tx_put(pwr_long_press ? 1 : 0);
			break;
		case CMD_GET_KBD_STATUS:
			tx_put(kbd_status);
			break;
		case CMD_KBD_INIT_STATE:
			tx_put(KBD_STATE_READY);
			break;
		case CMD_GET_MOUSE_ID:
			tx_put(mouse_get_device_id());
			break;
		case CMD_GET_VER1:
			tx_put(SMC_VERSION_MAJOR);
			break;
		case CMD_GET_VER2:
			tx_put(SMC_VERSION_MINOR);
			break;
		case CMD_GET_VER3:
			tx_put(SMC_VERSION_PATCH);
			break;
		case CMD_GET_BOOTLDR_VER:
			tx_put(0xff);     // no bootloader signature at $1E00 or $1FFE
			break;
		case CMD_SELF_PROGRAMMING:
			tx_put(0);
			break;
	}
	cmd[0] = default_request;
}

static void
smc_receive(void)
{
	const int ct = rx_len < 3 ? rx_len : 3;
	for (int i = 0; i < ct; i++) {
		cmd[i] = rx[i];
	}
	if (ct < 2) {
		return;
	}
	switch (cmd[0]) {
		case CMD_POW_OFF:
			if (cmd[1] == 0) {
				printf("SMC Power Off.\n");
				main_shutdown();
#ifdef __EMSCRIPTEN__
				emscripten_force_exit(0);
#endif
				exit(0);
			} else if (cmd[1] == 1) {
				smc_requested_reset = true;
			}
			break;
		case CMD_RESET:
			if (cmd[1] == 0) {
				smc_requested_reset = true;
			}
			break;
		case CMD_NMI:
			if (cmd[1] == 0) {
				nmi6502();
			}
			break;
		case CMD_SET_ACT_LED:
			activity_led = (cmd[1] & 0x80) ? 255 : 0;
			break;
		case CMD_ECHO:
			echo_byte = cmd[1];
			break;
		case CMD_KBD_CMD1:
			kbd_status = PS2_CMD_ACK;
			break;
		case CMD_KBD_CMD2:
			if (ct >= 3) {
				kbd_status = PS2_CMD_ACK;
			}
			break;
		case CMD_SET_MOUSE_ID:
			mouse_set_device_id(cmd[1]);
			break;
		case CMD_SET_DFLT_READ_OP:
			default_request = cmd[1];
			break;
	}
	cmd[0] = default_request;
}

bool
smc_i2c_start(bool read)
{
	master_read = read;
	rx_len = 0;
	tx_len = tx_idx = 0;
	if (read) {
		smc_send();
		return tx_len > 0;
	}
	return true;
}

bool
smc_i2c_write(uint8_t v)
{
	if (rx_len < BUFSIZE) {
		rx[rx_len++] = v;
		return true;
	}
	return false;
}

uint8_t
smc_i2c_read(void)
{
	return tx_idx < tx_len ? tx[tx_idx++] : 0xff;
}

void
smc_i2c_stop(void)
{
	if (!master_read && rx_len > 0) {
		smc_receive();
	}
	rx_len = 0;
	tx_len = tx_idx = 0;
}

// x16-smc.ino DoReset(); the first call is power-on, which does not clear the
// long-press flag.
void
smc_reset(void)
{
	if (powered) {
		pwr_long_press = false;
	}
	powered = true;
	activity_led = 0;
	default_request = CMD_GET_KEYCODE_FAST;
	rx_len = 0;
	tx_len = tx_idx = 0;
}

// Side-effect-free snapshot of SMC state for the ImGui debugger.
void
smc_debug_get_state(smc_debug_state_t *out)
{
	memset(out, 0, sizeof *out);
	out->default_read_op  = default_request;
	out->activity_led     = activity_led;
	out->i2c_data_pos     = rx_len;
	out->mse_count        = tx_idx;
	out->requested_reset  = smc_requested_reset;
	for (int i = 0; i < SMC_I2C_DATA_LEN && i < BUFSIZE; i++) {
		out->i2c_data[i] = rx[i];
	}
	out->kbd_fill = i2c_kbd_buffer_count();
	out->mse_fill = i2c_mse_buffer_count();
}


// ─── Save states ─────────────────────────────────────────────────────────────
//
// Version 2 held a per-byte command buffer; it loads with no transaction open.

void
smc_state_save(ss_writer *w)
{
	ss_begin(w, "SMC ", 3);
	ss_put_u8(w, default_request);
	ss_put_u8(w, activity_led);
	ss_put_bool(w, smc_requested_reset);
	ss_put_bool(w, pwr_long_press);
	ss_put(w, cmd, sizeof cmd);
	ss_put_u8(w, echo_byte);
	ss_put_u8(w, kbd_status);
	ss_put_bool(w, powered);
	ss_put_bool(w, master_read);
	ss_put_u8(w, rx_len);
	ss_put(w, rx, sizeof rx);
	ss_put_u8(w, tx_len);
	ss_put_u8(w, tx_idx);
	ss_put(w, tx, sizeof tx);
	ss_end(w);
}

void
smc_state_load(ss_reader *r)
{
	const int version = ss_open(r, "SMC ");
	if (version < 3) {
		uint8_t old[16];
		default_request     = ss_get_u8(r);
		(void)ss_get_u8(r);                 // default_read_state
		activity_led        = ss_get_u8(r);
		(void)ss_get_u8(r);                 // mse_count
		ss_get(r, old, sizeof old);
		(void)ss_get_u8(r);                 // i2c_data_pos
		smc_requested_reset = ss_get_bool(r);
		pwr_long_press      = ss_get_bool(r);
		cmd[0] = old[0];
		cmd[1] = old[1];
		cmd[2] = old[2];
		echo_byte   = 0;
		kbd_status  = PS2_CMD_ACK;
		powered     = true;
		master_read = false;
		rx_len = tx_len = tx_idx = 0;
	} else {
		default_request     = ss_get_u8(r);
		activity_led        = ss_get_u8(r);
		smc_requested_reset = ss_get_bool(r);
		pwr_long_press      = ss_get_bool(r);
		ss_get(r, cmd, sizeof cmd);
		echo_byte   = ss_get_u8(r);
		kbd_status  = ss_get_u8(r);
		powered     = ss_get_bool(r);
		master_read = ss_get_bool(r);
		rx_len      = ss_get_u8(r);
		ss_get(r, rx, sizeof rx);
		tx_len      = ss_get_u8(r);
		tx_idx      = ss_get_u8(r);
		ss_get(r, tx, sizeof tx);
		if (rx_len > BUFSIZE) rx_len = BUFSIZE;
		if (tx_len > BUFSIZE) tx_len = BUFSIZE;
		if (tx_idx > tx_len) tx_idx = tx_len;
	}
	ss_close(r);
}
