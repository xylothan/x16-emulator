// Debugger expressions. See dbg_expr.h for the language.

#include "dbg_expr.h"

#include <stdio.h>
#include <string.h>

enum {
	OP_PUSH, OP_REG,
	OP_BYTE, OP_WORD, OP_VBYTE, OP_BYTE_AT, OP_WORD_AT,
	OP_NEG, OP_NOT, OP_INV,
	OP_MUL, OP_DIV, OP_MOD, OP_ADD, OP_SUB, OP_SHL, OP_SHR,
	OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
	OP_AND, OP_XOR, OP_OR,
	OP_JZ, OP_JNZ, OP_BOOL,
};

typedef struct {
	const char           *s;
	dbg_expr_t           *out;
	const dbg_expr_env_t *env;
	char                 *err;
	size_t                errn;
	int                   depth;
	int                   stack;
	bool                  in_brackets;   // a colon here belongs to byte[] or word[]
} parser_t;

typedef struct {
	const char *text;
	int         len;
	int         prec;
	int         code;
} binop_t;

// Longest spelling first, so that << is not read as <.
static const binop_t binops[] = {
	{ "||", 2, 1, OP_JNZ }, { "&&", 2, 2, OP_JZ },
	{ "==", 2, 6, OP_EQ },  { "!=", 2, 6, OP_NE },
	{ "<<", 2, 8, OP_SHL }, { ">>", 2, 8, OP_SHR },
	{ "<=", 2, 7, OP_LE },  { ">=", 2, 7, OP_GE },
	{ "|",  1, 3, OP_OR },  { "^",  1, 4, OP_XOR }, { "&", 1, 5, OP_AND },
	{ "<",  1, 7, OP_LT },  { ">",  1, 7, OP_GT },
	{ "+",  1, 9, OP_ADD }, { "-",  1, 9, OP_SUB },
	{ "*",  1, 10, OP_MUL }, { "/", 1, 10, OP_DIV }, { "%", 1, 10, OP_MOD },
};

static const char *const reg_names[DBG_EXPR_REG_COUNT] = {
	"A", "X", "Y", "SP", "P", "PC",
	"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
	"R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
	"rambank", "rombank",
};

const char *
dbg_expr_reg_name(dbg_expr_reg_t reg)
{
	return (int)reg >= 0 && reg < DBG_EXPR_REG_COUNT ? reg_names[reg] : "";
}

static char
lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool
same_word(const char *a, size_t n, const char *b)
{
	if (strlen(b) != n)
		return false;
	for (size_t i = 0; i < n; i++) {
		if (lower(a[i]) != lower(b[i]))
			return false;
	}
	return true;
}

static bool
is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static bool
is_name_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '@';
}

static bool
is_name_char(char c)
{
	return is_name_start(c) || is_digit(c);
}

