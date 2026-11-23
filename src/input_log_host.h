// Commander X16 Emulator
// The input log, wired to the emulator's devices.

#ifndef _INPUT_LOG_HOST_H_
#define _INPUT_LOG_HOST_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void input_log_host_init(void);
// Starts a recording, with what the host controllers are holding as its first
// events, since a save state does not hold them.
bool input_log_host_start(void);

#ifdef __cplusplus
}
#endif

#endif
