// Commander X16 Emulator — HTTP request checks for the MCP server
//
// Decides what to do with the bytes of one HTTP request. No sockets and no
// emulator state, so every refusal can be tested on its own.

#ifndef _MCP_HTTP_H_
#define _MCP_HTTP_H_

#include <stdbool.h>
#include <stddef.h>

#define MCP_HTTP_MAX_HEADERS (8 * 1024)
#define MCP_HTTP_MAX_BODY    (64 * 1024)
#define MCP_HTTP_MAX_REQUEST (MCP_HTTP_MAX_HEADERS + MCP_HTTP_MAX_BODY)

typedef enum {
    MCP_HTTP_INCOMPLETE,   // more bytes are needed
    MCP_HTTP_READY,        // a request to dispatch
    MCP_HTTP_REFUSED,      // answer with `status`, then close
} mcp_http_state_t;

typedef struct {
    int         status;            // REFUSED: the response code
    const char *message;           // REFUSED: why
    const char *body;              // READY: inside the caller's buffer
    size_t      body_len;
    size_t      consumed;          // READY: bytes this request took up
    bool        keep_alive;
    bool        expects_continue;  // INCOMPLETE: the client waits for 100 Continue
} mcp_http_request_t;

// `port` is the one this server listens on, which a Host header has to name.
mcp_http_state_t mcp_http_parse(const char *buf, size_t len, int port, mcp_http_request_t *out);

const char *mcp_http_reason(int status);

// The MCP revisions this server speaks, newest first.
extern const char *const mcp_protocol_versions[];
extern const int         mcp_protocol_version_count;

bool mcp_protocol_version_supported(const char *version, size_t len);

#endif
