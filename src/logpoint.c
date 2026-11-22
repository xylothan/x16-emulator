// Logpoints. See logpoint.h.

#include "logpoint.h"

#include <stdio.h>
#include <stdlib.h>

#include "dbg_info.h"
#include "dbg_machine.h"

typedef struct {
	dbg_log_msg_t *msg;
	unsigned       symbols;   // dbg_info_changes() when it was compiled
} logpoint_t;

static dbg_log_ring_t s_ring;

static void
logpoint_free(void *p)
{
	logpoint_t *lp = (logpoint_t *)p;
	if (lp) {
		dbg_log_free(lp->msg);
		free(lp);
	}
}

// Runs on the CPU's path: no allocation, nothing sent, nothing written.
static void
logpoint_fire(void *p, debug_owner_t owner, int pc, uint8_t bank, int x16Bank)
{
	logpoint_t           *lp  = (logpoint_t *)p;
	const dbg_expr_env_t *env = dbg_machine_env();

	// Compiled expressions hold symbol values, which a load can move or define.
	const unsigned now = dbg_info_changes();
	if (lp->symbols != now) {
		dbg_log_recompile(lp->msg, env);
		lp->symbols = now;
	}

	dbg_log_entry_t *e = dbg_log_ring_push(&s_ring, (uint8_t)owner);
	e->pc       = (uint32_t)pc;
	e->bank     = bank;
	e->x16_bank = (int16_t)x16Bank;
	dbg_log_format(lp->msg, env, e->text, sizeof e->text);
}

bool
logpoint_set(int pc, uint8_t bank, int x16Bank, debug_owner_t owner, const char *message,
             char *err, size_t errn)
{
	dbg_log_msg_t *m = dbg_log_compile(message, dbg_machine_env(), err, errn);
	if (!m)
		return false;
	logpoint_t *lp = (logpoint_t *)malloc(sizeof *lp);
	if (!lp) {
		dbg_log_free(m);
		snprintf(err, errn, "out of memory");
		return false;
	}
	lp->msg     = m;
	lp->symbols = dbg_info_changes();

	debug_bp_log_fire = logpoint_fire;
	debug_bp_log_free = logpoint_free;
	if (!debug_bp_set_log(pc, bank, x16Bank, owner, lp)) {
		logpoint_free(lp);
		snprintf(err, errn, "no breakpoint of this owner at that address");
		return false;
	}
	return true;
}

void
logpoint_clear(int pc, uint8_t bank, int x16Bank, debug_owner_t owner)
{
	debug_bp_clear_log_for(pc, bank, x16Bank, owner);
}

const char *
logpoint_message(int pc, uint8_t bank, int x16Bank, debug_owner_t owner)
{
	const logpoint_t *lp = (const logpoint_t *)debug_bp_log_for(pc, bank, x16Bank, owner);
	return lp ? dbg_log_source(lp->msg) : NULL;
}

dbg_log_ring_t *
logpoint_ring(void)
{
	return &s_ring;
}
