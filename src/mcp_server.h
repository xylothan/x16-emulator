// Commander X16 Emulator — Model Context Protocol server
//
// -mcp speaks MCP (JSON-RPC, one message per line) on stdin and stdout, so that
// an agent can launch the emulator and drive its debugger. Every tool is a thin
// wrapper over a debug server request, so it behaves as it does over DAP.

#ifndef _MCP_SERVER_H_
#define _MCP_SERVER_H_

#include <stdbool.h>

#ifndef HAS_DAP

static inline int  mcp_server_init_stdio(void) { return -1; }
static inline int  mcp_server_poll(void) { return 0; }
static inline bool mcp_server_is_enabled(void) { return false; }
static inline void mcp_server_shutdown(void) { }

#else

// Take over stdout for the protocol and start reading stdin. Anything else the
// emulator prints goes to stderr from here on, so this has to run before the
// first printf. Returns 0 on success.
int  mcp_server_init_stdio(void);

// Handle what has arrived. Called from debug_server_poll, on the emulation
// thread. Returns 1 if a request changed the run state.
int  mcp_server_poll(void);

bool mcp_server_is_enabled(void);
void mcp_server_shutdown(void);

#endif // HAS_DAP

#endif
