// Checks for the debugger expression engine (src/dbg_expr.c).

#include "dbg_expr.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void
check(bool cond, const char *what)
{
	if (!cond) {
		failures++;
		printf("FAIL: %s\n", what);
	} else {
		printf("ok  : %s\n", what);
	}
}

static uint8_t  cpu[0x10000];
static uint8_t  high[0x10000];               // 65C816 bank 1
static uint8_t  banked[4][0x2000];           // X16 banks at $A000
static uint8_t  vram[0x20000];
static uint32_t reg_values[DBG_EXPR_REG_COUNT];
static int      reads;

static uint8_t
read_cpu(void *user, uint32_t addr, int bank)
{
	(void)user;
	reads++;
	if (addr >> 16)
		return high[addr & 0xFFFF];
	if (bank >= 0 && bank < 4 && addr >= 0xA000 && addr < 0xC000)
		return banked[bank][addr - 0xA000];
	return cpu[addr];
}

static uint8_t
read_vram(void *user, uint32_t addr)
{
	(void)user;
	reads++;
	return vram[addr];
}

static uint32_t
read_reg(void *user, dbg_expr_reg_t reg)
{
	(void)user;
	return (int)reg < DBG_EXPR_REG_COUNT ? reg_values[reg] : 0x100u + (uint32_t)reg;
}

static bool
symbol(void *user, const char *name, uint32_t *value, int *bank, char *err, size_t errn)
{
	(void)user;
	static const struct { const char *name; uint32_t value; int bank; } table[] = {
		{ "main", 0x0810, -1 }, { "data", 0xA100, 2 }, { "other", 0xA000, 3 },
		{ "fade", 0x1234, -1 }, { "sp1", 0x51, -1 }, { "R16", 0x52, -1 }, { "a1", 0x53, -1 },
		{ "Ready", 0x54, -1 }, { "@local", 0x55, -1 }, { "scope.name", 0x56, -1 },
		{ "byte", 0x57, -1 }, { "bp_clear", 0x58, -1 },
	};
	for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
		if (!strcmp(table[i].name, name)) {
			*value = table[i].value;
			*bank = table[i].bank;
			return true;
		}
	}
	snprintf(err, errn, "no symbol \"%s\"", name);
	return false;
}

static const dbg_expr_env_t env = { NULL, read_cpu, read_vram, read_reg, symbol };

static bool
eval(const char *text, dbg_expr_value_t *v, char *err, size_t errn)
{
	dbg_expr_t e;
	return dbg_expr_compile(text, &env, &e, err, errn) && dbg_expr_eval(&e, &env, v, err, errn);
}

static void
expect(const char *text, uint32_t want)
{
	dbg_expr_value_t v = { 0, -1 };
	char err[160] = "", what[640];
	const bool ok = eval(text, &v, err, sizeof err);
	snprintf(what, sizeof what, "%s == $%X (got %s$%X %s)", text, (unsigned)want,
	         ok ? "" : "error ", (unsigned)v.value, err);
	check(ok && v.value == want, what);
}

static void
expect_error(const char *text, const char *needle)
{
	dbg_expr_value_t v;
	char err[160] = "", what[640];
	const bool ok = eval(text, &v, err, sizeof err);
	snprintf(what, sizeof what, "%.200s is an error with \"%s\" (got \"%s\")", text, needle, err);
	check(!ok && strstr(err, needle), what);
}

static void
expect_bank(const char *text, int bank)
{
	dbg_expr_value_t v = { 0, -2 };
	char err[160] = "", what[640];
	const bool ok = eval(text, &v, err, sizeof err);
	snprintf(what, sizeof what, "%s carries bank %d (got %d)", text, bank, v.bank);
	check(ok && v.bank == bank, what);
}

