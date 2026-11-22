// Logpoint messages and the ring their output waits in. Depends on nothing in
// the emulator: expressions read the machine through the caller's environment,
// so this links alone in a unit test.
//
// A message is literal text with {expression} parts, in the language of
// dbg_expr.h. {{ and }} are literal braces. A message whose braces do not pair
// up is refused when it is set. A part that does not compile or evaluate is not:
// it prints as {text: reason}, and the machine keeps running.
//
// Values print in hex, as $05, $1234, or $02:A000 for one that carries a bank.
//
// Formatting allocates nothing and is bounded by the message limits below, since
// it runs on the CPU's path. A line that would not fit ends in "...".

#ifndef DBG_LOG_H
#define DBG_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dbg_expr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DBG_LOG_MAX_MESSAGE 255    // longest message, in bytes
#define DBG_LOG_MAX_EXPRS   16     // {expressions} in one message
#define DBG_LOG_TEXT        256    // one formatted line, with its NUL
#define DBG_LOG_RING        1024   // lines kept

typedef struct dbg_log_msg dbg_log_msg_t;

// Whether `text` is a well-formed message. Evaluates nothing.
bool dbg_log_check(const char *text, char *err, size_t errn);

// NULL, with `err` set, for a message dbg_log_check refuses or no memory.
dbg_log_msg_t *dbg_log_compile(const char *text, const dbg_expr_env_t *env, char *err,
                               size_t errn);
// Compile its expressions again, for when the symbols they name have changed.
void        dbg_log_recompile(dbg_log_msg_t *m, const dbg_expr_env_t *env);
void        dbg_log_free(dbg_log_msg_t *m);
const char *dbg_log_source(const dbg_log_msg_t *m);
void        dbg_log_format(const dbg_log_msg_t *m, const dbg_expr_env_t *env, char *out,
                           size_t n);

typedef struct {
	uint64_t seq;
	uint32_t pc;
	int16_t  x16_bank;   // the breakpoint's selector, or -1 for any
	uint8_t  bank;       // 65C816 program bank
	uint8_t  owner;      // < 16
	char     text[DBG_LOG_TEXT];
} dbg_log_entry_t;

// Lines are numbered from 0 for the life of the ring, so a reader's position
// stays meaningful across overflow. A full ring overwrites its oldest line.
//
// Any number of readers can follow it by holding a sequence number. One of them
// can instead be the sink: the ring tracks its position, so that it can count
// exactly the lines of the owners it wants that were overwritten before it took
// them.
typedef struct {
	dbg_log_entry_t entries[DBG_LOG_RING];
	uint64_t        next;        // the number the next line gets
	uint16_t        sink_mask;   // owner bits the sink takes
	uint64_t        sink_next;
	uint64_t        sink_lost;
} dbg_log_ring_t;

// The slot for a new line, with its seq and owner set. Valid until the next push.
dbg_log_entry_t *dbg_log_ring_push(dbg_log_ring_t *r, uint8_t owner);
uint64_t         dbg_log_ring_oldest(const dbg_log_ring_t *r);
// NULL if that line is not held.
const dbg_log_entry_t *dbg_log_ring_at(const dbg_log_ring_t *r, uint64_t seq);

// Start the sink at the current end of the ring, taking `owner_mask`.
void dbg_log_ring_sink(dbg_log_ring_t *r, uint16_t owner_mask);
// The sink's next line, or NULL. Adds what was lost since the last call to `*lost`.
const dbg_log_entry_t *dbg_log_ring_take(dbg_log_ring_t *r, uint64_t *lost);

#ifdef __cplusplus
}
#endif

#endif
