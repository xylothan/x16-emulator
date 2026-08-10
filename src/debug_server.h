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
static inline void debug_server_note_resumed(void) { }
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

// Breakpoints are owned, not reconstructed. debug_core records which of -bp,
// the SDL debugger's F9, and the four kinds this file can ask for wants each
// entry, so a client session can clear everything it asked for with
// debug_bp_clear_owner() and nothing else is disturbed. This replaced an
// ownership model rebuilt from outside the core (ext_keys[],
// server_bp_wanted_elsewhere(), per-table `external` flags), which is where
// most of this file's defects were found. See docs/breakpoint-ownership.md.
//
// KNOWN LIMITATION, and a deliberate one: the core is the single source of
// truth for what is ARMED, but each front end owns its own VIEW. A breakpoint
// the user sets with F9 does not appear in a connected client's UI, and one the
// user deletes with F9 may be sent again by the client -- DAP's setBreakpoints
// replaces a source's whole list from the client's own model, and the
// specification tells clients to keep using their own properties rather than
// the adapter's. Emitting `breakpoint` events with reason `new`/`removed` would
// paper over this in VS Code specifically, but nothing in the protocol requires
// a client to act on them, so the divergence is documented rather than faked.

// The machine has resumed. Called from the debugger's own execution control, so
// that a local F5/F10/F11 is accounted for the same way a DAP continue is: the
// next stop is then reported once, rather than suppressed as a duplicate of a
// halt that is already over.
void debug_server_note_resumed(void);

// Clean shutdown — close sockets, free resources.
void debug_server_shutdown(void);

#endif // HAS_DAP

#endif