static void
test_numbers(void)
{
	expect("0", 0);
	expect("10", 10);
	expect("$10", 0x10);
	expect("0x10", 0x10);
	expect("0XfF", 0xFF);
	expect("$FFFFFFFF", 0xFFFFFFFF);
	expect("4294967295", 0xFFFFFFFF);
	expect("  7  ", 7);

	expect_error("4294967296", "32 bits");
	expect_error("$100000000", "32 bits");
	expect_error("0x100000000", "32 bits");
	expect_error("010", "leading zero");
	expect_error("0400", "leading zero");
	expect_error("00", "leading zero");
	expect_error("$", "cannot read");
	expect_error("0x", "cannot read");
	expect_error("$ZZZZ", "cannot read");
	expect_error("0xNOPE", "cannot read");
	expect_error("$12G", "cannot read");
	expect_error("12ab", "cannot read");
	expect_error("1.5", "cannot read");
}

static void
test_width_and_overflow(void)
{
	expect("$FFFFFFFF+1", 0);
	expect("0-1", 0xFFFFFFFF);
	expect("-1", 0xFFFFFFFF);
	expect("$10000*$10000", 0);
	expect("$FFFFFFFF*$FFFFFFFF", 1);
	expect("1<<31", 0x80000000);
	expect("1<<32", 0);
	expect("1<<$FFFFFFFF", 0);
	expect("$80000000>>31", 1);
	expect("$80000000>>32", 0);
	expect("-1>>28", 0xF);
	expect("~0", 0xFFFFFFFF);
	expect("-1<0", 0);
	expect("0<-1", 1);
	expect("-1/2", 0x7FFFFFFF);
	expect("-1%16", 15);
	expect("$80000000/-1", 0);
}

static void
test_operators(void)
{
	expect("2+3", 5);
	expect("5-3", 2);
	expect("6*7", 42);
	expect("7/2", 3);
	expect("7%3", 1);
	expect("$F0|$0F", 0xFF);
	expect("$FF&$0F", 0x0F);
	expect("$FF^$0F", 0xF0);
	expect("1<<4", 16);
	expect("16>>2", 4);
	expect("1==1", 1);
	expect("1==2", 0);
	expect("1!=2", 1);
	expect("1<2", 1);
	expect("2<=2", 1);
	expect("3>2", 1);
	expect("2>=3", 0);
	expect("!0", 1);
	expect("!5", 0);
	expect("!!5", 1);
	expect("~$FFFFFFFE", 1);
	expect("- -5", 5);
	expect("-(2+3)", 0xFFFFFFFB);
	expect("5&&3", 1);
	expect("0&&3", 0);
	expect("5||0", 1);
	expect("0||0", 0);
	expect("0||7", 1);
}

static void
test_precedence(void)
{
	expect("2+3*4", 14);
	expect("(2+3)*4", 20);
	expect("10-3-2", 5);
	expect("100/10/2", 5);
	expect("1<<2+1", 8);
	expect("1+1<<2", 8);
	expect("1<2==1", 1);
	expect("$81&$80==$80", 1);
	expect("($81&$80)==$80", 1);
	expect("$82&$80==$80", 0);
	expect("1|2^3&4", 3);
	expect("1|0&&0", 0);
	expect("0&&0||1", 1);
	expect("-2*3", 0xFFFFFFFA);
	expect("!0+1", 2);
	expect("~0&$FF", 0xFF);
}

static void
test_short_circuit(void)
{
	expect("0&&1/0", 0);
	expect("1||1/0", 1);
	expect("0&&byte[$FFFFFFFF]", 0);
	reads = 0;
	expect("0&&byte[$10]", 0);
	expect("1||byte[$10]", 1);
	check(reads == 0, "the undecided side is not read");
	expect_error("1&&1/0", "division");
	expect_error("0||1/0", "division");
	expect("(0&&1)+(1||0)+2", 3);
}

