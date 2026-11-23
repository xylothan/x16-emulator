// Debugger expressions: parsed once to a small compiled form, then evaluated
// against read functions the caller supplies. This module writes nothing and
// depends on nothing in the emulator, so it links alone in a unit test.
//
//   operators   || && | ^ & == != < <= > >= << >> + - * / %   unary - ! ~
//   operands    $hex 0xhex decimal
//               A X Y SP P PC R0-R15 rambank rombank
//               byte[e] word[e] vbyte[e], byte[b:a] word[b:a], b:a
//               name, name+offset
//               val addr is_read is_write, in a watch condition only
//
// Values are unsigned 32-bit and arithmetic wraps. Comparison and division are
// unsigned, so -1 is $FFFFFFFF and is not less than 0. A shift by 32 or more
// gives 0. Precedence and associativity are C's. && and || give 0 or 1 and do
// not evaluate their right side when the left decides.
//
// A bare word is a register, then a symbol, and never hex. A decimal has no
// leading zero, since 0400 would otherwise mean different things here and in
// the debug console, where it is hex.
//
// b:a names X16 RAM or ROM bank b at address a. The high byte of a 24-bit
// address is the 65C816 bank, which is a different thing. A symbol keeps its
// bank through + and - with a value that has none, so byte[label+1] reads the
// label's own bank rather than whichever one is mapped. Inside byte[] and word[]
// each half is a whole expression. Elsewhere b:a is one operand that reads a
// byte, so 2:$A000+1 adds 1 to that byte and x == 2:$A000 compares with it.

#ifndef DBG_EXPR_H
#define DBG_EXPR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBG_EXPR_MAX_TEXT  256   // longest expression, in bytes
#define DBG_EXPR_MAX_NAME  127   // longest symbol name
#define DBG_EXPR_MAX_DEPTH 32    // nested (), [] and unary operators
#define DBG_EXPR_MAX_OPS   128   // compiled size
#define DBG_EXPR_MAX_STACK 48    // values pending at once

#define DBG_EXPR_NO_BANK (-1)

typedef enum {
	DBG_EXPR_REG_A,
	DBG_EXPR_REG_X,
	DBG_EXPR_REG_Y,
	DBG_EXPR_REG_SP,
	DBG_EXPR_REG_P,
	DBG_EXPR_REG_PC,
	DBG_EXPR_REG_R0,                                   // R1-R15 follow in order
	DBG_EXPR_REG_R15 = DBG_EXPR_REG_R0 + 15,
	DBG_EXPR_REG_RAMBANK,
	DBG_EXPR_REG_ROMBANK,
	DBG_EXPR_REG_COUNT
} dbg_expr_reg_t;

// The access a watch condition is judged on, read through read_reg. Named only
// when the environment sets `access`. Everywhere else they are ordinary symbols,
// so a program's own label called addr or val keeps working.
typedef enum {
	DBG_EXPR_ACCESS_VAL = DBG_EXPR_REG_COUNT,
	DBG_EXPR_ACCESS_ADDR,
	DBG_EXPR_ACCESS_IS_READ,
	DBG_EXPR_ACCESS_IS_WRITE,
	DBG_EXPR_ACCESS_END
} dbg_expr_access_t;

typedef struct {
	void *user;
	// `addr` is 24-bit. `x16_bank` is a RAM or ROM bank, or DBG_EXPR_NO_BANK for
	// whichever is mapped.
	uint8_t  (*read_cpu)(void *user, uint32_t addr, int x16_bank);
	uint8_t  (*read_vram)(void *user, uint32_t addr);
	uint32_t (*read_reg)(void *user, dbg_expr_reg_t reg);
	// Called while compiling. On failure, `err` says why.
	bool     (*symbol)(void *user, const char *name, uint32_t *value, int *bank,
	                   char *err, size_t errn);
	bool     access;    // val, addr, is_read and is_write are operands
} dbg_expr_env_t;

typedef struct {
	uint8_t  code;
	int16_t  bank;
	uint32_t value;
} dbg_expr_op_t;

typedef struct {
	dbg_expr_op_t ops[DBG_EXPR_MAX_OPS];
	int           count;
} dbg_expr_t;

typedef struct {
	uint32_t value;
	int      bank;      // the X16 bank it carries, or DBG_EXPR_NO_BANK
} dbg_expr_value_t;

typedef enum {
	DBG_EXPR_TARGET_REG,
	DBG_EXPR_TARGET_BYTE,
	DBG_EXPR_TARGET_WORD,
	DBG_EXPR_TARGET_VBYTE,
} dbg_expr_target_kind_t;

// A place an assignment can store to.
typedef struct {
	dbg_expr_target_kind_t kind;
	dbg_expr_reg_t         reg;    // for DBG_EXPR_TARGET_REG
	uint32_t               addr;
	int                    bank;   // X16 bank, or DBG_EXPR_NO_BANK
} dbg_expr_target_t;

// On failure, `err` says why and `out` is not usable.
bool dbg_expr_compile(const char *text, const dbg_expr_env_t *env, dbg_expr_t *out,
                      char *err, size_t errn);
bool dbg_expr_eval(const dbg_expr_t *expr, const dbg_expr_env_t *env, dbg_expr_value_t *out,
                   char *err, size_t errn);

// What `expr` names, if it can be assigned to: a register, or a whole byte[],
// word[], vbyte[] or b:a. Evaluates the address and reads no target memory.
bool dbg_expr_target(const dbg_expr_t *expr, const dbg_expr_env_t *env, dbg_expr_target_t *out,
                     char *err, size_t errn);

// The name of a register, for listing them.
const char *dbg_expr_reg_name(dbg_expr_reg_t reg);

#ifdef __cplusplus
}
#endif

#endif
