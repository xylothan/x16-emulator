// The I2C bus and its two targets, driven bit by bit.
//
// ORACLES:
//   bus  MCP7940N datasheet DS20005010J Section 4.1.1
//   RTC  DS20005010J Table 5-1, Registers 5-1 to 5-19, Sections 5.3, 5.4, 6.1
//   SMC  x16-smc r48.0.0 (0a2684d): x16-smc.ino I2C_Receive(), I2C_Send(),
//        DoReset(), sendMousePacket(); smc_wire.cpp; version.h

#include "../support/harness.h"
#include "../support/i2c_fixture.h"

#include <stdint.h>

#define SMC 0x42
#define RTC 0x6f

static bool
write_regs(uint8_t dev, const uint8_t *b, int n)
{
	i2c_bus_start();
	bool ok = i2c_bus_send((uint8_t)(dev << 1));
	for (int i = 0; ok && i < n; i++) {
		ok = i2c_bus_send(b[i]);
	}
	i2c_bus_stop();
	return ok;
}

static void
write2(uint8_t dev, uint8_t a, uint8_t v)
{
	const uint8_t b[2] = { a, v };
	write_regs(dev, b, 2);
}

// Random read, Section 6.1.4: the address, a repeated START, then n bytes.
static void
read_at(uint8_t dev, uint8_t a, uint8_t *out, int n)
{
	i2c_bus_start();
	i2c_bus_send((uint8_t)(dev << 1));
	i2c_bus_send(a);
	i2c_bus_start();
	i2c_bus_send((uint8_t)(dev << 1 | 1));
	for (int i = 0; i < n; i++) {
		out[i] = i2c_bus_recv(i < n - 1);
	}
	i2c_bus_stop();
}

static uint8_t
rtc_get(uint8_t a)
{
	uint8_t v;
	read_at(RTC, a, &v, 1);
	return v;
}

// A read with no command byte first; false if the address is not acknowledged.
static bool
direct_read(uint8_t dev, uint8_t *out, int n)
{
	i2c_bus_start();
	const bool ack = i2c_bus_send((uint8_t)(dev << 1 | 1));
	for (int i = 0; i < n; i++) {
		out[i] = i2c_bus_recv(i < n - 1);
	}
	i2c_bus_stop();
	return ack;
}

static uint8_t
smc_get(uint8_t cmd)
{
	uint8_t v;
	read_at(SMC, cmd, &v, 1);
	return v;
}

// DoReset() leaves I2C_Data[0]; the KERNAL sets the default request at start.
static void
fresh(void)
{
	rtc_init(false);
	i2c_fixture_reset();
	write2(SMC, 0x40, 0x41);
}

// ─── Bus: Section 4.1.1 ─────────────────────────────────────────────────────

static void
test_unknown_address_is_not_acknowledged(void)
{
	fresh();
	i2c_bus_start();
	check(!i2c_bus_send(0x50 << 1), "4.1.1.5: no target at $50");
	check(!i2c_bus_send(0x00), "4.1.1.5: nothing is acknowledged until STOP");
	i2c_bus_stop();
	check_eq(rtc_get(0x03), 0x01, "4.1.1.3: STOP ends it; the next transaction is answered");
}

static void
test_repeated_start_begins_a_transaction(void)
{
	fresh();
	write2(RTC, 0x20, 0x5a);
	i2c_bus_start();
	i2c_bus_send(0x50 << 1);
	i2c_bus_start();
	check(i2c_bus_send(RTC << 1), "4.1.1.2: a START while addressed elsewhere");
	i2c_bus_send(0x20);
	i2c_bus_start();
	check(i2c_bus_send(RTC << 1 | 1), "4.1.1.2: a START after the address byte");
	check_eq(i2c_bus_recv(false), 0x5a, "6.1.4: the repeated START keeps the pointer");
	i2c_bus_stop();
}

static void
test_stop_inside_a_byte(void)
{
	fresh();
	i2c_bus_start();
	i2c_bus_send(SMC << 1);
	i2c_bus_send(0x03);
	i2c_bus_send(0x00);
	i2c_port.clk_in = 0; i2c_port.data_in = 0; i2c_step();
	i2c_port.clk_in = 1; i2c_step();
	i2c_port.clk_in = 0; i2c_step();
	i2c_bus_stop();
	check_eq((uint32_t)i2c_fixture_nmis, 1, "4.1.1.3: STOP partway through a byte ends the transaction");
	check_eq(rtc_get(0x07), 0x80, "4.1.1.3: and the bus is free");
}

// ─── RTC: Section 6.1 ───────────────────────────────────────────────────────