static void
test_errors(void)
{
	expect_error("1/0", "division by zero");
	expect_error("1%0", "modulo by zero");
	expect_error("", "empty");
	expect_error("   ", "empty");
	expect_error("1+", "too soon");
	expect_error("(1", "too soon");
	expect_error("1)", "syntax");
	expect_error("()", "syntax");
	expect_error("1 2", "syntax");
	expect_error("*1", "syntax");
	expect_error("byte[1", "too soon");
	expect_error("byte[]", "syntax");
	expect_error("1:2:3", "syntax");
	expect_error("nosuchname", "no symbol");
	expect_error("A=5", "use ==");
	expect_error("byte[$10]=5", "use ==");
	expect_error("bp_add C000", "no symbol");
	expect_error("reset", "no symbol");
	expect_error("watch_clear", "no symbol");
	expect_error("regs_all", "no symbol");
	expect_error("\x80", "syntax");
}

static void
test_limits(void)
{
	char text[1200];
	dbg_expr_value_t v;
	char err[160];

	memset(text, ' ', DBG_EXPR_MAX_TEXT - 1);
	text[DBG_EXPR_MAX_TEXT - 1] = '1';
	text[DBG_EXPR_MAX_TEXT] = '\0';
	check(eval(text, &v, err, sizeof err) && v.value == 1, "the longest text is accepted");
	memset(text, ' ', DBG_EXPR_MAX_TEXT);
	text[DBG_EXPR_MAX_TEXT] = '1';
	text[DBG_EXPR_MAX_TEXT + 1] = '\0';
	expect_error(text, "longer than");

	int n = 0;
	for (int i = 0; i < DBG_EXPR_MAX_DEPTH; i++) text[n++] = '(';
	text[n++] = '1';
	for (int i = 0; i < DBG_EXPR_MAX_DEPTH; i++) text[n++] = ')';
	text[n] = '\0';
	check(eval(text, &v, err, sizeof err) && v.value == 1, "the deepest parentheses are accepted");

	n = 0;
	for (int i = 0; i < DBG_EXPR_MAX_DEPTH + 1; i++) text[n++] = '(';
	text[n++] = '1';
	for (int i = 0; i < DBG_EXPR_MAX_DEPTH + 1; i++) text[n++] = ')';
	text[n] = '\0';
	expect_error(text, "nested");

	memset(text, '-', DBG_EXPR_MAX_DEPTH);
	text[DBG_EXPR_MAX_DEPTH] = '1';
	text[DBG_EXPR_MAX_DEPTH + 1] = '\0';
	check(eval(text, &v, err, sizeof err) && v.value == 1, "the longest unary chain is accepted");
	memset(text, '!', DBG_EXPR_MAX_DEPTH + 1);
	text[DBG_EXPR_MAX_DEPTH + 1] = '1';
	text[DBG_EXPR_MAX_DEPTH + 2] = '\0';
	expect_error(text, "nested");

	n = 0;
	for (int i = 0; i < DBG_EXPR_MAX_DEPTH; i++) n += sprintf(text + n, "byte[");
	text[n++] = '0';
	for (int i = 0; i < DBG_EXPR_MAX_DEPTH; i++) text[n++] = ']';
	text[n] = '\0';
	check(eval(text, &v, err, sizeof err), "the deepest memory nesting is accepted");

	n = 0;
	for (int i = 0; i < DBG_EXPR_MAX_DEPTH + 1; i++) n += sprintf(text + n, "byte[");
	text[n++] = '0';
	for (int i = 0; i < DBG_EXPR_MAX_DEPTH + 1; i++) text[n++] = ']';
	text[n] = '\0';
	expect_error(text, "nested");

	// 1+1+... compiles to two ops per term.
	n = 0;
	text[n++] = '1';
	for (int i = 0; i < (DBG_EXPR_MAX_OPS - 1) / 2; i++) {
		text[n++] = '+';
		text[n++] = '1';
	}
	text[n] = '\0';
	check(eval(text, &v, err, sizeof err) && v.value == (uint32_t)(DBG_EXPR_MAX_OPS - 1) / 2 + 1,
	      "the largest compiled form is accepted");
	text[n++] = '+';
	text[n++] = '1';
	text[n] = '\0';
	expect_error(text, "too long");

	// Each ( defers a value and an operator.
	n = 0;
	for (int i = 0; i < 12; i++) n += sprintf(text + n, "1|1^1&1==1<1<<1+1*(");
	text[n++] = '1';
	for (int i = 0; i < 12; i++) text[n++] = ')';
	text[n] = '\0';
	expect_error(text, "too complex");

	char big[DBG_EXPR_MAX_NAME + 2];
	memset(big, 'n', DBG_EXPR_MAX_NAME + 1);
	big[DBG_EXPR_MAX_NAME + 1] = '\0';
	expect_error(big, "too long");
}

