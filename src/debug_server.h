// Commander X16 Emulator — Debug Adapter Protocol (DAP) Server
// Speaks DAP over TCP for direct IDE integration (VS, VS Code, etc.)
// Transport: Content-Length framed JSON over TCP, default port 9009

#ifndef _DEBUG_SERVER_H_
#define _DEBUG_SERVER_H_

#include <stdbool.h>
#include <stdint.h>

#define DEBUG_SERVER_DEFAULT_PORT 9009

// The server needs cJSON, which is optional at configure time. When it is not
// available the emulator is built without it and every entry point below
// becomes a no-op, so callers -- the CPU loop, the debugger, the store path --
// do not have to be littered with #ifdefs for a feature they only notify.
#ifndef HAS_DAP

static inline int  debug_server_init(int port) { (void)port; return -1; }
static inline int  debug_server_poll(void) { return 0; }
static inline void debug_server_notify_stopped(const char *reason) { (void)reason; }
static inline bool debug_server_has_client(void) { return false; }
static inline bool debug_server_is_enabled(void) { return false; }
static inline void debug_server_output(const char *category, const char *text) { (void)category; (void)text; }
static inline void debug_server_invalidate_breakpoints_in_range(uint32_t start, uint32_t end) { (void)start; (void)end; }
static inline void debug_server_retry_unverified_breakpoints(void) { }
static inline void debug_server_shutdown(void) { }

#else

// Start listening on the given port. Returns 0 on success, -1 on error.
int  debug_server_init(int port);

// Poll for client commands. Non-blocking.
// Returns: 0 = no action, 1 = mode was changed by a remote command
int  debug_server_poll(void);

// Send DAP "stopped" event to the connected client.
// reason: "breakpoint", "step", "user", etc.
void debug_server_notify_stopped(const char *reason);

// Returns true if a debug client is currently connected.
bool debug_server_has_client(void);

// Returns true if the DAP server is enabled (--debugport was used).
bool debug_server_is_enabled(void);

// Forward text to a connected DAP client's debug console (OutputEvent). No-op
// if no client is connected. category is "console"/"stdout"/"stderr"/etc.
void debug_server_output(const char *category, const char *text);

// Breakpoint bookkeeping hooks driven by the shared runtime-load handler when a
// module is (re)loaded. No-ops when no DAP client is connected.
void debug_server_invalidate_breakpoints_in_range(uint32_t start, uint32_t end);
void debug_server_retry_unverified_breakpoints(void);

// Clean shutdown — close sockets, free resources.
void debug_server_shutdown(void);

#endif // HAS_DAP

#endif
