// Commander X16 Emulator
// The input log, wired to the emulator's devices.

#include "input_log_host.h"

#include <SDL.h>

#include "debug_server.h"
#include "glue.h"
#include "i2c.h"
#include "input_log.h"
#include "joystick.h"
#include "cpu/fake6502.h"
#include "cpu/irq_ctx.h"

static void
apply_event(const input_event_t *e)
{
	switch ((input_event_kind_t)e->kind) {
		case INPUT_EV_KBD:         i2c_kbd_buffer_add(e->a); break;
		case INPUT_EV_MOUSE_DOWN:  mouse_button_down(e->a); break;
		case INPUT_EV_MOUSE_UP:    mouse_button_up(e->a); break;
		case INPUT_EV_MOUSE_MOVE:  mouse_move(e->x, e->y); break;
		case INPUT_EV_MOUSE_WHEEL: mouse_set_wheel((int8_t)e->x); break;
		case INPUT_EV_MOUSE_SEND:  mouse_send_state(); break;
		case INPUT_EV_JOY_SET:     joystick_set_virtual(e->a, (uint16_t)e->x); break;
		case INPUT_EV_JOY_CLEAR:   joystick_clear_virtual(e->a); break;
		case INPUT_EV_JOY_PHYS:    joystick_replay_host(e->a, e->y != 0, (uint16_t)e->x); break;
		case INPUT_EV_PASTE:       machine_paste_feed(e->a); break;
	}
}

// The debugger's poll clears the entered flag, so it depends on where the hash
// is taken in the loop. It does not affect how the machine runs.
static bool
verify_hash(uint64_t *out)
{
	const bool entered = cpu_irq_just_entered;
	cpu_irq_just_entered = false;
	const bool ok = debug_server_machine_hash(out);
	cpu_irq_just_entered = entered;
	return ok;
}

void
input_log_host_init(void)
{
	static const input_log_hooks_t hooks = {apply_event, verify_hash, joystick_replay_end};
	input_log_init(&clockticks6502, &hooks);
}

bool
input_log_host_start(void)
{
	if (!input_log_start()) {
		return false;
	}
	joystick_log_host_state(true);
	return true;
}