static void
test_registers_and_names(void)
{
	memset(reg_values, 0, sizeof reg_values);
	reg_values[DBG_EXPR_REG_A] = 0x12;
	reg_values[DBG_EXPR_REG_X] = 0x34;
	reg_values[DBG_EXPR_REG_Y] = 0x56;
	reg_values[DBG_EXPR_REG_SP] = 0xF0;
	reg_values[DBG_EXPR_REG_P] = 0x24;
	reg_values[DBG_EXPR_REG_PC] = 0xC123;
	reg_values[DBG_EXPR_REG_R0] = 0x1000;
	reg_values[DBG_EXPR_REG_R15] = 0x2000;
	reg_values[DBG_EXPR_REG_RAMBANK] = 3;
	reg_values[DBG_EXPR_REG_ROMBANK] = 4;

	expect("A", 0x12);
	expect("a", 0x12);
	expect("X", 0x34);
	expect("y", 0x56);
	expect("SP", 0xF0);
	expect("sp", 0xF0);
	expect("P", 0x24);
	expect("PC", 0xC123);
	expect("pc+1", 0xC124);
	expect("R0", 0x1000);
	expect("r15", 0x2000);
	expect("rambank", 3);
	expect("ROMBANK", 4);
	expect("A+X*2", 0x12 + 0x34 * 2);

	expect("main", 0x0810);
	expect("main+4", 0x0814);
	expect("main-1", 0x080F);
	expect("main + 4", 0x0814);
	expect("fade", 0x1234);
	expect("sp1", 0x51);
	expect("R16", 0x52);
	expect("a1", 0x53);
	expect("Ready", 0x54);
	expect("@local", 0x55);
	expect("scope.name", 0x56);
	expect("byte", 0x57);
	expect("bp_clear", 0x58);
	expect_error("beef", "no symbol");
	expect_error("C000", "no symbol");
	expect_error("R99", "no symbol");
	expect_error("R01", "no symbol");

	check(!strcmp(dbg_expr_reg_name(DBG_EXPR_REG_R0), "R0"), "R0 is named");
	check(!strcmp(dbg_expr_reg_name(DBG_EXPR_REG_R15), "R15"), "R15 is named");
	check(!strcmp(dbg_expr_reg_name(DBG_EXPR_REG_ROMBANK), "rombank"), "rombank is named");
	check(!strcmp(dbg_expr_reg_name(DBG_EXPR_REG_COUNT), ""), "past the end has no name");
}