static int
digit_value(char c)
{
	if (is_digit(c))
		return c - '0';
	c = lower(c);
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool
fail(parser_t *p, const char *message)
{
	snprintf(p->err, p->errn, "%s", message);
	return false;
}

// Up to 24 characters of the input, safe to put in a JSON reply.
static void
quote_input(const char *s, char out[25])
{
	int n = 0;
	for (; s[n] && n < 24; n++)
		out[n] = s[n] >= ' ' && s[n] < 127 ? s[n] : '?';
	out[n] = '\0';
}

static bool
syntax_error(parser_t *p)
{
	char shown[25];
	quote_input(p->s, shown);
	if (*p->s)
		snprintf(p->err, p->errn, "syntax error at \"%s\"", shown);
	else
		snprintf(p->err, p->errn, "the expression ends too soon");
	return false;
}

static void
skip_space(parser_t *p)
{
	while (*p->s == ' ' || *p->s == '\t')
		p->s++;
}

// `delta` is the change in pending values when this op falls through.
static bool
emit(parser_t *p, int code, uint32_t value, int bank, int delta)
{
	if (p->out->count >= DBG_EXPR_MAX_OPS)
		return fail(p, "the expression is too long");
	p->stack += delta;
	if (p->stack > DBG_EXPR_MAX_STACK)
		return fail(p, "the expression is too complex");
	dbg_expr_op_t *op = &p->out->ops[p->out->count++];
	op->code  = (uint8_t)code;
	op->bank  = (int16_t)bank;
	op->value = value;
	return true;
}

static bool
enter(parser_t *p)
{
	if (++p->depth > DBG_EXPR_MAX_DEPTH)
		return fail(p, "the expression is nested too deeply");
	return true;
}

static bool parse_binary(parser_t *p, int min_prec);
static bool parse_unary(parser_t *p);

static bool
parse_number(parser_t *p)
{
	const char *s = p->s;
	uint64_t    value = 0;
	int         base = 10;
	if (*s == '$') {
		base = 16;
		s++;
	} else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	const char *digits = s;
	for (;; s++) {
		const int d = digit_value(*s);
		if (d < 0 || d >= base)
			break;
		value = value * (unsigned)base + (unsigned)d;
		if (value > 0xFFFFFFFFull)
			return fail(p, "a number must fit in 32 bits");
	}
	if (s == digits || is_name_char(*s)) {
		char shown[25];
		quote_input(p->s, shown);
		snprintf(p->err, p->errn, "cannot read the number at \"%s\"", shown);
		return false;
	}
	if (base == 10 && s - digits > 1 && digits[0] == '0')
		return fail(p, "a decimal has no leading zero; write hex as $0400");
	p->s = s;
	return emit(p, OP_PUSH, (uint32_t)value, DBG_EXPR_NO_BANK, 1);
}

static int
find_register(const char *name, size_t n)
{
	for (int i = 0; i < DBG_EXPR_REG_COUNT; i++) {
		if (same_word(name, n, reg_names[i]))
			return i;
	}
	return -1;
}

// After byte, word or vbyte, at the [. `pinned` is the op for the b:a form.
static bool
parse_memory(parser_t *p, int plain, int pinned)
{
	if (!enter(p))
		return false;
	const bool outer = p->in_brackets;
	p->in_brackets = true;
	p->s++;
	if (!parse_binary(p, 1))
		return false;
	skip_space(p);
	if (*p->s == ':') {
		if (pinned < 0)
			return fail(p, "VRAM has no banks");
		p->s++;
		if (!parse_binary(p, 1) || !emit(p, pinned, 0, DBG_EXPR_NO_BANK, -1))
			return false;
	} else if (!emit(p, plain, 0, DBG_EXPR_NO_BANK, 0)) {
		return false;
	}
	skip_space(p);
	if (*p->s != ']')
		return syntax_error(p);
	p->s++;
	p->depth--;
	p->in_brackets = outer;
	return true;
}

static bool
parse_name(parser_t *p)
{
	const char *start = p->s;
	while (is_name_char(*p->s))
		p->s++;
	const size_t n = (size_t)(p->s - start);

	const char *after = p->s;
	while (*after == ' ' || *after == '\t')
		after++;
	if (*after == '[') {
		const bool is_byte = same_word(start, n, "byte"), is_word = same_word(start, n, "word");
		if (is_byte || is_word || same_word(start, n, "vbyte")) {
			p->s = after;
			return is_byte ? parse_memory(p, OP_BYTE, OP_BYTE_AT)
			     : is_word ? parse_memory(p, OP_WORD, OP_WORD_AT)
			               : parse_memory(p, OP_VBYTE, -1);
		}
	}

	const int reg = find_register(start, n);
	if (reg >= 0)
		return emit(p, OP_REG, (uint32_t)reg, DBG_EXPR_NO_BANK, 1);

	if (n > DBG_EXPR_MAX_NAME)
		return fail(p, "that name is too long");
	if (!p->env->symbol) {
		snprintf(p->err, p->errn, "no symbol \"%.*s\"", (int)n, start);
		return false;
	}
	char name[DBG_EXPR_MAX_NAME + 1];
	memcpy(name, start, n);
	name[n] = '\0';
	uint32_t value = 0;
	int      bank = DBG_EXPR_NO_BANK;
	if (!p->env->symbol(p->env->user, name, &value, &bank, p->err, p->errn))
		return false;
	return emit(p, OP_PUSH, value, bank >= 0 ? bank : DBG_EXPR_NO_BANK, 1);
}

static bool
parse_operand(parser_t *p)
{
	skip_space(p);
	const char c = *p->s;
	if (c == '(') {
		if (!enter(p))
			return false;
		const bool outer = p->in_brackets;
		p->in_brackets = false;
		p->s++;
		if (!parse_binary(p, 1))
			return false;
		skip_space(p);
		if (*p->s != ')')
			return syntax_error(p);
		p->s++;
		p->depth--;
		p->in_brackets = outer;
		return true;
	}
	if (c == '$' || is_digit(c))
		return parse_number(p);
	if (is_name_start(c))
		return parse_name(p);
	return syntax_error(p);
}

// An operand, or bank:address as one operand.
static bool
parse_primary(parser_t *p)
{
	if (!parse_operand(p))
		return false;
	if (p->in_brackets)
		return true;
	skip_space(p);
	if (*p->s != ':')
		return true;
	p->s++;
	return parse_operand(p) && emit(p, OP_BYTE_AT, 0, DBG_EXPR_NO_BANK, -1);
}

static bool
parse_unary(parser_t *p)
{
	skip_space(p);
	const char c = *p->s;
	if (c != '-' && c != '!' && c != '~')
		return parse_primary(p);
	if (!enter(p))
		return false;
	p->s++;
	if (!parse_unary(p))
		return false;
	p->depth--;
	return emit(p, c == '-' ? OP_NEG : c == '!' ? OP_NOT : OP_INV, 0, DBG_EXPR_NO_BANK, 0);
}

static const binop_t *
match_binop(const char *s)
{
	for (size_t i = 0; i < sizeof binops / sizeof binops[0]; i++) {
		if (!strncmp(s, binops[i].text, (size_t)binops[i].len))
			return &binops[i];
	}
	return NULL;
}

static bool
parse_binary(parser_t *p, int min_prec)
{
	if (!parse_unary(p))
		return false;
	for (;;) {
		skip_space(p);
		const binop_t *o = match_binop(p->s);
		if (!o || o->prec < min_prec)
			return true;
		p->s += o->len;
		if (o->code == OP_JZ || o->code == OP_JNZ) {
			const int jump = p->out->count;
			if (!emit(p, o->code, 0, DBG_EXPR_NO_BANK, -1) || !parse_binary(p, o->prec + 1) ||
			    !emit(p, OP_BOOL, 0, DBG_EXPR_NO_BANK, 0))
				return false;
			p->out->ops[jump].value = (uint32_t)p->out->count;
		} else if (!parse_binary(p, o->prec + 1) ||
		           !emit(p, o->code, 0, DBG_EXPR_NO_BANK, -1)) {
			return false;
		}
	}
}

bool
dbg_expr_compile(const char *text, const dbg_expr_env_t *env, dbg_expr_t *out,
                 char *err, size_t errn)
{
	out->count = 0;
	if (!text || !memchr(text, '\0', DBG_EXPR_MAX_TEXT + 1)) {
		snprintf(err, errn, "the expression is longer than %d characters", DBG_EXPR_MAX_TEXT);
		return false;
	}
	parser_t p = { text, out, env, err, errn, 0, 0, false };
	skip_space(&p);
	if (!*p.s)
		return fail(&p, "the expression is empty");
	if (!parse_binary(&p, 1))
		return false;
	skip_space(&p);
	if (*p.s == '=')
		return fail(&p, "use == to compare; this cannot assign");
	if (*p.s)
		return syntax_error(&p);
	return true;
}

// ---- Evaluation -------------------------------------------------------------

typedef struct {
	uint32_t v;
	int      bank;
} slot_t;

static bool
bad_address(char *err, size_t errn, const char *what, uint32_t addr)
{
	snprintf(err, errn, "%s $%X is out of range", what, (unsigned)addr);
	return false;
}

// Takes what a memory op consumes off the stack and checks it.
static bool
take_address(int code, slot_t *st, int *sp, uint32_t *addr, int *bank, char *err, size_t errn)
{
	const slot_t top = st[--*sp];
	*addr = top.v;
	*bank = top.bank;
	if (code == OP_VBYTE) {
		*bank = DBG_EXPR_NO_BANK;
		return *addr <= 0x1FFFF || bad_address(err, errn, "VRAM address", *addr);
	}
	if (code == OP_BYTE_AT || code == OP_WORD_AT) {
		const uint32_t b = st[--*sp].v;
		if (b > 0xFF)
			return bad_address(err, errn, "bank", b);
		*bank = (int)b;
	}
	if (*addr > 0xFFFFFF)
		return bad_address(err, errn, "address", *addr);
	if (*bank >= 0 && *addr > 0xFFFF) {
		snprintf(err, errn, "a bank:address takes a 16-bit address, not $%X", (unsigned)*addr);
		return false;
	}
	// The high byte would come from ROM bank b, a different device.
	if (*bank >= 0 && (code == OP_WORD || code == OP_WORD_AT) && *addr == 0xBFFF) {
		snprintf(err, errn, "a word at $BFFF in a named bank crosses into ROM");
		return false;
	}
	return true;
}

static uint32_t
read_at(int code, const dbg_expr_env_t *env, uint32_t addr, int bank)
{
	if (code == OP_VBYTE)
		return env->read_vram(env->user, addr);
	const uint32_t lo = env->read_cpu(env->user, addr, bank);
	if (code == OP_BYTE || code == OP_BYTE_AT)
		return lo;
	const uint32_t next = (addr & 0xFF0000) | ((addr + 1) & 0xFFFF);
	return lo | (uint32_t)env->read_cpu(env->user, next, bank) << 8;
}

static bool
apply_binary(int code, slot_t *a, slot_t b, char *err, size_t errn)
{
	const uint32_t x = a->v, y = b.v;
	int bank = DBG_EXPR_NO_BANK;
	uint32_t r = 0;
	switch (code) {
		case OP_MUL: r = x * y; break;
		case OP_DIV:
		case OP_MOD:
			if (!y) {
				snprintf(err, errn, code == OP_DIV ? "division by zero" : "modulo by zero");
				return false;
			}
			r = code == OP_DIV ? x / y : x % y;
			break;
		case OP_ADD:
			r = x + y;
			if (a->bank < 0 || b.bank < 0)
				bank = a->bank >= 0 ? a->bank : b.bank;
			break;
		case OP_SUB:
			r = x - y;
			if (b.bank < 0)
				bank = a->bank;
			break;
		case OP_SHL: r = y >= 32 ? 0 : x << y; break;
		case OP_SHR: r = y >= 32 ? 0 : x >> y; break;
		case OP_LT:  r = x < y; break;
		case OP_LE:  r = x <= y; break;
		case OP_GT:  r = x > y; break;
		case OP_GE:  r = x >= y; break;
		case OP_EQ:  r = x == y; break;
		case OP_NE:  r = x != y; break;
		case OP_AND: r = x & y; break;
		case OP_XOR: r = x ^ y; break;
		case OP_OR:  r = x | y; break;
		default:
			snprintf(err, errn, "bad compiled expression");
			return false;
	}
	a->v = r;
	a->bank = bank;
	return true;
}

// Runs ops up to, and not including, index `limit`.
static bool
run(const dbg_expr_t *e, int limit, const dbg_expr_env_t *env, slot_t *st, int *spp,
    char *err, size_t errn)
{
	int sp = 0;
	for (int pc = 0; pc < limit; pc++) {
		const dbg_expr_op_t *op = &e->ops[pc];
		switch (op->code) {
			case OP_PUSH:
				st[sp].v = op->value;
				st[sp++].bank = op->bank;
				break;
			case OP_REG:
				st[sp].v = env->read_reg(env->user, (dbg_expr_reg_t)op->value);
				st[sp++].bank = DBG_EXPR_NO_BANK;
				break;
			case OP_BYTE:
			case OP_WORD:
			case OP_VBYTE:
			case OP_BYTE_AT:
			case OP_WORD_AT: {
				uint32_t addr;
				int      bank;
				if (!take_address(op->code, st, &sp, &addr, &bank, err, errn))
					return false;
				st[sp].v = read_at(op->code, env, addr, bank);
				st[sp++].bank = DBG_EXPR_NO_BANK;
				break;
			}
			case OP_NEG:
				st[sp - 1].v = 0u - st[sp - 1].v;
				st[sp - 1].bank = DBG_EXPR_NO_BANK;
				break;
			case OP_NOT:
				st[sp - 1].v = !st[sp - 1].v;
				st[sp - 1].bank = DBG_EXPR_NO_BANK;
				break;
			case OP_INV:
				st[sp - 1].v = ~st[sp - 1].v;
				st[sp - 1].bank = DBG_EXPR_NO_BANK;
				break;
			case OP_BOOL:
				st[sp - 1].v = st[sp - 1].v != 0;
				st[sp - 1].bank = DBG_EXPR_NO_BANK;
				break;
			case OP_JZ:
			case OP_JNZ: {
				const bool truth = st[sp - 1].v != 0;
				if (truth == (op->code == OP_JNZ)) {
					st[sp - 1].v = truth;
					st[sp - 1].bank = DBG_EXPR_NO_BANK;
					pc = (int)op->value - 1;
				} else {
					sp--;
				}
				break;
			}
			default:
				sp--;
				if (!apply_binary(op->code, &st[sp - 1], st[sp], err, errn))
					return false;
				break;
		}
	}
	*spp = sp;
	return true;
}

bool
dbg_expr_eval(const dbg_expr_t *expr, const dbg_expr_env_t *env, dbg_expr_value_t *out,
              char *err, size_t errn)
{
	slot_t st[DBG_EXPR_MAX_STACK];
	int    sp = 0;
	if (!expr->count) {
		snprintf(err, errn, "the expression is empty");
		return false;
	}
	if (!run(expr, expr->count, env, st, &sp, err, errn))
		return false;
	out->value = st[0].v;
	out->bank  = st[0].bank;
	return true;
}

bool
dbg_expr_target(const dbg_expr_t *expr, const dbg_expr_env_t *env, dbg_expr_target_t *out,
                char *err, size_t errn)
{
	static const char not_assignable[] =
		"cannot assign to this: name a register, byte[...], word[...], vbyte[...] or bank:address";
	if (!expr->count) {
		snprintf(err, errn, "the expression is empty");
		return false;
	}
	const dbg_expr_op_t *last = &expr->ops[expr->count - 1];
	memset(out, 0, sizeof *out);
	out->bank = DBG_EXPR_NO_BANK;

	if (last->code == OP_REG) {
		if (expr->count != 1) {
			snprintf(err, errn, "%s", not_assignable);
			return false;
		}
		out->kind = DBG_EXPR_TARGET_REG;
		out->reg  = (dbg_expr_reg_t)last->value;
		return true;
	}
	switch (last->code) {
		case OP_BYTE:
		case OP_BYTE_AT: out->kind = DBG_EXPR_TARGET_BYTE;  break;
		case OP_WORD:
		case OP_WORD_AT: out->kind = DBG_EXPR_TARGET_WORD;  break;
		case OP_VBYTE:   out->kind = DBG_EXPR_TARGET_VBYTE; break;
		default:
			snprintf(err, errn, "%s", not_assignable);
			return false;
	}
	slot_t st[DBG_EXPR_MAX_STACK];
	int    sp = 0;
	if (!run(expr, expr->count - 1, env, st, &sp, err, errn))
		return false;
	return take_address(last->code, st, &sp, &out->addr, &out->bank, err, errn);
}
