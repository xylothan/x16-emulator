// Expression conditions. See dbg_cond.h.

#include "dbg_cond.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/fake6502.h"
#include "cpu/irq_ctx.h"
#include "dbg_expr.h"
#include "dbg_info.h"
#include "dbg_machine.h"
#include "debug_core.h"
#include "glue.h"

#define COND_ERR_MAX 96

struct dbg_cond {
	char       text[DBG_EXPR_MAX_TEXT + 1];
	bool       access;
	bool       ok;
	unsigned   symbols;   // dbg_info_changes() when it was compiled
	char       err[COND_ERR_MAX];
	dbg_expr_t expr;
};

static const debug_cond_access_t *s_access;

static bool s_err_valid;
static int  s_err_instr;
static uint32_t s_err_resets;
static char s_err_text[DBG_EXPR_MAX_TEXT + 1];
static char s_err_message[COND_ERR_MAX];

// During an access regs.pc is already past the operands, so PC is the
// instruction that made it.
static uint32_t
read_reg_at_access(void *user, dbg_expr_reg_t reg)
{
	const int r = (int)reg;
	if (s_access) {
		if (r == DBG_EXPR_ACCESS_VAL)
			return s_access->value;
		if (r == DBG_EXPR_ACCESS_ADDR)
			return s_access->addr;
		if (r == DBG_EXPR_ACCESS_IS_READ)
			return s_access->access == DEBUG_WP_ACCESS_READ;
		if (r == DBG_EXPR_ACCESS_IS_WRITE)
			return s_access->access == DEBUG_WP_ACCESS_WRITE;
		if (reg == DBG_EXPR_REG_PC)
			return (is_gen2 || regs.is65c816) ? ((uint32_t)regs.k << 16) | opcode_addr
			                                  : opcode_addr;
	}
	if (r >= DBG_EXPR_REG_COUNT)
		return 0;
	return dbg_machine_env()->read_reg(user, reg);
}

static const dbg_expr_env_t *
env_for(bool access)
{
	static dbg_expr_env_t watch;
	if (!access)
		return dbg_machine_env();
	if (!watch.read_reg) {
		watch          = *dbg_machine_env();
		watch.read_reg = read_reg_at_access;
		watch.access   = true;
	}
	return &watch;
}

static void
record_error(const dbg_cond_t *c, const char *message)
{
	s_err_valid = true;
	s_err_instr = instruction_counter;
	s_err_resets = cpu_irq_reset_count();
	snprintf(s_err_text, sizeof s_err_text, "%s", c->text);
	snprintf(s_err_message, sizeof s_err_message, "%s", message);
}

// Runs on the CPU's path: no allocation, nothing sent, nothing written.
static bool
cond_test(void *p, const debug_cond_access_t *access)
{
	dbg_cond_t           *c   = (dbg_cond_t *)p;
	const dbg_expr_env_t *env = env_for(c->access);

	// Compiled expressions hold symbol values, which a load can move or define.
	const unsigned now = dbg_info_changes();
	if (c->symbols != now) {
		c->ok      = dbg_expr_compile(c->text, env, &c->expr, c->err, sizeof c->err);
		c->symbols = now;
	}
	if (!c->ok) {
		record_error(c, c->err);
		return true;
	}

	char             err[COND_ERR_MAX];
	dbg_expr_value_t v;
	s_access = access;
	const bool ok = dbg_expr_eval(&c->expr, env, &v, err, sizeof err);
	s_access = NULL;
	if (!ok) {
		record_error(c, err);
		return true;
	}
	return v.value != 0;
}

static bool
cond_same(const void *a, const void *b)
{
	const dbg_cond_t *x = (const dbg_cond_t *)a;
	const dbg_cond_t *y = (const dbg_cond_t *)b;
	return x->access == y->access && !strcmp(x->text, y->text);
}

static void
cond_free(void *p)
{
	free(p);
}

bool
dbg_cond_check(const char *text, bool access, char *err, size_t errn)
{
	dbg_expr_t expr;
	return dbg_expr_compile(text, env_for(access), &expr, err, errn);
}

dbg_cond_t *
dbg_cond_new(const char *text, bool access)
{
	dbg_cond_t *c = (dbg_cond_t *)malloc(sizeof *c);
	if (!c)
		return NULL;
	snprintf(c->text, sizeof c->text, "%s", text);
	c->access  = access;
	c->symbols = dbg_info_changes();
	c->ok      = dbg_expr_compile(c->text, env_for(access), &c->expr, c->err, sizeof c->err);

	debug_cond_test = cond_test;
	debug_cond_same = cond_same;
	debug_cond_free = cond_free;
	return c;
}

void
dbg_cond_free(dbg_cond_t *cond)
{
	cond_free(cond);
}

const char *
dbg_cond_text(const dbg_cond_t *cond)
{
	return cond->text;
}

bool
dbg_cond_last_error(const char **text, const char **message)
{
	if (!s_err_valid || s_err_instr != instruction_counter ||
	    s_err_resets != cpu_irq_reset_count())
		return false;
	*text    = s_err_text;
	*message = s_err_message;
	return true;
}