static void
test_memory(void)
{
	memset(cpu, 0, sizeof cpu);
	memset(high, 0, sizeof high);
	memset(banked, 0, sizeof banked);
	memset(vram, 0, sizeof vram);
	cpu[0x0400] = 0x12;
	cpu[0x0401] = 0x34;
	cpu[0xFFFF] = 0xAA;
	cpu[0x0000] = 0xBB;
	cpu[0x0810] = 0x77;
	cpu[0x0811] = 0x88;
	cpu[0xA100] = 0x01;
	cpu[0xA101] = 0x02;
	high[0xC000] = 0x99;
	banked[2][0x0100] = 0xC1;
	banked[2][0x0101] = 0xC7;
	cpu[0x00C1] = 0x3C;
	banked[3][0x0000] = 0xD1;
	vram[0x1B000] = 0x5A;
	vram[0x1FFFF] = 0xA5;

	expect("byte[$0400]", 0x12);
	expect("BYTE[$0400]", 0x12);
	expect("byte [ $0400 ]", 0x12);
	expect("word[$0400]", 0x3412);
	expect("byte[$0400+1]", 0x34);
	expect("byte[$01C000]", 0x99);
	expect("word[$FFFF]", 0xBBAA);
	expect("byte[byte[$0401]+$03CC]", 0x12);
	expect("vbyte[$1B000]", 0x5A);
	expect("vbyte[$1FFFF]", 0xA5);

	expect("byte[main]", 0x77);
	expect("word[main]", 0x8877);
	expect("byte[main+1]", 0x88);
	expect("byte[data]", 0xC1);
	expect("byte[data+1]", 0xC7);
	expect("byte[1+data]", 0xC7);
	expect("word[data]", 0xC7C1);
	expect("byte[other]", 0xD1);

	expect("byte[2:$A100]", 0xC1);
	expect("word[2:$A100]", 0xC7C1);
	expect("byte[2:$A100+1]", 0xC7);
	expect("2:$A100", 0xC1);
	expect("2:$A100+1", 0xC2);
	expect("(2:$A100)+1", 0xC2);
	expect("1+2:$A100", 0xC2);
	expect("2:$A100==$C1", 1);
	expect("$C1==2:$A100", 1);
	expect("2:($A100+1)", 0xC7);
	expect("(1+1):$A100", 0xC1);
	expect("byte[1+1:$A100]", 0xC1);
	expect("byte[(2:$A100)]", 0x3C);
	expect("byte[0:$A100]", 0);
	expect("byte[3:data]", 0);

	// A difference of two labels is a size, with no bank.
	expect_bank("data", 2);
	expect_bank("data+1", 2);
	expect_bank("1+data", 2);
	expect_bank("data-1", 2);
	expect_bank("data-other", -1);
	expect_bank("data+other", -1);
	expect_bank("data*1", -1);
	expect_bank("-data", -1);
	expect_bank("main", -1);
	expect_bank("byte[data]", -1);

	expect_error("byte[$1000000]", "out of range");
	expect_error("byte[-1]", "out of range");
	expect_error("word[$FFFFFFFF]", "out of range");
	expect_error("vbyte[$20000]", "out of range");
	expect_error("vbyte[2:$100]", "no banks");
	expect_error("byte[256:$A000]", "out of range");
	expect_error("byte[2:3:$A000]", "syntax");
	expect_error("2:$10000", "16-bit");
	expect_error("byte[data+$10000]", "16-bit");
	expect_error("word[2:$BFFF]", "crosses");
	expect_error("word[data+$1EFF]", "crosses");
}

static void
expect_target(const char *text, dbg_expr_target_kind_t kind, uint32_t addr, int bank)
{
	dbg_expr_t e;
	dbg_expr_target_t t;
	char err[160] = "", what[640];
	const bool ok = dbg_expr_compile(text, &env, &e, err, sizeof err) &&
	                dbg_expr_target(&e, &env, &t, err, sizeof err);
	snprintf(what, sizeof what, "%s is assignable (%s)", text, err);
	check(ok && t.kind == kind && t.addr == addr && t.bank == bank, what);
}

static void
expect_no_target(const char *text, const char *needle)
{
	dbg_expr_t e;
	dbg_expr_target_t t;
	char err[160] = "", what[640];
	const bool compiled = dbg_expr_compile(text, &env, &e, err, sizeof err);
	const bool ok = compiled && dbg_expr_target(&e, &env, &t, err, sizeof err);
	snprintf(what, sizeof what, "%s is refused with \"%s\" (%s)", text, needle, err);
	check(compiled && !ok && strstr(err, needle), what);
}

