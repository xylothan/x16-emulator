#include "joystick.h"

#include <SDL.h>
#include <stdio.h>
#include "io_trace.h"

struct joystick_info {
	int                 instance_id;
	SDL_GameController *controller;
	uint16_t            button_mask;
};

static const uint16_t button_map[SDL_CONTROLLER_BUTTON_MAX] = {
    1 << 0,  //SDL_CONTROLLER_BUTTON_A,
    1 << 8,  //SDL_CONTROLLER_BUTTON_B,
    1 << 1,  //SDL_CONTROLLER_BUTTON_X,
    1 << 9,  //SDL_CONTROLLER_BUTTON_Y,
    1 << 2,  //SDL_CONTROLLER_BUTTON_BACK,
    0,       //SDL_CONTROLLER_BUTTON_GUIDE,
    1 << 3,  //SDL_CONTROLLER_BUTTON_START,
    0,       //SDL_CONTROLLER_BUTTON_LEFTSTICK,
    0,       //SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    1 << 10, //SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    1 << 11, //SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    1 << 4,  //SDL_CONTROLLER_BUTTON_DPAD_UP,
    1 << 5,  //SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    1 << 6,  //SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    1 << 7,  //SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
};

static struct joystick_info *Joystick_controllers     = NULL;
static int                   Num_joystick_controllers = 0;

static void
resize_joystick_controllers(int new_size)
{
	if (new_size == 0) {
		free(Joystick_controllers);
		Joystick_controllers     = NULL;
		Num_joystick_controllers = 0;
		return;
	}

	struct joystick_info *old_controllers = Joystick_controllers;
	Joystick_controllers                  = (struct joystick_info *)malloc(sizeof(struct joystick_info) * new_size);

	int min_size = new_size < Num_joystick_controllers ? new_size : Num_joystick_controllers;
	if (min_size > 0) {
		memcpy(Joystick_controllers, old_controllers, sizeof(struct joystick_info) * min_size);
		free(old_controllers);
	}

	for (int i = min_size; i < new_size; ++i) {
		Joystick_controllers[i].instance_id = -1;
		Joystick_controllers[i].controller  = NULL;
		Joystick_controllers[i].button_mask = 0xffff;
	}
}

static void
add_joystick_controller(struct joystick_info *info)
{
	int i;
	for (i = 0; i < Num_joystick_controllers; ++i) {
		if (Joystick_controllers[i].instance_id == -1) {
			memcpy(&Joystick_controllers[i], info, sizeof(struct joystick_info));
			return;
		}
	}

	i = Num_joystick_controllers;
	resize_joystick_controllers(Num_joystick_controllers << 1);

	memcpy(&Joystick_controllers[i], info, sizeof(struct joystick_info));
}

static void
remove_joystick_controller(int instance_id)
{
	for (int i = 0; i < Num_joystick_controllers; ++i) {
		if (Joystick_controllers[i].instance_id == instance_id) {
			Joystick_controllers[i].instance_id = -1;
			Joystick_controllers[i].controller  = NULL;
			return;
		}
	}
}

static struct joystick_info *
find_joystick_controller(int instance_id)
{
	for (int i = 0; i < Num_joystick_controllers; ++i) {
		if (Joystick_controllers[i].instance_id == instance_id) {
			return &Joystick_controllers[i];
		}
	}

	return NULL;
}

bool Joystick_slots_enabled[NUM_JOYSTICKS] = {false, false, false, false};
// -1 is "no controller bound". Statically initialised because zero would mean
// instance id 0 is bound to every slot, which is what a caller reaching the
// joystick state before joystick_init() runs would otherwise see.
static int Joystick_slots[NUM_JOYSTICKS] = {-1, -1, -1, -1};

// A virtual joystick needs no SDL controller, so automation can drive a port on
// a machine with no gamepad attached. Active-low, same layout as button_mask.
static uint16_t Joystick_virtual_mask[NUM_JOYSTICKS]    = {0xffff, 0xffff, 0xffff, 0xffff};
static bool     Joystick_virtual_enabled[NUM_JOYSTICKS] = {false, false, false, false};