static void
test_rtc_sequential_read_wraps_sram(void)
{
	fresh();
	const uint8_t w[] = { 0x5e, 0xa1, 0xa2 };
	write_regs(RTC, w, 3);
	write2(RTC, 0x20, 0xa3);
	write2(RTC, 0x21, 0xa4);
	uint8_t r[4];
	read_at(RTC, 0x5e, r, 4);
	check_eq(r[0], 0xa1, "6.1.2: sequential write, first byte at the address");
	check_eq(r[1], 0xa2, "6.1.2: sequential write, then the next address");
	check_eq(r[2], 0xa3, "6.1.5: 0x5F rolls over to 0x20");
	check_eq(r[3], 0xa4, "6.1.5: then 0x21");
}

static void
test_rtc_sequential_write_wraps_rtcc(void)
{
	fresh();
	const uint8_t w[] = { 0x1f, 0x93, 0x42 };
	write_regs(RTC, w, 3);
	check_eq(rtc_get(0x1f), 0x93, "6.1.2: PWRUPMTH written");
	check_eq(rtc_get(0x00), 0x42, "6.1.2: 0x1F rolls over to 0x00");
}

static void
test_rtc_current_address_read(void)
{
	fresh();
	write2(RTC, 0x24, 0x11);
	write2(RTC, 0x25, 0x22);
	check_eq(rtc_get(0x24), 0x11, "6.1.4: random read of 0x24");
	uint8_t v;
	direct_read(RTC, &v, 1);
	check_eq(v, 0x22, "6.1.3: a current address read gives n + 1");
}

static void
test_rtc_address_past_sram(void)
{
	fresh();
	i2c_bus_start();
	check(i2c_bus_send(RTC << 1), "6.1.1: control byte acknowledged");
	check(!i2c_bus_send(0x60), "6.1.1: address 0x60 is not acknowledged");
	check(!i2c_bus_send(0x12), "6.1.1: nor its data");
	i2c_bus_stop();
	check_eq(rtc_get(0x00), 0x00, "6.1.1: and nothing is written");
}

// ─── RTC registers: Table 5-1, Registers 5-1 to 5-15 ────────────────────────

static void
test_rtc_reset_values(void)
{
	fresh();
	static const uint8_t want[] = { 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x80, 0x00 };
	uint8_t r[9];
	read_at(RTC, 0x00, r, 9);
	for (int i = 0; i < 9; i++) {
		check_eq(r[i], want[i], "Registers 5-1 to 5-15: value at POR");
	}
	check_eq(rtc_get(0x0d), 0x01, "Register 5-11: ALM0WKDAY at POR");
	check_eq(rtc_get(0x15), 0x01, "Register 5-12: ALM1DATE at POR");
}

static void
test_rtc_wkday_bits(void)
{
	fresh();
	write2(RTC, 0x03, 0x0b);
	check_eq(rtc_get(0x03), 0x0b, "Register 5-4: VBATEN is bit 3");
	write2(RTC, 0x03, 0xff);
	check_eq(rtc_get(0x03), 0x0f, "Register 5-4: OSCRUN read-only, PWRFAIL cannot be set");
	write2(RTC, 0x00, 0x80);
	check_eq(rtc_get(0x03) & 0x20, 0x20, "Register 5-4: OSCRUN with ST set");
}

static void
test_rtc_unimplemented_bits(void)
{
	fresh();
	write2(RTC, 0x01, 0xff);
	check_eq(rtc_get(0x01), 0x7f, "Register 5-2: bit 7 reads 0");
	write2(RTC, 0x04, 0xff);
	check_eq(rtc_get(0x04), 0x3f, "Register 5-5: bits 7-6 read 0");
	write2(RTC, 0x05, 0xff);
	check_eq(rtc_get(0x05), 0x1f, "Register 5-6: bits 7-6 read 0, LPYR read-only");
	write2(RTC, 0x09, 0xff);
	check_eq(rtc_get(0x09), 0x00, "Table 5-1: 0x09 reserved");
}

static void
test_rtc_read_only_mirrors(void)
{
	fresh();
	write2(RTC, 0x06, 0x24);
	check_eq(rtc_get(0x05) & 0x20, 0x20, "Register 5-6: LPYR for 2024");
	write2(RTC, 0x06, 0x25);
	check_eq(rtc_get(0x05) & 0x20, 0x00, "Register 5-6: no LPYR for 2025");
	write2(RTC, 0x02, 0x52);
	check_eq(rtc_get(0x0c), 0x40, "Register 5-10: ALMxHOUR 12/24 mirrors RTCHOUR");
	write2(RTC, 0x0d, 0x80);
	check_eq(rtc_get(0x14) & 0x80, 0x80, "Table 5-1 note 3: ALM1WKDAY ALMPOL mirrors ALM0");
}

// ─── RTC timekeeping and alarms: Sections 5.3, 5.4 ──────────────────────────