static void
test_targets(void)
{
	memset(cpu, 0, sizeof cpu);
	cpu[0x20] = 0x40;
	reg_values[DBG_EXPR_REG_X] = 4;

	dbg_expr_t e;
	dbg_expr_target_t t;
	char err[160] = "";
	check(dbg_expr_compile("x", &env, &e, err, sizeof err) &&
	      dbg_expr_target(&e, &env, &t, err, sizeof err) &&
	      t.kind == DBG_EXPR_TARGET_REG && t.reg == DBG_EXPR_REG_X, "a register is assignable");
	check(dbg_expr_compile("r3", &env, &e, err, sizeof err) &&
	      dbg_expr_target(&e, &env, &t, err, sizeof err) &&
	      t.kind == DBG_EXPR_TARGET_REG && t.reg == DBG_EXPR_REG_R0 + 3, "R3 is assignable");

	expect_target("byte[$400]", DBG_EXPR_TARGET_BYTE, 0x400, -1);
	expect_target("(byte[$400])", DBG_EXPR_TARGET_BYTE, 0x400, -1);
	expect_target("word[$400+X]", DBG_EXPR_TARGET_WORD, 0x404, -1);
	expect_target("byte[byte[$20]]", DBG_EXPR_TARGET_BYTE, 0x40, -1);
	expect_target("vbyte[$1B000]", DBG_EXPR_TARGET_VBYTE, 0x1B000, -1);
	expect_target("byte[2:$A100]", DBG_EXPR_TARGET_BYTE, 0xA100, 2);
	expect_target("word[2:$A100]", DBG_EXPR_TARGET_WORD, 0xA100, 2);
	expect_target("2:$A100", DBG_EXPR_TARGET_BYTE, 0xA100, 2);
	expect_target("(2:$A100)", DBG_EXPR_TARGET_BYTE, 0xA100, 2);
	expect_target("byte[data+1]", DBG_EXPR_TARGET_BYTE, 0xA101, 2);
	expect_target("byte[$10&&$400]", DBG_EXPR_TARGET_BYTE, 1, -1);

	expect_no_target("1", "cannot assign");
	expect_no_target("main", "cannot assign");
	expect_no_target("A+1", "cannot assign");
	expect_no_target("byte[$400]+1", "cannot assign");
	expect_no_target("!byte[$400]", "cannot assign");
	expect_no_target("2:$A100+1", "cannot assign");
	expect_no_target("byte[$1000000]", "out of range");
	expect_no_target("word[2:$BFFF]", "crosses");
	expect_no_target("byte[1/0]", "division");
}

static void
test_access_operands(void)
{
	static const struct { const char *name; int reg; } names[] = {
		{"val", DBG_EXPR_ACCESS_VAL},         {"addr", DBG_EXPR_ACCESS_ADDR},
		{"is_read", DBG_EXPR_ACCESS_IS_READ}, {"IS_WRITE", DBG_EXPR_ACCESS_IS_WRITE},
	};
	dbg_expr_env_t watch = env;
	watch.access = true;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		dbg_expr_t       e;
		dbg_expr_value_t v = {0, -1};
		char             err[160] = "", what[96];
		const bool ok = dbg_expr_compile(names[i].name, &watch, &e, err, sizeof err) &&
		                dbg_expr_eval(&e, &watch, &v, err, sizeof err);
		snprintf(what, sizeof what, "%s reads its operand", names[i].name);
		check(ok && v.value == 0x100u + (uint32_t)names[i].reg, what);
		snprintf(what, sizeof what, "%s is not an operand without access", names[i].name);
		check(!dbg_expr_compile(names[i].name, &env, &e, err, sizeof err), what);
	}
	check((int)DBG_EXPR_ACCESS_VAL == (int)DBG_EXPR_REG_COUNT, "access operands follow the registers");
	check(dbg_expr_reg_name((dbg_expr_reg_t)(DBG_EXPR_REG_COUNT - 1))[0] != '\0', "last register named");
	check(dbg_expr_reg_name(DBG_EXPR_REG_COUNT)[0] == '\0', "the register list ends at COUNT");
}

int
main(void)
{
	test_numbers();
	test_width_and_overflow();
	test_operators();
	test_precedence();
	test_short_circuit();
	test_errors();
	test_limits();
	test_registers_and_names();
	test_memory();
	test_targets();
	test_access_operands();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
