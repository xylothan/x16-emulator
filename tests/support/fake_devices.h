// Recording stand-ins for the devices behind the I/O page, so memory.c can be
// linked and exercised without SDL, a ROM, or any real hardware model.
// The debugger-contract seam: a debug read moves no device state, spends no cycles.
// Each fake records its calls; a real read advances `state`, a debug read must not.

#ifndef X16_TEST_FAKE_DEVICES_H
#define X16_TEST_FAKE_DEVICES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	FAKE_VIA1,
	FAKE_VIA2,
	FAKE_VERA,
	FAKE_YM,
	FAKE_MIDI,
	FAKE_CART,
	FAKE_DEV_COUNT,
} fake_dev_t;

typedef struct {
	int     reads;        // every read, debug or not
	int     debug_reads;  // those that arrived with the flag set
	int     writes;
	uint8_t last_reg;
	uint8_t last_value;

	// Advanced by a real read only. A debug read must leave it alone.
	uint8_t state;

	// VIAs and VERA only: the cycle offset of each catch-up, in order.
	int      catch_ups;
	unsigned offsets[8];
} fake_dev_log_t;

extern fake_dev_log_t fake_dev[FAKE_DEV_COUNT];

const char *fake_dev_name(fake_dev_t dev);

// Clear every log and every device's state. Also zeroes the CPU cycle counter,
// since "did this read cost anything?" is half of the contract.
void fake_devices_reset(void);

// Total device state across all fakes, for asserting that a sweep of the I/O
// page moved nothing.
uint32_t fake_devices_state_sum(void);

// Reads and writes seen by every fake since the last reset.
int fake_devices_total_reads(void);
int fake_devices_total_writes(void);

// YM_read_status() takes no debug flag, so the YM fake always advances.
#define FAKE_YM_IS_DEBUG_BLIND 1

#endif // X16_TEST_FAKE_DEVICES_H