static void
seconds(int n)
{
	for (int i = 0; i < n; i++) {
		rtc_step(8000000);
	}
}

static void
test_rtc_twelve_hour_rollover(void)
{
	fresh();
	const uint8_t t[] = { 0x00, 0x80 | 0x59, 0x59, 0x40 | 0x20 | 0x11, 0x03, 0x28, 0x02, 0x24 };
	write_regs(RTC, t, 8);
	seconds(1);
	uint8_t r[7];
	read_at(RTC, 0x00, r, 7);
	check_eq(r[0], 0x80, "5.3: seconds roll over");
	check_eq(r[2], 0x52, "Register 5-3: 11:59:59 PM to 12 AM");
	check_eq(r[3] & 7, 4, "5.3: weekday advances");
	check_eq(r[4], 0x29, "5.3: 28 February 2024 to the 29th");
}

static void
test_rtc_month_lengths(void)
{
	fresh();
	const uint8_t t[] = { 0x00, 0x80 | 0x59, 0x59, 0x23, 0x01, 0x28, 0x02, 0x25 };
	write_regs(RTC, t, 8);
	seconds(1);
	check_eq(rtc_get(0x04), 0x01, "5.3: 28 February 2025 to 1 March");
	check_eq(rtc_get(0x05), 0x03, "5.3: month advances");
}

static void
test_rtc_stopped_oscillator(void)
{
	fresh();
	write2(RTC, 0x00, 0x30);
	seconds(3);
	check_eq(rtc_get(0x00), 0x30, "Register 5-1: ST clear, time does not advance");
}

static void
test_rtc_alarm_flag(void)
{
	fresh();
	write2(RTC, 0x0a, 0x05);
	write2(RTC, 0x0d, 0x00);
	write2(RTC, 0x00, 0x80 | 0x04);
	seconds(1);
	check_eq(rtc_get(0x0d) & 0x08, 0x00, "5.4: no flag while ALM0EN is clear");
	write2(RTC, 0x00, 0x80 | 0x04);
	write2(RTC, 0x07, 0x10);
	seconds(1);
	check_eq(rtc_get(0x0d) & 0x08, 0x08, "Table 5-5: seconds match sets ALM0IF");
	write2(RTC, 0x0d, 0x00);
	check_eq(rtc_get(0x0d) & 0x08, 0x08, "5.4 note 1: cleared while matching, set again");
	seconds(1);
	write2(RTC, 0x0d, 0x00);
	check_eq(rtc_get(0x0d) & 0x08, 0x00, "Register 5-11: writing ALMxWKDAY clears ALMxIF");
}

// ─── SMC: x16-smc.ino and smc_wire.cpp ──────────────────────────────────────

static void
test_smc_write_acts_at_stop(void)
{
	fresh();
	i2c_bus_start();
	i2c_bus_send(SMC << 1);
	i2c_bus_send(0x03);
	i2c_bus_send(0x00);
	check_eq((uint32_t)i2c_fixture_nmis, 0, "smc_wire.cpp: the write is held until STOP or START");
	i2c_bus_stop();
	check_eq((uint32_t)i2c_fixture_nmis, 1, "I2C_Receive: NMI at STOP");
}

static void
test_smc_first_three_bytes(void)
{
	fresh();
	const uint8_t w[] = { 0x40, 0x07, 0x40, 0x41 };
	write_regs(SMC, w, 4);
	uint8_t v = 0xee;
	check(direct_read(SMC, &v, 1), "I2C_Receive: only the first three bytes count");
	check_eq(v, 0x00, "I2C_Send: GET_KEYCODE with nothing queued sends 0");
}

static void
test_smc_command_byte_selects_read(void)
{
	fresh();
	const uint8_t w[] = { 0x30 };
	write_regs(SMC, w, 1);
	uint8_t v;
	check(direct_read(SMC, &v, 1), "I2C_Receive: one byte leaves the command for the read");
	check_eq(v, 48, "version.h: major version");
	check(!direct_read(SMC, &v, 1), "I2C_Send: back to the default request afterwards");
}

static void
test_smc_empty_fast_read(void)
{
	fresh();
	uint8_t v;
	check(!direct_read(SMC, &v, 1), "smc_wire.cpp: an empty reply is not acknowledged");
	check_eq(v, 0xff, "smc_wire.cpp: the bus is left released");
	i2c_kbd_buffer_add(0x1c);
	check(direct_read(SMC, &v, 1), "I2C_Send: GET_KEYCODE_FAST with a key");
	check_eq(v, 0x1c, "sendKeyCode: the key");
}

static void
test_smc_reply_past_its_end(void)
{
	fresh();
	uint8_t r[2];
	read_at(SMC, 0x31, r, 2);
	check_eq(r[0], 0, "version.h: minor version");
	check_eq(r[1], 0xff, "smc_wire.cpp: past the reply, $FF");
}

