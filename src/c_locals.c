// Commander X16 Emulator — where a C function's variables are. See c_locals.h.

#include "c_locals.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

#define OPCODE_JSR     0x20
#define OPCODE_JMP_ABS 0x4C
#define OPCODE_LDY_IMM 0xA0
#define OPCODE_LDA_ABS 0xAD
#define OPCODE_LDX_ABS 0xAE
#define MAIN_START_SIZE 23
#define PLAIN_RAM_LOW  0x0002 // $0000 and $0001 are the bank registers
#define PLAIN_RAM_END  0x9F00

const char *
c_local_status_name(c_local_status_t status)
{
	switch (status) {
	case C_LOCAL_OK:               return "ok";
	case C_LOCAL_UNALLOCATED:      return "unallocated";
	case C_LOCAL_NOT_STORED:       return "not-stored";
	case C_LOCAL_UNKNOWN_FRAME:    return "unknown-frame";
	case C_LOCAL_REFUSED_VARIADIC: return "refused-variadic";
	case C_LOCAL_REFUSED_REGISTER: return "refused-register";
	case C_LOCAL_REFUSED_LOCATION: return "refused-location";
	}
	return "unknown-frame";
}

const char *
c_frame_state_name(c_frame_state_t state)
{
	switch (state) {
	case C_FRAME_KNOWN:            return "known";
	case C_FRAME_NOT_SEEN:         return "unknown-frame";
	case C_FRAME_NATIVE:           return "refused-native";
	case C_FRAME_DIRECT_PAGE:      return "refused-direct-page";
	case C_FRAME_BANKED:           return "refused-banked";
	case C_FRAME_NO_STACK_POINTER: return "refused-no-stack-pointer";
	case C_FRAME_PROLOGUE:         return "refused-prologue";
	}
	return "unknown-frame";
}

const char *
c_frame_state_reason(c_frame_state_t state)
{
	switch (state) {
	case C_FRAME_KNOWN:            return "";
	case C_FRAME_NOT_SEEN:         return "this call was not seen entering";
	case C_FRAME_NATIVE:           return "65C816 native mode";
	case C_FRAME_DIRECT_PAGE:      return "the direct page is not zero";
	case C_FRAME_BANKED:           return "banked code";
	case C_FRAME_NO_STACK_POINTER: return "no C stack pointer in the loaded symbols";
	case C_FRAME_PROLOGUE:         return "the function's first call cannot be identified";
	}
	return "";
}

static int
pick_helper(int count, c_frames_symbol_at_fn at, const char *want)
{
	int found = -1;
	for (int i = 0; i < count; i++) {
		const char *name = NULL;
		uint32_t    addr = 0;
		if (!at(i, &name, &addr) || !name || strcmp(name, want) != 0 || addr > 0xFFFF)
			continue;
		if (found >= 0 && (uint32_t)found != addr)
			return -2;
		found = (int)addr;
	}
	return found;
}

void
c_locals_pick_helpers(int count, c_frames_symbol_at_fn at, c_locals_helpers_t *out)
{
	out->pusha   = pick_helper(count, at, "pusha");
	out->pushax  = pick_helper(count, at, "pushax");
	out->pusheax = pick_helper(count, at, "pusheax");
	out->enter   = pick_helper(count, at, "enter");
}

int
c_locals_push_bytes(uint16_t entry, uint8_t (*read)(uint16_t), const c_locals_helpers_t *h,
                    bool (*named)(uint16_t), bool *variadic)
{
	*variadic = false;
	if (read(entry) != OPCODE_JSR)
		return 0;
	if (h->pusha == -2 || h->pushax == -2 || h->pusheax == -2 || h->enter == -2)
		return -1;

	const int target = read((uint16_t)(entry + 1)) | (read((uint16_t)(entry + 2)) << 8);
	if (target == h->pusha)
		return 1;
	if (target == h->pushax)
		return 2;
	if (target == h->pusheax)
		return 4;
	if (target == h->enter) {
		*variadic = true;
		return 1;
	}
	return named((uint16_t)target) ? 0 : -1;
}

static uint16_t
read_word(uint16_t a, uint8_t (*read)(uint16_t))
{
	return (uint16_t)(read(a) | (read((uint16_t)(a + 1)) << 8));
}

