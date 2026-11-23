// Commander X16 Emulator — c_frames, wired to the emulator.

#ifndef _C_FRAMES_HOST_H_
#define _C_FRAMES_HOST_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once, when the command line has settled whether a debugger is present.
// From then on the flow hook is taken while one is and the loaded debug info
// has C functions and a C stack pointer; otherwise it stays NULL.
//
// A snapshot on purpose. The guest can clear debugger_enabled through $9FB0,
// and following it would lose the frames open at that moment.
void c_frames_host_start(bool debugger);

#ifdef __cplusplus
}
#endif

#endif