static void
add_packet(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	i2c_mse_buffer_add(a);
	i2c_mse_buffer_add(b);
	i2c_mse_buffer_add(c);
	i2c_mse_buffer_add(d);
}

static void
test_smc_ps2data(void)
{
	fresh();
	write2(SMC, 0x40, 0x43);
	add_packet(0x09, 0x01, 0x02, 0x03);
	uint8_t r[5];
	check(direct_read(SMC, r, 5), "I2C_Send: GET_PS2DATA_FAST with a mouse packet");
	check_eq(r[0], 0x00, "sendKeyCode: no key, 0");
	check_eq(r[1], 0x09, "sendMousePacket: the packet");
	check_eq(r[4], 0x03, "sendMousePacket: four bytes for mouse ID 3");
	i2c_kbd_buffer_add(0x12);
	check(direct_read(SMC, r, 2), "I2C_Send: GET_PS2DATA_FAST with a key");
	check_eq(r[0], 0x12, "sendKeyCode: the key");
	check_eq(r[1], 0x00, "sendMousePacket: no packet, 0");
}

static void
test_smc_mouse_packet_checks(void)
{
	fresh();
	i2c_mse_buffer_add(0x00);
	add_packet(0x08, 0x01, 0x02, 0x03);
	check_eq(smc_get(0x21), 0x00, "sendMousePacket: bit 3 clear, one byte dropped");
	uint8_t r[4];
	read_at(SMC, 0x21, r, 4);
	check_eq(r[0], 0x08, "sendMousePacket: then the packet");
	check_eq(r[3], 0x03, "sendMousePacket: whole");
	add_packet(0xc8, 0x01, 0x02, 0x03);
	add_packet(0x09, 0x04, 0x05, 0x06);
	check_eq(smc_get(0x21), 0x00, "sendMousePacket: an overflow packet is dropped");
	read_at(SMC, 0x21, r, 4);
	check_eq(r[1], 0x04, "sendMousePacket: whole, then the next");
}

static void
test_smc_echo_and_status(void)
{
	fresh();
	write2(SMC, 0x08, 0xa5);
	check_eq(smc_get(0x08), 0xa5, "I2C_CMD_ECHO");
	check_eq(smc_get(0x18), 0xfa, "ps2.h CMD_ACK: the keyboard acknowledged");
	check_eq(smc_get(0x1b), 0x01, "setup_ps2.h KBD_STATE_READY");
	check_eq(smc_get(0x32), 0, "version.h: patch version");
}

static void
test_smc_activity_led(void)
{
	fresh();
	write2(SMC, 0x05, 0x7f);
	check_eq(activity_led, 0, "I2C_CMD_SET_ACT_LED: bit 7 clear, off");
	write2(SMC, 0x05, 0x80);
	check_eq(activity_led, 255, "I2C_CMD_SET_ACT_LED: bit 7 set, on");
}

static void
test_smc_reset(void)
{
	fresh();
	pwr_long_press = true;
	check_eq(smc_get(0x09), 1, "I2C_Send: LONGPRESS_START");
	check_eq(smc_get(0x09), 1, "I2C_Send: reading does not clear it");
	write2(SMC, 0x40, 0x07);
	write2(SMC, 0x05, 0xff);
	smc_reset();
	check_eq(smc_get(0x09), 0, "DoReset: clears the long press");
	check_eq(activity_led, 0, "DoReset: activity LED off");
	uint8_t v;
	check(!direct_read(SMC, &v, 1), "DoReset: default request back to GET_KEYCODE_FAST");
}

int
main(void)
{
	test_unknown_address_is_not_acknowledged();
	test_repeated_start_begins_a_transaction();
	test_stop_inside_a_byte();
	test_rtc_sequential_read_wraps_sram();
	test_rtc_sequential_write_wraps_rtcc();
	test_rtc_current_address_read();
	test_rtc_address_past_sram();
	test_rtc_reset_values();
	test_rtc_wkday_bits();
	test_rtc_unimplemented_bits();
	test_rtc_read_only_mirrors();
	test_rtc_twelve_hour_rollover();
	test_rtc_month_lengths();
	test_rtc_stopped_oscillator();
	test_rtc_alarm_flag();
	test_smc_write_acts_at_stop();
	test_smc_first_three_bytes();
	test_smc_command_byte_selects_read();
	test_smc_empty_fast_read();
	test_smc_reply_past_its_end();
	test_smc_ps2data();
	test_smc_mouse_packet_checks();
	test_smc_echo_and_status();
	test_smc_activity_led();
	test_smc_reset();
	return x16_test_summary("i2c_targets");
}