// LDA abs / LDX abs+1 / JSR pushax
static bool
is_word_push(uint16_t a, uint8_t (*read)(uint16_t), int pushax)
{
	return read(a) == OPCODE_LDA_ABS && read((uint16_t)(a + 3)) == OPCODE_LDX_ABS &&
	       read((uint16_t)(a + 6)) == OPCODE_JSR &&
	       read_word((uint16_t)(a + 4), read) == (uint16_t)(read_word((uint16_t)(a + 1), read) + 1) &&
	       read_word((uint16_t)(a + 7), read) == (uint16_t)pushax;
}

bool
c_locals_is_main_start(uint16_t start, uint16_t entry, uint8_t (*read)(uint16_t),
                       const c_locals_helpers_t *helpers)
{
	if (helpers->pushax < 0 || start + MAIN_START_SIZE > PLAIN_RAM_END)
		return false;
	return is_word_push(start, read, helpers->pushax) &&
	       is_word_push((uint16_t)(start + 9), read, helpers->pushax) &&
	       read((uint16_t)(start + 18)) == OPCODE_LDY_IMM &&
	       read((uint16_t)(start + 19)) == C_LOCALS_MAIN_START_PUSH &&
	       read((uint16_t)(start + 20)) == OPCODE_JMP_ABS &&
	       read_word((uint16_t)(start + 21), read) == entry;
}

bool
c_locals_entry_pending(bool is_current, uint16_t address, uint16_t entry, int push_bytes)
{
	if (push_bytes <= 0)
		return false;
	return is_current ? address == entry : address == (uint16_t)(entry + 3);
}

bool
c_locals_record_matches(const c_frame_record_t *r, uint16_t slot, uint16_t caller_address,
                        uint16_t entry, uint8_t lo, uint8_t hi)
{
	return r->slot == slot && (uint16_t)(r->ret + 1) == caller_address &&
	       r->ret == (uint16_t)(lo | (hi << 8)) && r->target == entry;
}

// The gap to the next higher offset. 0 when none is usable.
static int
inferred_size(const c_frame_view_t *frame, const dbg_cvar_t *vars, int count, int index)
{
	int next = INT_MAX;
	for (int i = 0; i < count; i++) {
		if (vars[i].is_register)
			return 0;
		if (vars[i].offs > vars[index].offs && vars[i].offs < next)
			next = vars[i].offs;
	}
	if (next == INT_MAX)
		return 0;
	// A variadic function's fixed parameters move with the argument count.
	if (frame->variadic && next >= 0)
		return 0;
	return next - vars[index].offs;
}

void
c_locals_var(const c_frame_view_t *frame, const dbg_cvar_t *vars, int count, int index,
             c_local_t *out)
{
	const dbg_cvar_t *v = &vars[index];
	memset(out, 0, sizeof *out);
	out->is_param = !v->is_register && v->offs >= 0;

	if (v->is_register) {
		out->status = C_LOCAL_REFUSED_REGISTER;
		return;
	}
	if (frame->state != C_FRAME_KNOWN) {
		out->status = C_LOCAL_UNKNOWN_FRAME;
		return;
	}
	if (frame->variadic && v->offs >= 0) {
		out->status = C_LOCAL_REFUSED_VARIADIC;
		return;
	}
	if (frame->entry_pending && v->offs == 0) {
		out->status = C_LOCAL_NOT_STORED;
		return;
	}

	const int address = frame->base + v->offs;
	const int size    = inferred_size(frame, vars, count, index);
	if (address < PLAIN_RAM_LOW || address + (size ? size : 1) > PLAIN_RAM_END) {
		out->status = C_LOCAL_REFUSED_LOCATION;
		return;
	}
	out->has_address = true;
	out->address     = (uint16_t)address;
	out->size        = size;
	out->status      = address < frame->c_sp ? C_LOCAL_UNALLOCATED : C_LOCAL_OK;
}

static bool
ident_start(char c)
{
	return isalpha((unsigned char)c) || c == '_';
}

static bool
ident_char(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

const char *
c_locals_expr_name(const char *expr, const dbg_cvar_t *vars, int count)
{
	const char *p = expr;
	while (*p) {
		if (*p == '$' || isdigit((unsigned char)*p)) {
			p++;
			while (ident_char(*p))
				p++;
		} else if (ident_start(*p)) {
			const char *start = p;
			while (ident_char(*p))
				p++;
			const size_t n = (size_t)(p - start);
			for (int i = 0; i < count; i++) {
				if (strlen(vars[i].name) == n && !memcmp(vars[i].name, start, n))
					return vars[i].name;
			}
		} else {
			p++;
		}
	}
	return NULL;
}
