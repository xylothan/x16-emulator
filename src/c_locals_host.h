// Commander X16 Emulator — c_locals, wired to the machine.

#ifndef _C_LOCALS_HOST_H_
#define _C_LOCALS_HOST_H_

#include "c_locals.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bytes shown for one variable, and for one whose size is unknown.
#define C_LOCALS_SHOW_MAX     32
#define C_LOCALS_SHOW_UNKNOWN 4

typedef struct {
	int            func;       // -1 when the frame is in no C function
	const char    *name;       // lasts until debug info next changes
	uint16_t       entry;
	int            var_count;
	c_frame_view_t view;
} c_locals_frame_t;

typedef struct {
	dbg_cvar_t var;
	c_local_t  local;
} c_locals_row_t;

// False when the call stack has no such frame.
bool c_locals_host_frame(int frame, c_locals_frame_t *out);

// One row per variable, in record order. Free with free(). NULL when there are
// none or no memory.
c_locals_row_t *c_locals_host_rows(const c_locals_frame_t *frame);

// Bytes to show for a row with an address, at most C_LOCALS_SHOW_MAX.
int c_locals_host_bytes(const c_local_t *local, uint8_t *out, bool *truncated);

// The variable of `frame`'s function that `expr` names, or NULL. Frame 0 when
// there is no such frame.
const char *c_locals_host_expr_name(int frame, const char *expr);

#ifdef __cplusplus
}
#endif

#endif
