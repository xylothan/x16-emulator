// Logpoints: a breakpoint that prints and keeps running.
//
// debug_core.c decides when one fires and calls here through a function pointer,
// since it links without the rest of the emulator in some tests. A firing is
// formatted into a bounded ring and nothing is sent from the CPU's path: each
// front end reads the ring when it next polls.

#ifndef LOGPOINT_H
#define LOGPOINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dbg_log.h"
#include "debug_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Make `owner`'s claim on the breakpoint a logpoint, replacing any message it
// had. The owner must already hold the breakpoint. False, with `err` set and
// nothing changed, for a malformed message.
bool logpoint_set(int pc, uint8_t bank, int x16Bank, debug_owner_t owner, const char *message,
                  char *err, size_t errn);
void logpoint_clear(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);
// The owner's message here, or NULL. Valid until that logpoint changes.
const char *logpoint_message(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);

dbg_log_ring_t *logpoint_ring(void);

#ifdef __cplusplus
}
#endif

#endif
