// Commander X16 Emulator — Model Context Protocol server
//
// -mcp speaks MCP (JSON-RPC, one message per line) on stdin and stdout, so that
// an agent can launch the emulator and drive its debugger. -mcp <port> serves
// the same tools over HTTP on loopback, for an agent attaching to an emulator
// that is already running. Every tool is a thin wrapper over a debug server
// request, so it behaves as it does over DAP.

#ifndef _MCP_SERVER_H_
#define _MCP_SERVER_H_

#include <stdbool.h>
#include <stdint.h>

#ifndef HAS_DAP

static inline int  mcp_server_init_stdio(void) { return -1; }
static inline int  mcp_server_init_http(int port) { (void)port; return -1; }
static inline int  mcp_server_sockets(uintptr_t *out, int max) { (void)out; (void)max; return 0; }
static inline int  mcp_server_poll(void) { return 0; }
static inline bool mcp_server_is_enabled(void) { return false; }
static inline void mcp_server_shutdown(void) { }
static inline void mcp_server_note_stopped(void) { }
static inline void mcp_server_note_resumed(void) { }
static inline void mcp_server_set_readonly(bool on) { (void)on; }
static inline bool mcp_server_is_readonly(void) { return false; }

#else

// Take over stdout for the protocol and start reading stdin. Anything else the
// emulator prints goes to stderr from here on, so this has to run before the
// first printf. Returns 0 on success.
int  mcp_server_init_stdio(void);

// Listen on 127.0.0.1:<port>, at /mcp. Returns 0 on success.
int  mcp_server_init_http(int port);

// The sockets a paused machine should wake for, at most `max`. None under stdio.
#define MCP_SERVER_MAX_SOCKETS 5
int  mcp_server_sockets(uintptr_t *out, int max);

// Handle what has arrived. Called from debug_server_poll, on the emulation
// thread. Returns 1 if a request changed the run state.
int  mcp_server_poll(void);

bool mcp_server_is_enabled(void);
void mcp_server_shutdown(void);

// The machine stopped, or was set going. Told as it happens, because a stop and
// a resume can both land between two polls and each stop has to be counted.
void mcp_server_note_stopped(void);
void mcp_server_note_resumed(void);

// Offer and accept only the tools that change nothing. Set before the first poll.
void mcp_server_set_readonly(bool on);
bool mcp_server_is_readonly(void);

#endif // HAS_DAP

#endif
