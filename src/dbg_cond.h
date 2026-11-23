// Expression conditions on breakpoints and watches: the expression engine,
// reached by debug_core through its debug_cond_* hooks.
//
// A condition that cannot be judged stops the machine and says why, because
// staying quiet would look like the condition being false.

#ifndef DBG_COND_H
#define DBG_COND_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dbg_cond dbg_cond_t;

// Does `text` compile? `access` allows val, addr, is_read and is_write.
bool dbg_cond_check(const char *text, bool access, char *err, size_t errn);

// NULL only when out of memory. Text that no longer compiles is kept, and is
// tried again whenever the debug symbols change.
dbg_cond_t *dbg_cond_new(const char *text, bool access);
void        dbg_cond_free(dbg_cond_t *cond);
const char *dbg_cond_text(const dbg_cond_t *cond);

// A condition that could not be judged on the instruction the machine is at.
bool dbg_cond_last_error(const char **text, const char **message);

#ifdef __cplusplus
}
#endif

#endif