// The shift register is per slot rather than per controller: a virtual joystick
// has no controller to hang one off, and a slot is what the hardware clocks.
static uint16_t Joystick_slot_shift[NUM_JOYSTICKS] = {0xffff, 0xffff, 0xffff, 0xffff};

static bool Joystick_latch = false;
uint8_t Joystick_data  = 0;

// Whether anything at all is driving this slot. A slot with no source keeps the
// original behaviour of reporting 1s (released) forever rather than clocking a
// shift register, so no existing configuration changes behaviour.
static bool
slot_has_source(int slot)
{
	return Joystick_virtual_enabled[slot] || Joystick_slots[slot] >= 0;
}

// The button state the slot should latch: the virtual joystick when one is
// active, otherwise the controller bound to the slot.
static uint16_t
slot_button_mask(int slot)
{
	if (Joystick_virtual_enabled[slot]) {
		return Joystick_virtual_mask[slot];
	}

	if (Joystick_slots[slot] >= 0) {
		struct joystick_info *joy = find_joystick_controller(Joystick_slots[slot]);
		if (joy != NULL) {
			return joy->button_mask;
		}
	}

	return 0xffff;
}

bool
joystick_init(void)
{
	for (int i = 0; i < NUM_JOYSTICKS; ++i) {
		Joystick_slots[i] = -1;
	}

	const int num_joysticks = SDL_NumJoysticks();

	Num_joystick_controllers = num_joysticks > 16 ? num_joysticks : 16;
	Joystick_controllers     = malloc(sizeof(struct joystick_info) * Num_joystick_controllers);

	for (int i = 0; i < Num_joystick_controllers; ++i) {
		Joystick_controllers[i].instance_id = -1;
		Joystick_controllers[i].controller  = NULL;
		Joystick_controllers[i].button_mask = 0xffff;
	}

	for (int i = 0; i < num_joysticks; ++i) {
		joystick_add(i);
	}

	return true;
}

void
joystick_add(int index)
{
	if (!SDL_IsGameController(index)) {
		return;
	}

	SDL_GameController *controller = SDL_GameControllerOpen(index);
	if (controller == NULL) {
		fprintf(stderr, "Could not open controller %d: %s\n", index, SDL_GetError());
		return;
	}

	SDL_JoystickID instance_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
	bool           exists      = false;
	for (int i = 0; i < NUM_JOYSTICKS; ++i) {
		if (!Joystick_slots_enabled[i]) {
			continue;
		}

		if (Joystick_slots[i] == instance_id) {
			exists = true;
			break;
		}
	}

	if (!exists) {
		for (int i = 0; i < NUM_JOYSTICKS; ++i) {
			if (!Joystick_slots_enabled[i]) {
				continue;
			}

			if (Joystick_slots[i] == -1) {
				Joystick_slots[i] = instance_id;
				break;
			}
		}

		struct joystick_info new_info;
		new_info.instance_id = instance_id;
		new_info.controller  = controller;
		new_info.button_mask = 0xffff;
		add_joystick_controller(&new_info);
	}
}

void
joystick_remove(int instance_id)
{
	for (int i = 0; i < NUM_JOYSTICKS; ++i) {
		if (Joystick_slots[i] == instance_id) {
			Joystick_slots[i] = -1;
			break;
		}
	}

	SDL_GameController *controller = SDL_GameControllerFromInstanceID(instance_id);
	if (controller == NULL) {
		fprintf(stderr, "Could not find controller from instance_id %d: %s\n", instance_id, SDL_GetError());
	} else {
		SDL_GameControllerClose(controller);
		remove_joystick_controller(instance_id);
	}
}

void
joystick_button_down(int instance_id, uint8_t button)
{
	struct joystick_info *joy = find_joystick_controller(instance_id);
	if (joy != NULL) {
		joy->button_mask &= ~(button_map[button]);
	}
}

