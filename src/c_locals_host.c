// Commander X16 Emulator — c_locals, wired to the machine. See c_locals_host.h.

#include "c_locals_host.h"

#include "c_frames_host.h"
#include "call_stack.h"
#include "glue.h"
#include "memory.h"

#include <stdlib.h>
#include <string.h>

static uint8_t
host_read(uint16_t a)
{
	return debug_read6502(a, 0, USE_CURRENT_X16_BANK);
}

static uint8_t
stack_read(uint16_t a, uint8_t bank)
{
	return debug_read6502(a, bank, USE_CURRENT_X16_BANK);
}

static bool
host_named(uint16_t a)
{
	const char *name = NULL;
	return dbg_info_addr_to_label(a, &name);
}

static const c_locals_helpers_t *
helpers(void)
{
	static c_locals_helpers_t cached;
	static unsigned           seen;
	static bool               valid;
	const unsigned            now = dbg_info_changes();
	if (!valid || seen != now) {
		c_locals_pick_helpers(dbg_info_symbol_count(), dbg_info_symbol_at, &cached);
		seen  = now;
		valid = true;
	}
	return &cached;
}

static void
resolve_frame(int frame, int count, const uint16_t *addrs, const uint16_t *slots,
              c_locals_frame_t *out)
{
	c_frame_view_t *v = &out->view;
	uint16_t        c_sp_addr;

	if (regs.is65c816 && !regs.e) {
		v->state = C_FRAME_NATIVE;
		return;
	}
	if (regs.dp != 0) {
		v->state = C_FRAME_DIRECT_PAGE;
		return;
	}
	if (regs.k != 0 || out->entry >= 0xA000) {
		v->state = C_FRAME_BANKED;
		return;
	}
	if (!c_frames_host_c_sp(&c_sp_addr)) {
		v->state = C_FRAME_NO_STACK_POINTER;
		return;
	}
	v->c_sp = (uint16_t)(host_read(c_sp_addr) | (host_read((uint16_t)(c_sp_addr + 1)) << 8));

	bool      variadic = false;
	const int pushed   = c_locals_push_bytes(out->entry, host_read, helpers(), host_named, &variadic);
	if (pushed < 0) {
		v->state = C_FRAME_PROLOGUE;
		return;
	}

	c_frame_record_t r;
	if (frame + 1 >= count || !c_frames_at_slot(regs.sp, slots[frame + 1], &r)) {
		v->state = C_FRAME_NOT_SEEN;
		return;
	}
	// Startup enters main by JMP, so the record names callmain.
	const bool via_start = r.target != out->entry &&
	                       c_locals_is_main_start(r.target, out->entry, host_read, helpers());
	if (!c_locals_record_matches(&r, slots[frame + 1], addrs[frame + 1],
	                             via_start ? r.target : out->entry,
	                             host_read(slots[frame + 1]),
	                             host_read((uint16_t)(slots[frame + 1] + 1)))) {
		v->state = C_FRAME_NOT_SEEN;
		return;
	}

	v->state         = C_FRAME_KNOWN;
	v->base          = (int)r.c_sp - (via_start ? C_LOCALS_MAIN_START_PUSH : 0) - pushed;
	v->variadic      = variadic;
	v->entry_pending = c_locals_entry_pending(frame == 0, addrs[frame], out->entry, pushed);
}

bool
c_locals_host_frame(int frame, c_locals_frame_t *out)
{
	uint16_t addrs[CALL_STACK_MAX], slots[CALL_STACK_MAX];
	const int count = call_stack_unwind_slots(regs.pc, regs.sp, regs.k, regs.is65c816 && !regs.e,
	                                          stack_read, addrs, slots);
	memset(out, 0, sizeof *out);
	out->func = -1;
	if (frame < 0 || frame >= count)
		return false;

	const uint16_t site = call_stack_site(frame == 0, addrs[frame]);
	const int      func = dbg_info_cfunc_containing((dbg_addr_t)site | ((dbg_addr_t)regs.k << 16));
	dbg_cfunc_t    f;
	if (func < 0 || !dbg_info_cfunc_at(func, &f))
		return true;

	out->func      = func;
	out->name      = f.name;
	out->var_count = f.var_count;
	if (f.entry > 0xFFFF) {
		out->view.state = C_FRAME_BANKED;
		return true;
	}
	out->entry = (uint16_t)f.entry;
	resolve_frame(frame, count, addrs, slots, out);
	return true;
}

static dbg_cvar_t *
load_vars(int func, int count)
{
	if (count <= 0)
		return NULL;
	dbg_cvar_t *vars = (dbg_cvar_t *)malloc((size_t)count * sizeof *vars);
	if (!vars)
		return NULL;
	for (int i = 0; i < count; i++) {
		if (!dbg_info_cfunc_var_at(func, i, &vars[i])) {
			free(vars);
			return NULL;
		}
	}
	return vars;
}

c_locals_row_t *
c_locals_host_rows(const c_locals_frame_t *frame)
{
	if (frame->func < 0)
		return NULL;
	dbg_cvar_t *vars = load_vars(frame->func, frame->var_count);
	if (!vars)
		return NULL;
	c_locals_row_t *rows = (c_locals_row_t *)malloc((size_t)frame->var_count * sizeof *rows);
	if (rows) {
		for (int i = 0; i < frame->var_count; i++) {
			rows[i].var = vars[i];
			c_locals_var(&frame->view, vars, frame->var_count, i, &rows[i].local);
		}
	}
	free(vars);
	return rows;
}

int
c_locals_host_bytes(const c_local_t *local, uint8_t *out, bool *truncated)
{
	int n = local->size ? local->size : C_LOCALS_SHOW_UNKNOWN;
	*truncated = local->size > C_LOCALS_SHOW_MAX;
	if (n > C_LOCALS_SHOW_MAX)
		n = C_LOCALS_SHOW_MAX;
	if (local->address + n > 0x9F00)
		n = 0x9F00 - local->address;
	for (int i = 0; i < n; i++)
		out[i] = host_read((uint16_t)(local->address + i));
	return n;
}

const char *
c_locals_host_expr_name(int frame, const char *expr)
{
	c_locals_frame_t f;
	if (!c_locals_host_frame(frame, &f) && !c_locals_host_frame(0, &f))
		return NULL;
	if (f.func < 0)
		return NULL;
	dbg_cvar_t *vars = load_vars(f.func, f.var_count);
	if (!vars)
		return NULL;
	const char *name = c_locals_expr_name(expr, vars, f.var_count);
	free(vars);
	return name;
}
