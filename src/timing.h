#ifndef TIMING_H
#define TIMING_H

#include <stdint.h>

void timing_init();
void timing_update();
void timing_update_no_sleep();
int64_t timing_lead_us();

// Emulation speed, as an absolute target clock in kHz rather than a percentage.
//
// The machine's clock is whatever -mhz selected (default 8, range 1..40), so a
// percentage on its own says nothing without knowing what it is a percentage
// of: "50%" is a different absolute speed on a machine started with -mhz 1 than
// on one started with -mhz 12. kHz is unambiguous, and does not lose precision
// at the very low speeds that are useful for watching a routine execute.
//
// Warp mode bypasses throttling entirely and ignores all of this.
#define TIMING_SPEED_MIN_KHZ 25      // 0.025 MHz - slow enough to follow by eye
#define TIMING_SPEED_MAX_KHZ 100000  // 100 MHz - well past any real hardware

int  timing_native_khz(void);        // the machine's own clock, from -mhz
int  timing_get_speed_khz(void);
void timing_set_speed_khz(int khz);  // clamped to the range above
void timing_reset_speed(void);       // back to the machine's own clock

// Step to the next/previous preset clock (direction > 0 faster, < 0 slower).
// The presets are anchored on speeds real hardware runs at, and always include
// the machine's own, so normal speed is reachable however it was started.
void timing_step_speed(int direction);

// The target as a percentage of the machine's clock, for display.
int  timing_get_speed_percent(void);

#endif