void
joystick_button_up(int instance_id, uint8_t button)
{
	struct joystick_info *joy = find_joystick_controller(instance_id);
	if (joy != NULL) {
		joy->button_mask |= button_map[button];
	}
}

static void
do_shift()
{
	for (int i = 0; i < NUM_JOYSTICKS; ++i) {
		if (slot_has_source(i)) {
			Joystick_data |= ((Joystick_slot_shift[i] & 1) ? (0x80 >> i) : 0);
			Joystick_slot_shift[i] >>= 1;
		} else {
			Joystick_data |= 0x80 >> i;
		}
	}
}

void
joystick_set_latch(bool value)
{
	Joystick_latch = value;
	if (value) {
		for (int i = 0; i < NUM_JOYSTICKS; ++i) {
			Joystick_slot_shift[i] = slot_button_mask(i) | 0xF000;
		}
		do_shift();
		if (io_trace_wants(IO_DEV_JOYSTICK)) {
			// Initialised because no slot is enabled unless -joy1..-joy4 was
			// passed, which is the default. The loop then writes nothing and
			// the %s below would otherwise walk uninitialised stack.
			char buf[64] = "";
			int  off = 0;
			for (int i = 0; i < NUM_JOYSTICKS && off < (int)sizeof(buf) - 10; ++i) {
				if (!Joystick_slots_enabled[i]) continue;
				off += snprintf(buf + off, sizeof(buf) - off, " j%d=%04X", i, slot_button_mask(i));
			}
			io_trace_event(IO_DEV_JOYSTICK, "latch%s", buf);
		}
	}
}

void
joystick_set_clock(bool value)
{
	if (!Joystick_latch && value) {
		Joystick_data = 0;
		do_shift();
	}
}

// ─── Virtual joysticks ───────────────────────────────────────────────────────

// Enabling the slot here is what lets automation drive a port without the
// -joy1..-joy4 flags, which exist to decide where a *physical* pad gets bound.
// A caller that has explicitly asked for input on this port has answered that
// question, and requiring the flag as well would only fail confusingly.
void
joystick_set_virtual(int slot, uint16_t button_mask)
{
	if (slot < 0 || slot >= NUM_JOYSTICKS) {
		return;
	}

	Joystick_virtual_mask[slot]    = button_mask;
	Joystick_virtual_enabled[slot] = true;
	Joystick_slots_enabled[slot]   = true;
}

// Leaves Joystick_slots_enabled alone: that is a user-visible configuration bit,
// and a physical pad may bind to the slot now that the virtual one has let go.
void
joystick_clear_virtual(int slot)
{
	if (slot < 0 || slot >= NUM_JOYSTICKS) {
		return;
	}

	Joystick_virtual_mask[slot]    = 0xffff;
	Joystick_virtual_enabled[slot] = false;
}

bool
joystick_virtual_active(int slot)
{
	if (slot < 0 || slot >= NUM_JOYSTICKS) {
		return false;
	}

	return Joystick_virtual_enabled[slot];
}

// Side-effect-free snapshot of joystick state for the ImGui debugger.
void
joystick_debug_get_state(joystick_debug_state_t *out)
{
	for (int i = 0; i < NUM_JOYSTICKS; ++i) {
		joystick_slot_debug_t *s = &out->slots[i];
		s->enabled          = Joystick_slots_enabled[i];
		s->virtual_active   = Joystick_virtual_enabled[i];
		s->controller_bound = Joystick_slots[i] >= 0 && find_joystick_controller(Joystick_slots[i]) != NULL;
		s->button_mask      = slot_button_mask(i);
		// A slot with no source clocks out 1s rather than draining a shift
		// register, so report that instead of a stale value it never uses.
		s->shift_mask = slot_has_source(i) ? Joystick_slot_shift[i] : 0xffff;
	}
	out->latch = Joystick_latch;
	out->data  = Joystick_data;
}
