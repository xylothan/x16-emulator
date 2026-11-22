// Commander X16 Emulator — Model Context Protocol server
//
// Over stdio, two small threads move bytes and touch nothing else: one blocks on
// stdin and queues complete lines, the other blocks on stdout and drains a queue
// of replies. Over HTTP, non-blocking sockets are serviced from the poll.
// Everything that reads or changes the machine runs on the emulation thread,
// from mcp_server_poll. A client that stops reading therefore never stalls
// emulation, and neither does a client that sends nothing.

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
#endif

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "mcp_server.h"
#include "mcp_http.h"
#include "debug_server_call.h"
#include "debugger.h"
#include "debug_core.h"
#include "dbg_info.h"
#include "perf_budget.h"
#include "sdcard.h"
#include "version.h"

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
  #define MCP_READ(fd, buf, n)  _read((fd), (buf), (unsigned)(n))
  #define MCP_WRITE(fd, buf, n) _write((fd), (buf), (unsigned)(n))
  #define MCP_DUP               _dup
  #define MCP_DUP2              _dup2
  #define MCP_NULL_DEVICE       "NUL"
  #define MCP_OPEN              _open
  #define MCP_O_WRONLY          _O_WRONLY
  typedef SOCKET mcp_sock_t;
  #define MCP_NO_SOCK           INVALID_SOCKET
  #define MCP_CLOSE_SOCK        closesocket
  #define MCP_WOULD_BLOCK       (WSAGetLastError() == WSAEWOULDBLOCK)
  #define MCP_SHUT_WRITE        SD_SEND
#else
  #include <fcntl.h>
  #include <signal.h>
  #include <unistd.h>
  #define MCP_READ(fd, buf, n)  read((fd), (buf), (n))
  #define MCP_WRITE(fd, buf, n) write((fd), (buf), (n))
  #define MCP_DUP               dup
  #define MCP_DUP2              dup2
  #define MCP_NULL_DEVICE       "/dev/null"
  #define MCP_OPEN              open
  #define MCP_O_WRONLY          O_WRONLY
  typedef int mcp_sock_t;
  #define MCP_NO_SOCK           (-1)
  #define MCP_CLOSE_SOCK        close
  #define MCP_WOULD_BLOCK       (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
  #define MCP_SHUT_WRITE        SHUT_WR
#endif

#define MCP_MAX_LINE       (1024 * 1024)
#define MCP_MAX_QUEUED     256                 // lines read ahead of the machine
#define MCP_MAX_OUT_BYTES  (64u * 1024 * 1024) // past this the client is gone
#define MCP_LINES_PER_POLL 16
#define MCP_READ_LIMIT     4096
#define MCP_DISASM_LIMIT   256
#define MCP_WAIT_DEFAULT   5000                // ms a long-running call waits
#define MCP_WAIT_LIMIT     30000
#define MCP_FRAMES_LIMIT   1000000000
#define MCP_MAX_CONNS      4
#define MCP_IDLE_MS        30000               // an HTTP connection with nothing to do
#define MCP_DRAIN_MS       1000                // reading off a refused request's body
#define MCP_LEASE_MS       (10 * 60 * 1000)    // an HTTP client with no calls

#define RPC_PARSE_ERROR      (-32700)
#define RPC_INVALID_REQUEST  (-32600)
#define RPC_METHOD_NOT_FOUND (-32601)
#define RPC_INVALID_PARAMS   (-32602)

typedef struct in_line {
    struct in_line *next;
    char           *text;
} in_line;

typedef struct out_msg {
    struct out_msg *next;
    size_t          len;
    char           *data;
} out_msg;

static bool       enabled = false;
static bool       read_only = false;
static bool       quit_sent = false;
static int        out_fd = -1;

static SDL_mutex *lock = NULL;
static SDL_cond  *in_space = NULL;   // the reader waits here when the queue is full
static SDL_cond  *out_ready = NULL;  // the writer waits here for something to send
static SDL_cond  *out_idle = NULL;   // shutdown waits here for the queue to drain

static in_line   *in_head = NULL, *in_tail = NULL;
static int        in_count = 0;
static bool       in_eof = false;

static out_msg   *out_head = NULL, *out_tail = NULL;
static size_t     out_bytes = 0;
static bool       out_busy = false;
static bool       out_broken = false;
static bool       out_stop = false;

// Stops, counted at the one place every path into a stop passes through, so
// that a stop landing between two calls cannot be missed.
static uint32_t   stop_id = 0;
static bool       was_stopped = false;

// One HTTP client connection. A request is answered before the next is read, so
// one that is waiting on the machine holds its place in the stream.
typedef struct {
    mcp_sock_t sock;
    char      *in;
    size_t     in_len;
    char      *out;
    size_t     out_len, out_off, out_cap;
    Uint32     touched;        // last time bytes moved
    Uint32     drain_until;
    bool       fresh;          // bytes arrived that have not been looked at
    bool       keep_alive;     // of the request being answered
    bool       waiting;        // a deferred call answers on this connection
    bool       closing;        // close once the reply has gone
    bool       half_closed;    // reply sent; discarding whatever is still coming
    bool       sent_continue;
    bool       peer_closed;
    bool       dead;           // released at the end of the poll, never mid-request
} http_conn;

static bool       http_mode = false;
static int        http_port = 0;
static mcp_sock_t listen_sock = MCP_NO_SOCK;
static http_conn  conns[MCP_MAX_CONNS];
static http_conn *reply_conn = NULL;     // where a reply to this request goes
static int        reply_status = 200;
static bool       replied = false;
static Uint32     idle_ms = MCP_IDLE_MS;

// HTTP has no connection to hang a session on, so a client holds the machine for
// as long as it keeps calling.
static bool       lease_live = false;
static Uint32     lease_end = 0;
static Uint32     lease_ms = MCP_LEASE_MS;

// The one call that answers later: a step, a run of frames, or a wait for a
// stop. Other requests are still served while it waits, on the machine it is
// waiting for, so a client can pause what it started.
typedef enum { WAIT_NONE, WAIT_STOP, WAIT_FRAMES } wait_kind_t;

static struct {
    wait_kind_t kind;
    cJSON      *id;
    Uint32      deadline;
    uint32_t    since;       // WAIT_STOP: answered once stop_id has passed this
    http_conn  *conn;        // HTTP: the connection owed the answer
} waiter;

static const cJSON *current_id = NULL;   // the request being handled

// ─── The two I/O threads ────────────────────────────────────────────

static void in_push(char *text) {
    in_line *l = (in_line *)malloc(sizeof *l);
    if (!l) {
        free(text);
        return;
    }
    l->next = NULL;
    l->text = text;
    SDL_LockMutex(lock);
    while (in_count >= MCP_MAX_QUEUED && !out_stop)
        SDL_CondWait(in_space, lock);
    if (in_tail) in_tail->next = l;
    else         in_head = l;
    in_tail = l;
    in_count++;
    SDL_UnlockMutex(lock);
}

static int SDLCALL reader_main(void *unused) {
    (void)unused;
    static char chunk[8192];
    char   *line = (char *)malloc(MCP_MAX_LINE + 1);
    size_t  len = 0;
    bool    overflow = false;
    if (!line) {
        SDL_LockMutex(lock);
        in_eof = true;
        SDL_UnlockMutex(lock);
        return 0;
    }
    for (;;) {
        const int n = (int)MCP_READ(0, chunk, sizeof chunk);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            const char c = chunk[i];
            if (c != '\n') {
                if (len < MCP_MAX_LINE) line[len++] = c;
                else                    overflow = true;
                continue;
            }
            if (len && line[len - 1] == '\r') len--;
            if (overflow) {
                // Reported as unparseable, since no id can be read from it.
                char *empty = (char *)calloc(1, 1);
                if (empty) in_push(empty);
            } else if (len) {
                char *text = (char *)malloc(len + 1);
                if (text) {
                    memcpy(text, line, len);
                    text[len] = '\0';
                    in_push(text);
                }
            }
            len = 0;
            overflow = false;
        }
    }
    free(line);
    SDL_LockMutex(lock);
    in_eof = true;
    SDL_UnlockMutex(lock);
    return 0;
}

static int SDLCALL writer_main(void *unused) {
    (void)unused;
    for (;;) {
        SDL_LockMutex(lock);
        while (!out_head && !out_stop)
            SDL_CondWait(out_ready, lock);
        out_msg *m = out_head;
        if (!m) {
            SDL_UnlockMutex(lock);
            return 0;
        }
        out_head = m->next;
        if (!out_head) out_tail = NULL;
        out_busy = true;
        const bool broken = out_broken;
        SDL_UnlockMutex(lock);

        bool failed = false;
        for (size_t off = 0; !broken && off < m->len; ) {
            const size_t chunk = m->len - off > (1u << 20) ? (1u << 20) : m->len - off;
            const int w = (int)MCP_WRITE(out_fd, m->data + off, chunk);
            if (w <= 0) {
                failed = true;
                break;
            }
            off += (size_t)w;
        }

        SDL_LockMutex(lock);
        out_bytes -= m->len;
        out_busy = false;
        if (failed) out_broken = true;
        if (!out_head) SDL_CondBroadcast(out_idle);
        SDL_UnlockMutex(lock);
        free(m->data);
        free(m);
    }
}

// Queue one message. Takes ownership of `text`, which has room for a newline.
static void out_send(char *text) {
    const size_t len = strlen(text);
    text[len] = '\n';
    out_msg *m = (out_msg *)malloc(sizeof *m);
    if (!m) {
        free(text);
        return;
    }
    m->next = NULL;
    m->len  = len + 1;
    m->data = text;

    SDL_LockMutex(lock);
    if (out_broken || out_bytes + m->len > MCP_MAX_OUT_BYTES) {
        // Nobody is reading. Dropping it here, not waiting, keeps the machine going.
        out_broken = true;
        SDL_UnlockMutex(lock);
        free(text);
        free(m);
        return;
    }
    out_bytes += m->len;
    if (out_tail) out_tail->next = m;
    else          out_head = m;
    out_tail = m;
    SDL_CondSignal(out_ready);
    SDL_UnlockMutex(lock);
}

// ─── JSON-RPC ───────────────────────────────────────────────────────

static void http_answer(const char *json, size_t len);

static void rpc_send(cJSON *msg) {
    char *printed = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!printed) return;
    size_t len = strlen(printed);
    char *repaired = debug_server_utf8_repaired(printed, len, &len);
    const char *body = repaired ? repaired : printed;
    if (http_mode) {
        http_answer(body, len);
        free(repaired);
        cJSON_free(printed);
        return;
    }
    char *text = (char *)malloc(len + 2);
    if (text) {
        memcpy(text, body, len + 1);
        out_send(text);
    }
    free(repaired);
    cJSON_free(printed);
}

static void rpc_add_id(cJSON *msg, const cJSON *id) {
    cJSON *copy = id ? cJSON_Duplicate(id, 1) : NULL;
    if (copy) cJSON_AddItemToObject(msg, "id", copy);
    else      cJSON_AddNullToObject(msg, "id");
}

static void rpc_result(const cJSON *id, cJSON *result) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    rpc_add_id(msg, id);
    cJSON_AddItemToObject(msg, "result", result);
    rpc_send(msg);
}

static void rpc_error(const cJSON *id, int code, const char *message) {
    // With no id there was no readable request, which over HTTP is a 400.
    if (!id) reply_status = 400;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    rpc_add_id(msg, id);
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(msg, "error", err);
    rpc_send(msg);
}

// ─── Tool results ───────────────────────────────────────────────────

// Every result says what the machine is doing, because another front end may
// have moved it between two calls.
static void track_stops(void) {
    const bool stopped = DEBUGIsPaused();
    if (stopped && !was_stopped) stop_id++;
    was_stopped = stopped;
}

static void add_machine_state(cJSON *data) {
    track_stops();
    const bool paused = DEBUGIsPaused();
    if (!cJSON_HasObjectItem(data, "paused"))
        cJSON_AddBoolToObject(data, "paused", paused);
    // A finished run of frames already says which frame it ended on, and can be
    // reported from inside another request, where this call would be refused.
    if (!cJSON_HasObjectItem(data, "frame")) {
        dbg_reply_t st;
        debug_server_call("x16/status", NULL, &st);
        cJSON *f = st.body ? cJSON_GetObjectItemCaseSensitive(st.body, "frame") : NULL;
        if (f) cJSON_AddNumberToObject(data, "frame", f->valuedouble);
        cJSON_Delete(st.body);
    }
    cJSON_AddNumberToObject(data, "stopId", stop_id);
    const char *why = paused ? DEBUGGetStopReason() : NULL;
    if (why && *why) cJSON_AddStringToObject(data, "stopReason", why);
    else             cJSON_AddNullToObject(data, "stopReason");
    if (!cJSON_HasObjectItem(data, "stop")) debug_server_add_stop_info(data);
}

static cJSON *text_content(const char *text) {
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    return content;
}

// Takes ownership of `data`. With no `text`, the data itself is the text.
static cJSON *tool_ok(cJSON *data, const char *text) {
    add_machine_state(data);
    cJSON *result = cJSON_CreateObject();
    if (text) {
        cJSON_AddItemToObject(result, "content", text_content(text));
    } else {
        char *printed = cJSON_PrintUnformatted(data);
        cJSON_AddItemToObject(result, "content", text_content(printed ? printed : "{}"));
        cJSON_free(printed);
    }
    cJSON_AddItemToObject(result, "structuredContent", data);
    return result;
}

static cJSON *tool_fail(const char *text) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "content", text_content(text));
    cJSON_AddBoolToObject(result, "isError", true);
    return result;
}

// The result of a command with no shaping to do.
static cJSON *tool_passthrough(const char *command) {
    dbg_reply_t r;
    debug_server_call(command, NULL, &r);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    return tool_ok(r.body ? r.body : cJSON_CreateObject(), NULL);
}

// ─── Arguments ──────────────────────────────────────────────────────

static bool arg_int(cJSON *args, const char *name, int def, int lo, int hi, int *out,
                    char *err, size_t errn) {
    cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, name) : NULL;
    if (!v) {
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble) ||
        v->valuedouble < lo || v->valuedouble > hi) {
        snprintf(err, errn, "'%s' must be a whole number from %d to %d", name, lo, hi);
        return false;
    }
    *out = (int)v->valuedouble;
    return true;
}

// An address as a debug server memory reference.
// Whether a CPU address string is written as a number or a bank:addr pair. A
// bare token is a symbol here, because fade and beef are labels as well as hex.
static bool is_numeric_address(const char *s) {
    return s[0] == '$' || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) || strchr(s, ':');
}

// `sym_bank` receives the bank of a symbol that belongs to one, else -1.
static bool arg_address_bank(cJSON *args, bool vram, char *out, size_t n, int *sym_bank,
                             char *err, size_t errn) {
    *sym_bank = -1;
    cJSON *a = args ? cJSON_GetObjectItemCaseSensitive(args, "address") : NULL;
    const char *prefix = vram ? "vram:" : "";
    if (a && cJSON_IsNumber(a) && a->valuedouble >= 0 && a->valuedouble <= 0xFFFFFF &&
        a->valuedouble == floor(a->valuedouble)) {
        snprintf(out, n, "%s0x%X", prefix, (unsigned)a->valuedouble);
        return true;
    }
    if (a && cJSON_IsString(a) && a->valuestring[0] && strlen(a->valuestring) < 32) {
        if (!SDL_strncasecmp(a->valuestring, "vram:", 5)) {
            snprintf(err, errn, "give the address alone, and select VRAM with \"space\": \"vram\"");
            return false;
        }
        if (!vram && !is_numeric_address(a->valuestring)) {
            dbg_resolved_t r;
            if (!dbg_info_resolve(a->valuestring, &r, err, errn)) return false;
            *sym_bank = r.bank;
            snprintf(out, n, "0x%X", (unsigned)r.addr);
            return true;
        }
        snprintf(out, n, "%s%s", prefix, a->valuestring);
        return true;
    }
    snprintf(err, errn, "'address' is required: a number, a string like \"$C000\" or \"0xC000\", "
                        "or a symbol");
    return false;
}

// For a request with no way to name a bank: a symbol in one that is not mapped
// would quietly act on another bank's bytes.
static bool arg_address(cJSON *args, bool vram, char *out, size_t n, char *err, size_t errn) {
    int bank;
    if (!arg_address_bank(args, vram, out, n, &bank, err, errn)) return false;
    if (bank >= 0) {
        const unsigned long at = strtoul(out, NULL, 16);
        if (at <= 0xFFFF && debug_current_x16_bank((int)at, 0) != bank) {
            snprintf(err, errn, "that symbol is in bank %d, which is not mapped", bank);
            return false;
        }
    }
    return true;
}

static bool arg_space(cJSON *args, bool *vram, char *err, size_t errn) {
    cJSON *sp = args ? cJSON_GetObjectItemCaseSensitive(args, "space") : NULL;
    if (sp && (!cJSON_IsString(sp) ||
               (strcmp(sp->valuestring, "cpu") && strcmp(sp->valuestring, "vram")))) {
        snprintf(err, errn, "'space' must be \"cpu\" or \"vram\"");
        return false;
    }
    *vram = sp && !strcmp(sp->valuestring, "vram");
    return true;
}

// Whether every key in an object is one of `keys`, which ends with NULL. The
// argument check covers only the top level, and a misspelled "button" inside a
// pad would otherwise quietly mean nothing held.
static bool only_keys(cJSON *obj, const char *const *keys, const char *what,
                      char *err, size_t errn) {
    if (!cJSON_IsObject(obj)) {
        snprintf(err, errn, "each %s must be an object", what);
        return false;
    }
    cJSON *a;
    cJSON_ArrayForEach(a, obj) {
        bool known = false;
        for (const char *const *k = keys; *k; k++) {
            if (!strcmp(a->string, *k)) known = true;
        }
        if (!known) {
            snprintf(err, errn, "a %s takes no '%.40s'", what, a->string);
            return false;
        }
    }
    return true;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Hex, already checked, to base64. Returns NULL if out of memory.
static char *hex_to_b64(const char *hex) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t n = strlen(hex) / 2;
    char *out = (char *)malloc((n + 2) / 3 * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const size_t left = n - i;
        unsigned t = 0;
        for (size_t k = 0; k < 3; k++) {
            t <<= 8;
            if (k < left)
                t |= (unsigned)(hex_value(hex[(i + k) * 2]) << 4 | hex_value(hex[(i + k) * 2 + 1]));
        }
        out[o++] = alphabet[(t >> 18) & 63];
        out[o++] = alphabet[(t >> 12) & 63];
        out[o++] = left > 1 ? alphabet[(t >> 6) & 63] : '=';
        out[o++] = left > 2 ? alphabet[t & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Base64 to uppercase hex. Returns NULL if out of memory.
static char *b64_to_hex(const char *s, int *bytes) {
    static const char digits[] = "0123456789ABCDEF";
    const size_t n = strlen(s);
    char *hex = (char *)malloc(n / 4 * 6 + 8);
    if (!hex) return NULL;
    size_t o = 0;
    int count = 0, bits = 0;
    unsigned acc = 0;
    for (size_t i = 0; i < n; i++) {
        const int v = b64_value(s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            const unsigned b = (acc >> bits) & 0xFF;
            hex[o++] = digits[b >> 4];
            hex[o++] = digits[b & 15];
            count++;
        }
    }
    hex[o] = '\0';
    *bytes = count;
    return hex;
}

// ─── Tools ──────────────────────────────────────────────────────────

static cJSON *tool_get_status(cJSON *args) {
    (void)args;
    return tool_passthrough("x16/status");
}

static cJSON *tool_get_registers(cJSON *args) {
    (void)args;
    return tool_passthrough("x16/registers");
}

static cJSON *tool_list_breakpoints(cJSON *args) {
    (void)args;
    return tool_passthrough("x16/listBreakpoints");
}

static cJSON *tool_read_memory(cJSON *args) {
    char err[200], ref[48];
    bool vram;
    int length, bank, sym_bank;
    if (!arg_space(args, &vram, err, sizeof err) ||
        !arg_address_bank(args, vram, ref, sizeof ref, &sym_bank, err, sizeof err) ||
        !arg_int(args, "length", 16, 1, MCP_READ_LIMIT, &length, err, sizeof err) ||
        !arg_int(args, "bank", -1, 0, 255, &bank, err, sizeof err))
        return tool_fail(err);
    if (vram && bank >= 0)
        return tool_fail("VRAM has no banks: leave out 'bank' when 'space' is \"vram\"");
    if (bank < 0) bank = sym_bank;

    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "memoryReference", ref);
    cJSON_AddNumberToObject(call, "count", length);
    if (bank >= 0) cJSON_AddNumberToObject(call, "x16Bank", bank);
    dbg_reply_t r;
    debug_server_call("readMemory", call, &r);
    cJSON_Delete(call);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }

    cJSON *d  = r.body ? cJSON_GetObjectItemCaseSensitive(r.body, "data") : NULL;
    cJSON *ad = r.body ? cJSON_GetObjectItemCaseSensitive(r.body, "address") : NULL;
    cJSON *un = r.body ? cJSON_GetObjectItemCaseSensitive(r.body, "unreadableBytes") : NULL;
    int got = 0;
    char *hex = b64_to_hex(d && cJSON_IsString(d) ? d->valuestring : "", &got);
    if (!hex) {
        cJSON_Delete(r.body);
        return tool_fail("out of memory");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "address", ad && cJSON_IsString(ad) ? ad->valuestring : "");
    cJSON_AddStringToObject(data, "space", vram ? "vram" : "cpu");
    cJSON_AddNumberToObject(data, "length", got);
    cJSON_AddStringToObject(data, "hex", hex);
    cJSON_AddNumberToObject(data, "unreadableBytes", un ? un->valuedouble : 0);
    cJSON_Delete(r.body);
    cJSON *result = tool_ok(data, hex);
    free(hex);
    return result;
}

static cJSON *tool_disassemble(cJSON *args) {
    char err[200], ref[48];
    int count;
    if (!arg_address(args, false, ref, sizeof ref, err, sizeof err) ||
        !arg_int(args, "count", 16, 1, MCP_DISASM_LIMIT, &count, err, sizeof err))
        return tool_fail(err);

    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "memoryReference", ref);
    cJSON_AddNumberToObject(call, "instructionCount", count);
    dbg_reply_t r;
    debug_server_call("disassemble", call, &r);
    cJSON_Delete(call);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }

    cJSON *list = r.body ? cJSON_GetObjectItemCaseSensitive(r.body, "instructions") : NULL;
    const int rows = cJSON_GetArraySize(list);
    char *text = (char *)malloc((size_t)rows * (96 + 64) + 1);
    size_t o = 0;
    if (text) {
        text[0] = '\0';
        cJSON *row;
        cJSON_ArrayForEach(row, list) {
            cJSON *a = cJSON_GetObjectItemCaseSensitive(row, "address");
            cJSON *b = cJSON_GetObjectItemCaseSensitive(row, "instructionBytes");
            cJSON *i = cJSON_GetObjectItemCaseSensitive(row, "instruction");
            cJSON *s = cJSON_GetObjectItemCaseSensitive(row, "symbol");
            if (s && cJSON_IsString(s))
                o += (size_t)snprintf(text + o, 64, "%.60s:\n", s->valuestring);
            o += (size_t)snprintf(text + o, 97, "%-9.9s %-12.12s %.60s\n",
                                  a && cJSON_IsString(a) ? a->valuestring : "",
                                  b && cJSON_IsString(b) ? b->valuestring : "",
                                  i && cJSON_IsString(i) ? i->valuestring : "");
        }
    }
    cJSON *result = tool_ok(r.body, text);
    free(text);
    return result;
}

// ─── Calls that answer later ────────────────────────────────────────

static void waiter_clear(void) {
    cJSON_Delete(waiter.id);
    waiter.id = NULL;
    waiter.kind = WAIT_NONE;
    waiter.conn = NULL;
}

static cJSON *wait_begin(wait_kind_t kind, int timeout_ms, uint32_t since) {
    waiter.id = cJSON_Duplicate(current_id, 1);
    if (!waiter.id) return tool_fail("out of memory");
    waiter.kind = kind;
    waiter.since = since;
    waiter.conn = reply_conn;
    waiter.deadline = SDL_GetTicks() + (Uint32)timeout_ms;
    return NULL;   // the reply comes from the poll
}

// How a wait ended, with where the machine is if it has stopped there.
static cJSON *wait_result(bool completed, const char *reason) {
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "completed", completed);
    if (reason) cJSON_AddStringToObject(data, "reason", reason);
    if (DEBUGIsPaused()) {
        dbg_reply_t r;
        debug_server_call("x16/registers", NULL, &r);
        cJSON *cpu = r.body ? cJSON_GetObjectItemCaseSensitive(r.body, "cpu") : NULL;
        cJSON *pc  = cpu ? cJSON_GetObjectItemCaseSensitive(cpu, "pc") : NULL;
        cJSON *pbr = cpu ? cJSON_GetObjectItemCaseSensitive(cpu, "pbr") : NULL;
        if (pc)  cJSON_AddNumberToObject(data, "pc", pc->valuedouble);
        if (pbr) cJSON_AddNumberToObject(data, "pbr", pbr->valuedouble);
        cJSON_Delete(r.body);
    }
    return tool_ok(data, NULL);
}

// This can run in the middle of another connection's request, whose reply is
// still to come.
static void waiter_answer(cJSON *result) {
    http_conn *const saved_conn = reply_conn;
    const int  saved_status = reply_status;
    const bool saved_replied = replied;
    reply_conn = waiter.conn;
    reply_status = 200;
    if (waiter.conn) waiter.conn->waiting = false;
    rpc_result(waiter.id, result);
    reply_conn = saved_conn;
    reply_status = saved_status;
    replied = saved_replied;
    waiter_clear();
}

static void waiter_poll(void) {
    if (waiter.kind == WAIT_NONE) return;
    track_stops();
    if (waiter.kind == WAIT_STOP && (int32_t)(stop_id - waiter.since) > 0) {
        waiter_answer(wait_result(true, NULL));
    } else if (SDL_TICKS_PASSED(SDL_GetTicks(), waiter.deadline)) {
        // The machine keeps going. wait_for_stop picks it up from here.
        waiter_answer(wait_result(false, "timeout"));
    }
}

// From the debug server, when a run of frames ends for any reason. It can be in
// the middle of a request, since a continue is one of the things that ends it.
static void on_frames_done(cJSON *body) {
    if (waiter.kind != WAIT_FRAMES) {
        // That call already timed out, and a stop id tells the client the rest.
        cJSON_Delete(body);
        return;
    }
    cJSON *why = cJSON_GetObjectItemCaseSensitive(body, "reason");
    const bool cancelled = why && cJSON_IsString(why) && !strcmp(why->valuestring, "cancelled");
    cJSON_AddBoolToObject(body, "completed", !cancelled);

    cJSON *reads = cJSON_GetObjectItemCaseSensitive(body, "reads");
    const int n = cJSON_GetArraySize(reads);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(reads, i);
        int bytes = 0;
        char *hex = b64_to_hex(cJSON_IsString(item) ? item->valuestring : "", &bytes);
        cJSON_ReplaceItemInArray(reads, i, cJSON_CreateString(hex ? hex : ""));
        free(hex);
    }
    waiter_answer(tool_ok(body, NULL));
}

static bool mcp_holds_run_state(void) {
    return enabled && (!http_mode || lease_live);
}

static bool mcp_can_resume(void) {
    return !read_only;
}

static void lease_renew(void) {
    if (!http_mode) return;
    lease_live = true;
    lease_end = SDL_GetTicks() + lease_ms;
}

static void lease_poll(void) {
    if (!lease_live) return;
    if (waiter.kind != WAIT_NONE) {   // a call in progress is not an idle client
        lease_renew();
        return;
    }
    if (!SDL_TICKS_PASSED(SDL_GetTicks(), lease_end)) return;
    lease_live = false;
    // A read-only client armed nothing, and its going must not resume anything.
    if (!read_only) debug_server_release_frontend();
}

// ─── Run control ────────────────────────────────────────────────────

static cJSON *tool_pause(cJSON *args) {
    (void)args;
    return tool_passthrough("pause");
}

static cJSON *tool_continue(cJSON *args) {
    (void)args;
    return tool_passthrough("continue");
}

static cJSON *tool_reset(cJSON *args) {
    (void)args;
    return tool_passthrough("restart");
}

static const char busy_message[] =
    "another call is already waiting on the machine; let it finish or cancel it";

static cJSON *tool_step(cJSON *args) {
    char err[160];
    int timeout;
    if (!arg_int(args, "timeoutMs", MCP_WAIT_DEFAULT, 0, MCP_WAIT_LIMIT, &timeout, err, sizeof err))
        return tool_fail(err);
    cJSON *mo = args ? cJSON_GetObjectItemCaseSensitive(args, "mode") : NULL;
    cJSON *gr = args ? cJSON_GetObjectItemCaseSensitive(args, "granularity") : NULL;
    const char *mode = mo && cJSON_IsString(mo) ? mo->valuestring : (mo ? "" : "into");
    const char *gran = gr && cJSON_IsString(gr) ? gr->valuestring : (gr ? "" : "instruction");
    const char *cmd = !strcmp(mode, "into") ? "stepIn"
                    : !strcmp(mode, "over") ? "next"
                    : !strcmp(mode, "out")  ? "stepOut" : NULL;
    if (!cmd) return tool_fail("'mode' must be \"into\", \"over\" or \"out\"");
    if (strcmp(gran, "instruction") && strcmp(gran, "line"))
        return tool_fail("'granularity' must be \"instruction\" or \"line\"");
    if (waiter.kind != WAIT_NONE) return tool_fail(busy_message);
    if (!DEBUGIsPaused()) return tool_fail("the machine is running: call pause first");

    track_stops();
    const uint32_t since = stop_id;
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "granularity", gran);
    dbg_reply_t r;
    debug_server_call(cmd, call, &r);
    cJSON_Delete(call);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    cJSON_Delete(r.body);
    return wait_begin(WAIT_STOP, timeout, since);
}

// One pad as x16/joystick and x16/step take it. NULL, with err set, if refused.
static cJSON *pad_args(cJSON *pad, char *err, size_t errn) {
    static const char *const keys[] = { "port", "buttons", "release", NULL };
    if (!only_keys(pad, keys, "joystick", err, errn)) return NULL;
    int port;
    if (!arg_int(pad, "port", 0, 1, 4, &port, err, errn)) return NULL;
    if (!port) {
        snprintf(err, errn, "'port' is required: 1 to 4, as BASIC's JOY(n) numbers them");
        return NULL;
    }
    cJSON *b  = cJSON_GetObjectItemCaseSensitive(pad, "buttons");
    cJSON *re = cJSON_GetObjectItemCaseSensitive(pad, "release");
    if (re && !cJSON_IsBool(re)) {
        snprintf(err, errn, "'release' must be true or false");
        return NULL;
    }
    const bool release = re && cJSON_IsTrue(re);
    if (b && release) {
        snprintf(err, errn, "give 'buttons' or 'release', not both");
        return NULL;
    }
    if (b) {
        bool names = cJSON_IsArray(b);
        cJSON *item;
        cJSON_ArrayForEach(item, b) {
            if (!cJSON_IsString(item)) names = false;
        }
        if (!names) {
            snprintf(err, errn, "'buttons' must be an array of button names");
            return NULL;
        }
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "index", port - 1);
    if (release) cJSON_AddBoolToObject(out, "enabled", false);
    else if (b)  cJSON_AddItemToObject(out, "buttons", cJSON_Duplicate(b, 1));
    return out;
}

// One region to read back as x16/step takes it.
static cJSON *read_args(cJSON *rd, char *err, size_t errn) {
    static const char *const keys[] = { "address", "length", "space", "bank", NULL };
    if (!only_keys(rd, keys, "read", err, errn)) return NULL;
    char ref[48];
    bool vram;
    int length, bank, sym_bank;
    if (!arg_space(rd, &vram, err, errn) ||
        !arg_address_bank(rd, vram, ref, sizeof ref, &sym_bank, err, errn) ||
        !arg_int(rd, "length", 16, 1, MCP_READ_LIMIT, &length, err, errn) ||
        !arg_int(rd, "bank", -1, 0, 255, &bank, err, errn))
        return NULL;
    if (vram && bank >= 0) {
        snprintf(err, errn, "VRAM has no banks: leave out 'bank' when 'space' is \"vram\"");
        return NULL;
    }
    if (bank < 0) bank = sym_bank;
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "addr", ref);
    cJSON_AddNumberToObject(out, "len", length);
    if (bank >= 0) cJSON_AddNumberToObject(out, "bank", bank);
    return out;
}

static cJSON *tool_run_frames(cJSON *args) {
    char err[160];
    int frames, timeout;
    if (!arg_int(args, "frames", 1, 1, MCP_FRAMES_LIMIT, &frames, err, sizeof err) ||
        !arg_int(args, "timeoutMs", MCP_WAIT_DEFAULT, 0, MCP_WAIT_LIMIT, &timeout, err, sizeof err))
        return tool_fail(err);
    if (waiter.kind != WAIT_NONE) return tool_fail(busy_message);

    cJSON *call = cJSON_CreateObject();
    cJSON_AddNumberToObject(call, "frames", frames);

    cJSON *joys = args ? cJSON_GetObjectItemCaseSensitive(args, "joysticks") : NULL;
    if (joys) {
        if (!cJSON_IsArray(joys)) {
            cJSON_Delete(call);
            return tool_fail("'joysticks' must be an array");
        }
        cJSON *list = cJSON_AddArrayToObject(call, "joysticks");
        cJSON *pad;
        cJSON_ArrayForEach(pad, joys) {
            cJSON *one = pad_args(pad, err, sizeof err);
            if (!one) {
                cJSON_Delete(call);
                return tool_fail(err);
            }
            cJSON_AddItemToArray(list, one);
        }
    }
    cJSON *reads = args ? cJSON_GetObjectItemCaseSensitive(args, "read") : NULL;
    if (reads) {
        if (!cJSON_IsArray(reads)) {
            cJSON_Delete(call);
            return tool_fail("'read' must be an array");
        }
        cJSON *list = cJSON_AddArrayToObject(call, "read");
        cJSON *rd;
        cJSON_ArrayForEach(rd, reads) {
            cJSON *one = read_args(rd, err, sizeof err);
            if (!one) {
                cJSON_Delete(call);
                return tool_fail(err);
            }
            cJSON_AddItemToArray(list, one);
        }
    }
    cJSON *hash = args ? cJSON_GetObjectItemCaseSensitive(args, "hash") : NULL;
    if (hash && !cJSON_IsBool(hash)) {
        cJSON_Delete(call);
        return tool_fail("'hash' must be true or false");
    }
    if (hash && cJSON_IsTrue(hash)) cJSON_AddBoolToObject(call, "hash", true);

    dbg_reply_t r;
    debug_server_call("x16/step", call, &r);
    cJSON_Delete(call);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    cJSON_Delete(r.body);
    return wait_begin(WAIT_FRAMES, timeout, 0);
}

static cJSON *tool_wait_for_stop(cJSON *args) {
    char err[160];
    int since_arg, timeout;
    if (!arg_int(args, "sinceStopId", -1, 0, 0x7FFFFFFF, &since_arg, err, sizeof err) ||
        !arg_int(args, "timeoutMs", MCP_WAIT_DEFAULT, 0, MCP_WAIT_LIMIT, &timeout, err, sizeof err))
        return tool_fail(err);
    if (waiter.kind != WAIT_NONE) return tool_fail(busy_message);
    track_stops();
    if (since_arg < 0) {
        // No marker: a machine that is already stopped is what was asked for.
        if (DEBUGIsPaused()) return wait_result(true, NULL);
        return wait_begin(WAIT_STOP, timeout, stop_id);
    }
    if ((int32_t)(stop_id - (uint32_t)since_arg) > 0) return wait_result(true, NULL);
    return wait_begin(WAIT_STOP, timeout, (uint32_t)since_arg);
}

// ─── Registers and memory ───────────────────────────────────────────

// As get_registers names them. The status register goes first because it sets
// how wide A, X and Y are on a 65C816.
static const struct { const char *arg, *name; } reg_names[] = {
    { "status", "P (Status)" }, { "pc", "PC" }, { "a", "A" }, { "x", "X" }, { "y", "Y" },
    { "sp", "SP" }, { "dp", "D" }, { "dbr", "DBR" }, { "pbr", "PBR" },
    { "ramBank", "RAM Bank" }, { "romBank", "ROM Bank" },
};

#define NUM_REG_NAMES ((int)(sizeof reg_names / sizeof reg_names[0]))

static bool reg_value_text(cJSON *v, char *out, size_t n) {
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble < 0 || v->valuedouble > 0xFFFF || v->valuedouble != floor(v->valuedouble))
            return false;
        snprintf(out, n, "%u", (unsigned)v->valuedouble);
        return true;
    }
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= n) return false;
    const char *s = v->valuestring;
    bool hex = false;
    if (s[0] == '$') { s++; hex = true; }
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; hex = true; }
    if (!*s) return false;
    for (const char *p = s; *p; p++) {
        if (hex ? hex_value(*p) < 0 : (*p < '0' || *p > '9')) return false;
    }
    snprintf(out, n, "%s", v->valuestring);
    return true;
}

static cJSON *tool_set_registers(cJSON *args) {
    if (!args || !cJSON_GetArraySize(args))
        return tool_fail("name at least one register, e.g. {\"pc\": \"$0801\"}");
    // Every value is checked first, so a typo in the last one changes nothing.
    char values[NUM_REG_NAMES][16];
    for (int i = 0; i < NUM_REG_NAMES; i++) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(args, reg_names[i].arg);
        if (v && !reg_value_text(v, values[i], sizeof values[i])) {
            char msg[120];
            snprintf(msg, sizeof msg, "'%s' must be a number, or a string like \"$FF\" or \"0xFF\"",
                     reg_names[i].arg);
            return tool_fail(msg);
        }
    }
    cJSON *set = cJSON_CreateObject();
    for (int i = 0; i < NUM_REG_NAMES; i++) {
        if (!cJSON_HasObjectItem(args, reg_names[i].arg)) continue;
        cJSON *call = cJSON_CreateObject();
        cJSON_AddNumberToObject(call, "variablesReference", 1);
        cJSON_AddStringToObject(call, "name", reg_names[i].name);
        cJSON_AddStringToObject(call, "value", values[i]);
        dbg_reply_t r;
        debug_server_call("setVariable", call, &r);
        cJSON_Delete(call);
        if (!r.success) {
            char msg[420], done[120] = "";
            size_t used = 0;
            cJSON *d;
            cJSON_ArrayForEach(d, set) {
                used += (size_t)snprintf(done + used, sizeof done - used, " %s", d->string);
                if (used >= sizeof done) break;
            }
            snprintf(msg, sizeof msg, "could not set %s: %.200s. Already set:%s",
                     reg_names[i].arg, r.message, done[0] ? done : " none");
            cJSON_Delete(r.body);
            cJSON_Delete(set);
            return tool_fail(msg);
        }
        cJSON *stored = r.body ? cJSON_GetObjectItemCaseSensitive(r.body, "value") : NULL;
        cJSON_AddStringToObject(set, reg_names[i].arg,
                                stored && cJSON_IsString(stored) ? stored->valuestring : values[i]);
        cJSON_Delete(r.body);
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "registers", set);
    return tool_ok(data, NULL);
}

static cJSON *tool_write_memory(cJSON *args) {
    char err[200], ref[48];
    bool vram;
    if (!arg_space(args, &vram, err, sizeof err) ||
        !arg_address(args, vram, ref, sizeof ref, err, sizeof err))
        return tool_fail(err);
    cJSON *hx = args ? cJSON_GetObjectItemCaseSensitive(args, "hex") : NULL;
    if (!hx || !cJSON_IsString(hx))
        return tool_fail("'hex' is required: the bytes to write, e.g. \"A9058D0004\"");
    const size_t digits = strlen(hx->valuestring);
    if (!digits || digits % 2 || digits > MCP_READ_LIMIT * 2)
        return tool_fail("'hex' must be an even number of digits, 1 to 4096 bytes");
    for (size_t i = 0; i < digits; i++) {
        if (hex_value(hx->valuestring[i]) < 0)
            return tool_fail("'hex' may contain only hex digits");
    }

    // The write itself wraps at the top of the space, so a range that runs off
    // the end would land on the start of memory. A read reports it instead.
    cJSON *probe = cJSON_CreateObject();
    cJSON_AddStringToObject(probe, "memoryReference", ref);
    cJSON_AddNumberToObject(probe, "count", (double)(digits / 2));
    dbg_reply_t pr;
    debug_server_call("readMemory", probe, &pr);
    cJSON_Delete(probe);
    if (!pr.success) {
        cJSON_Delete(pr.body);
        return tool_fail(pr.message);
    }
    cJSON *un = pr.body ? cJSON_GetObjectItemCaseSensitive(pr.body, "unreadableBytes") : NULL;
    const bool past_end = un && un->valuedouble > 0;
    cJSON_Delete(pr.body);
    if (past_end) return tool_fail("the write runs past the end of the address space");

    char *b64 = hex_to_b64(hx->valuestring);
    if (!b64) return tool_fail("out of memory");
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "memoryReference", ref);
    cJSON_AddStringToObject(call, "data", b64);
    free(b64);
    dbg_reply_t r;
    debug_server_call("writeMemory", call, &r);
    cJSON_Delete(call);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    cJSON *wrote = r.body ? cJSON_GetObjectItemCaseSensitive(r.body, "bytesWritten") : NULL;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "space", vram ? "vram" : "cpu");
    cJSON_AddNumberToObject(data, "bytesWritten", wrote ? wrote->valuedouble : 0);
    cJSON_Delete(r.body);
    return tool_ok(data, NULL);
}

// ─── Breakpoints ────────────────────────────────────────────────────

// The one place a source line generated code, as a breakpoint address. A line
// with code in several places is refused rather than guessed: two segments at
// $A000 are different banks even before a LOAD has shown which.
static bool source_line_ref(cJSON *args, bool vram, char *ref, size_t ref_n, int *sym_bank,
                            char *file, size_t file_n, int *line, char *err, size_t errn) {
    cJSON *f = cJSON_GetObjectItemCaseSensitive(args, "file");
    cJSON *k = cJSON_GetObjectItemCaseSensitive(args, "kind");
    if (cJSON_HasObjectItem(args, "address")) {
        snprintf(err, errn, "give 'address', or 'file' and 'line', not both");
        return false;
    }
    if (vram || (k && (!cJSON_IsString(k) || strcmp(k->valuestring, "exec")))) {
        snprintf(err, errn, "'file' and 'line' name code, so they set an exec breakpoint in CPU memory");
        return false;
    }
    if (!f || !cJSON_IsString(f) || !f->valuestring[0] || !cJSON_HasObjectItem(args, "line")) {
        snprintf(err, errn, "'file' and 'line' go together");
        return false;
    }
    int bank;
    if (!arg_int(args, "line", 0, 1, 0x7FFFFFF0, line, err, errn) ||
        !arg_int(args, "bank", -1, 0, 255, &bank, err, errn))
        return false;

    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "file", f->valuestring);
    cJSON_AddNumberToObject(call, "line", *line);
    if (bank >= 0) cJSON_AddNumberToObject(call, "bank", bank);
    dbg_reply_t r;
    debug_server_call("x16/sourceLocation", call, &r);
    cJSON_Delete(call);
    if (!r.success) {
        cJSON_Delete(r.body);
        snprintf(err, errn, "%s", r.message);
        return false;
    }
    cJSON *hits = cJSON_GetObjectItemCaseSensitive(r.body, "locations");
    const int count = cJSON_GetArraySize(hits);
    if (count != 1) {
        if (!count) {
            snprintf(err, errn, "no code at %s:%d", f->valuestring, *line);
        } else {
            size_t o = (size_t)snprintf(err, errn, "%s:%d has code in %d places:",
                                        f->valuestring, *line, count);
            for (int i = 0; i < count && o < errn; i++) {
                cJSON *h = cJSON_GetArrayItem(hits, i);
                cJSON *a = cJSON_GetObjectItemCaseSensitive(h, "address");
                cJSON *b = cJSON_GetObjectItemCaseSensitive(h, "bank");
                cJSON *s = cJSON_GetObjectItemCaseSensitive(h, "segment");
                o += (size_t)snprintf(err + o, errn - o, "%s $%04X", i ? "," : "",
                                      a ? (unsigned)a->valuedouble : 0u);
                if (o < errn && cJSON_IsString(s))
                    o += (size_t)snprintf(err + o, errn - o, " in %s", s->valuestring);
                if (o < errn && cJSON_IsNumber(b))
                    o += (size_t)snprintf(err + o, errn - o, " bank %d", (int)b->valuedouble);
            }
            if (o < errn) snprintf(err + o, errn - o, ". Give 'bank', or the address.");
        }
        cJSON_Delete(r.body);
        return false;
    }
    cJSON *h = cJSON_GetArrayItem(hits, 0);
    cJSON *a = cJSON_GetObjectItemCaseSensitive(h, "address");
    cJSON *b = cJSON_GetObjectItemCaseSensitive(h, "bank");
    cJSON *n = cJSON_GetObjectItemCaseSensitive(h, "file");
    snprintf(ref, ref_n, "0x%X", a ? (unsigned)a->valuedouble : 0u);
    *sym_bank = cJSON_IsNumber(b) ? (int)b->valuedouble : -1;
    snprintf(file, file_n, "%s", cJSON_IsString(n) ? n->valuestring : f->valuestring);
    cJSON_Delete(r.body);
    return true;
}

// A dump breakpoint's file and regions as mcp/setBreakpoint takes them. NULL,
// with err set, if refused.
static cJSON *dump_args(cJSON *dump, char *err, size_t errn) {
    static const char *const keys[] = { "file", "regions", NULL };
    if (!only_keys(dump, keys, "dump", err, errn)) return NULL;
    cJSON *file = cJSON_GetObjectItemCaseSensitive(dump, "file");
    cJSON *regs = cJSON_GetObjectItemCaseSensitive(dump, "regions");
    if (!file || !cJSON_IsString(file) || !file->valuestring[0] || !cJSON_IsArray(regs) ||
        cJSON_GetArraySize(regs) < 1 || cJSON_GetArraySize(regs) > 16) {
        snprintf(err, errn, "'dump' needs a 'file' and 1 to 16 'regions'");
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "file", file->valuestring);
    cJSON *list = cJSON_AddArrayToObject(out, "regions");
    cJSON *rd;
    cJSON_ArrayForEach(rd, regs) {
        cJSON *one = read_args(rd, err, errn);
        if (!one) {
            cJSON_Delete(out);
            return NULL;
        }
        cJSON_AddItemToArray(list, one);
    }
    return out;
}

static cJSON *breakpoint_call(const char *command, cJSON *args) {
    char err[512], ref[48], where[512] = "";
    bool vram = false;
    int sym_bank, where_line = 0;
    const bool by_line = cJSON_HasObjectItem(args, "file") || cJSON_HasObjectItem(args, "line");
    if (!by_line && !cJSON_HasObjectItem(args, "address"))
        return tool_fail("give 'address', or 'file' and 'line'");
    if (!arg_space(args, &vram, err, sizeof err))
        return tool_fail(err);
    if (by_line ? !source_line_ref(args, vram, ref, sizeof ref, &sym_bank, where, sizeof where,
                                   &where_line, err, sizeof err)
                : !arg_address_bank(args, vram, ref, sizeof ref, &sym_bank, err, sizeof err))
        return tool_fail(err);

    cJSON *ki = args ? cJSON_GetObjectItemCaseSensitive(args, "kind") : NULL;
    if (ki && (!cJSON_IsString(ki) ||
               (strcmp(ki->valuestring, "exec") && strcmp(ki->valuestring, "write") &&
                strcmp(ki->valuestring, "read") && strcmp(ki->valuestring, "access"))))
        return tool_fail("'kind' must be \"exec\", \"write\", \"read\" or \"access\"");
    cJSON *xf = args ? cJSON_GetObjectItemCaseSensitive(args, "excludeFetch") : NULL;
    if (xf && !cJSON_IsBool(xf))
        return tool_fail("'excludeFetch' must be true or false");
    if (xf && (!ki || (strcmp(ki->valuestring, "read") && strcmp(ki->valuestring, "access"))))
        return tool_fail("'excludeFetch' applies to kind \"read\" or \"access\"");
    // Code never runs from VRAM, and only stores are hooked there.
    if (vram && (!ki || strcmp(ki->valuestring, "write")))
        return tool_fail("a VRAM watch needs \"kind\": \"write\"");
    int bank, length;
    if (!arg_int(args, "bank", -1, 0, 255, &bank, err, sizeof err) ||
        !arg_int(args, "length", 1, 1, 0x20000, &length, err, sizeof err))
        return tool_fail(err);
    // An exec breakpoint on a banked symbol belongs to that bank. A watch covers
    // every bank, so it would also stop on whatever shares the address.
    if (sym_bank >= 0 && bank < 0) {
        if (ki && strcmp(ki->valuestring, "exec")) {
            snprintf(err, sizeof err, "that symbol is in bank %d, and a watch covers every bank: "
                                      "give its address to watch it in all of them", sym_bank);
            return tool_fail(err);
        }
        bank = sym_bank;
    }
    cJSON *co = args ? cJSON_GetObjectItemCaseSensitive(args, "condition") : NULL;
    cJSON *hi = args ? cJSON_GetObjectItemCaseSensitive(args, "hitCondition") : NULL;
    if ((co && !cJSON_IsString(co)) || (hi && !cJSON_IsString(hi)))
        return tool_fail("'condition' and 'hitCondition' must be strings");
    cJSON *dp = args ? cJSON_GetObjectItemCaseSensitive(args, "dump") : NULL;
    if (dp && ki && strcmp(ki->valuestring, "exec"))
        return tool_fail("'dump' applies to an exec breakpoint");
    cJSON *lg = args ? cJSON_GetObjectItemCaseSensitive(args, "log") : NULL;
    if (lg && !cJSON_IsString(lg))
        return tool_fail("'log' must be a string");
    if (lg && ki && strcmp(ki->valuestring, "exec"))
        return tool_fail("'log' applies to an exec breakpoint");

    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "address", ref);
    if (ki) cJSON_AddStringToObject(call, "kind", ki->valuestring);
    if (bank >= 0) cJSON_AddNumberToObject(call, "bank", bank);
    if (cJSON_HasObjectItem(args, "length")) cJSON_AddNumberToObject(call, "length", length);
    if (xf) cJSON_AddBoolToObject(call, "excludeFetch", cJSON_IsTrue(xf));
    if (co) cJSON_AddStringToObject(call, "condition", co->valuestring);
    if (hi) cJSON_AddStringToObject(call, "hitCondition", hi->valuestring);
    cJSON *en = args ? cJSON_GetObjectItemCaseSensitive(args, "enabled") : NULL;
    if (en) cJSON_AddBoolToObject(call, "enabled", cJSON_IsTrue(en));
    if (dp) {
        cJSON *converted = dump_args(dp, err, sizeof err);
        if (!converted) {
            cJSON_Delete(call);
            return tool_fail(err);
        }
        cJSON_AddItemToObject(call, "dump", converted);
    }
    if (lg) cJSON_AddStringToObject(call, "log", lg->valuestring);
    dbg_reply_t r;
    debug_server_call(command, call, &r);
    cJSON_Delete(call);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    cJSON *body = r.body ? r.body : cJSON_CreateObject();
    if (by_line) {
        cJSON_AddStringToObject(body, "file", where);
        cJSON_AddNumberToObject(body, "line", where_line);
    }
    return tool_ok(body, NULL);
}

static cJSON *tool_set_breakpoint(cJSON *args) {
    return breakpoint_call("mcp/setBreakpoint", args);
}

static cJSON *tool_remove_breakpoint(cJSON *args) {
    return breakpoint_call("mcp/removeBreakpoint", args);
}

static cJSON *tool_enable_breakpoint(cJSON *args) {
    cJSON *en = args ? cJSON_GetObjectItemCaseSensitive(args, "enabled") : NULL;
    if (!en || !cJSON_IsBool(en)) return tool_fail("'enabled' is required: true or false");
    return breakpoint_call("mcp/enableBreakpoint", args);
}

// ─── Input, save states, statistics ─────────────────────────────────

// A command whose arguments the schema has already narrowed.
static cJSON *tool_forward(const char *command, cJSON *args) {
    dbg_reply_t r;
    debug_server_call(command, args, &r);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    return tool_ok(r.body ? r.body : cJSON_CreateObject(), NULL);
}

static cJSON *tool_load_symbols(cJSON *args) {
    return tool_forward("x16/loadSymbols", args);
}

static cJSON *tool_lookup_symbol(cJSON *args) {
    return tool_forward("x16/lookupSymbol", args);
}

// Reaches only the expression engine, never the console commands behind the
// DAP evaluate request, one of which clears every client's breakpoints.
static cJSON *tool_evaluate(cJSON *args) {
    cJSON *e = args ? cJSON_GetObjectItemCaseSensitive(args, "expression") : NULL;
    if (!e || !cJSON_IsString(e))
        return tool_fail("'expression' is required, e.g. \"byte[$0400] + 1\"");
    return tool_forward("mcp/evaluate", args);
}

static cJSON *tool_read_log(cJSON *args) {
    return tool_forward("x16/readLog", args);
}

static cJSON *tool_list_sources(cJSON *args) {
    (void)args;
    return tool_passthrough("loadedSources");
}

static cJSON *tool_read_source(cJSON *args) {
    char err[200];
    int start, count;
    cJSON *f = args ? cJSON_GetObjectItemCaseSensitive(args, "file") : NULL;
    if (!f || !cJSON_IsString(f) || !f->valuestring[0])
        return tool_fail("'file' is required: a name from list_sources");
    if (!arg_int(args, "startLine", 1, 1, 0x7FFFFFF0, &start, err, sizeof err) ||
        !arg_int(args, "lineCount", 200, 1, 2000, &count, err, sizeof err))
        return tool_fail(err);
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "file", f->valuestring);
    cJSON_AddNumberToObject(call, "startLine", start);
    cJSON_AddNumberToObject(call, "lineCount", count);
    cJSON *result = tool_forward("mcp/readSource", call);
    cJSON_Delete(call);
    return result;
}

static cJSON *tool_source_location(cJSON *args) {
    char err[200], ref[48];
    int bank, line = 0, sym_bank = -1;
    cJSON *f = args ? cJSON_GetObjectItemCaseSensitive(args, "file") : NULL;
    const bool has_address = cJSON_HasObjectItem(args, "address");
    const bool has_line    = cJSON_HasObjectItem(args, "line");
    if (has_address == (f != NULL || has_line))
        return tool_fail("give 'address', or 'file' and 'line'");
    if (!arg_int(args, "bank", -1, 0, 255, &bank, err, sizeof err))
        return tool_fail(err);
    if (has_address) {
        if (!arg_address_bank(args, false, ref, sizeof ref, &sym_bank, err, sizeof err))
            return tool_fail(err);
    } else {
        if (!f || !cJSON_IsString(f) || !f->valuestring[0] || !has_line)
            return tool_fail("'file' and 'line' go together");
        if (!arg_int(args, "line", 0, 1, 0x7FFFFFF0, &line, err, sizeof err))
            return tool_fail(err);
    }
    cJSON *call = cJSON_CreateObject();
    if (has_address) {
        cJSON_AddStringToObject(call, "address", ref);
    } else {
        cJSON_AddStringToObject(call, "file", f->valuestring);
        cJSON_AddNumberToObject(call, "line", line);
    }
    if (bank < 0) bank = sym_bank;
    if (bank >= 0) cJSON_AddNumberToObject(call, "bank", bank);
    cJSON *result = tool_forward("x16/sourceLocation", call);
    cJSON_Delete(call);
    return result;
}

static cJSON *tool_list_symbols(cJSON *args) {
    return tool_forward("x16/listSymbols", args);
}

static cJSON *tool_get_stack(cJSON *args) {
    return tool_forward("x16/callStack", args);
}

// hash_memory and dump_memory take the same range as read_memory.
static cJSON *tool_range(const char *command, cJSON *args, bool to_file) {
    char err[200], ref[48];
    bool vram;
    int length, bank, sym_bank;
    if (!arg_space(args, &vram, err, sizeof err) ||
        !arg_address_bank(args, vram, ref, sizeof ref, &sym_bank, err, sizeof err) ||
        !arg_int(args, "length", 0, 1, 0x100000, &length, err, sizeof err) ||
        !arg_int(args, "bank", -1, 0, 255, &bank, err, sizeof err))
        return tool_fail(err);
    if (!length) return tool_fail("'length' is required");
    if (vram && bank >= 0)
        return tool_fail("VRAM has no banks: leave out 'bank' when 'space' is \"vram\"");
    if (bank < 0) bank = sym_bank;

    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "memoryReference", ref);
    cJSON_AddNumberToObject(call, "count", length);
    if (bank >= 0) cJSON_AddNumberToObject(call, "x16Bank", bank);
    if (to_file) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
        cJSON *o = cJSON_GetObjectItemCaseSensitive(args, "overwrite");
        if (p) cJSON_AddItemToObject(call, "path", cJSON_Duplicate(p, 1));
        if (o) cJSON_AddItemToObject(call, "overwrite", cJSON_Duplicate(o, 1));
    }
    cJSON *result = tool_forward(command, call);
    cJSON_Delete(call);
    return result;
}

static cJSON *tool_hash_memory(cJSON *args) {
    return tool_range("x16/hashMemory", args, false);
}

static cJSON *tool_dump_memory(cJSON *args) {
    return tool_range("x16/dumpMemory", args, true);
}

static cJSON *tool_set_warp(cJSON *args) {
    return tool_forward("x16/setWarp", args);
}

static cJSON *tool_mouse(cJSON *args) {
    return tool_forward("x16/mouse", args);
}

static cJSON *tool_load_prg(cJSON *args) {
    cJSON *p = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    cJSON *call = cJSON_CreateObject();
    if (p) cJSON_AddItemToObject(call, "path", cJSON_Duplicate(p, 1));
    if (args && cJSON_HasObjectItem(args, "address")) {
        char err[200], ref[48];
        if (!arg_address(args, false, ref, sizeof ref, err, sizeof err)) {
            cJSON_Delete(call);
            return tool_fail(err);
        }
        cJSON_AddStringToObject(call, "address", ref);
    }
    cJSON *result = tool_forward("x16/loadPrg", call);
    cJSON_Delete(call);
    return result;
}

static cJSON *tool_type_text(cJSON *args) {
    cJSON *t = args ? cJSON_GetObjectItemCaseSensitive(args, "text") : NULL;
    if (!t || !cJSON_IsString(t)) return tool_fail("'text' is required");
    return tool_forward("x16/type", args);
}

static cJSON *tool_send_key(cJSON *args) {
    cJSON *k = args ? cJSON_GetObjectItemCaseSensitive(args, "key") : NULL;
    if (!k || !cJSON_IsString(k)) return tool_fail("'key' is required: an SDL key name such as \"Return\"");
    cJSON *ac = cJSON_GetObjectItemCaseSensitive(args, "action");
    if (ac && (!cJSON_IsString(ac) || (strcmp(ac->valuestring, "press") &&
                                       strcmp(ac->valuestring, "down") &&
                                       strcmp(ac->valuestring, "up"))))
        return tool_fail("'action' must be \"press\", \"down\" or \"up\"");
    // The handler skips a modifier it cannot name, which would type the wrong key.
    cJSON *mods = cJSON_GetObjectItemCaseSensitive(args, "modifiers");
    if (mods) {
        if (!cJSON_IsArray(mods) || cJSON_GetArraySize(mods) > 8)
            return tool_fail("'modifiers' must be an array of at most 8 key names");
        cJSON *m;
        cJSON_ArrayForEach(m, mods) {
            if (!cJSON_IsString(m) ||
                SDL_GetScancodeFromName(m->valuestring) == SDL_SCANCODE_UNKNOWN)
                return tool_fail("each modifier must be an SDL key name, such as \"Left Shift\"");
        }
    }
    return tool_forward("x16/sendKey", args);
}

static cJSON *tool_joystick(cJSON *args) {
    char err[160];
    if (!args) return tool_fail("'port' is required: 1 to 4, as BASIC's JOY(n) numbers them");
    cJSON *call = pad_args(args, err, sizeof err);
    if (!call) return tool_fail(err);
    cJSON *result = tool_forward("x16/joystick", call);
    cJSON_Delete(call);
    return result;
}

// Named slots only. A file path is a host filesystem write, which is more than
// a debugging client should get by default.
static cJSON *state_call(const char *command, cJSON *args, bool events) {
    cJSON *s = args ? cJSON_GetObjectItemCaseSensitive(args, "slot") : NULL;
    if (!s || !cJSON_IsString(s) || !s->valuestring[0])
        return tool_fail("'slot' is required: a name for the state");
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "slot", s->valuestring);
    if (events) cJSON_AddBoolToObject(call, "events", true);
    cJSON *result = tool_forward(command, call);
    cJSON_Delete(call);
    return result;
}

static cJSON *tool_save_state(cJSON *args) {
    return state_call("x16/saveState", args, false);
}

static cJSON *tool_load_state(cJSON *args) {
    // An editor attached alongside has to redraw a machine that just changed.
    return state_call("x16/loadState", args, true);
}

static cJSON *tool_perf_stats(cJSON *args) {
    return tool_forward("x16/perfStats", args);
}

// The requests below skip a value of the wrong type, which would turn a mistake
// into a call that quietly did less than it was asked to.
static bool arg_bool(cJSON *args, const char *name, char *err, size_t errn) {
    cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, name) : NULL;
    if (v && !cJSON_IsBool(v)) {
        snprintf(err, errn, "'%s' must be true or false", name);
        return false;
    }
    return true;
}

static bool arg_text(cJSON *args, const char *name, char *err, size_t errn) {
    cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, name) : NULL;
    if (v && (!cJSON_IsString(v) || !v->valuestring[0])) {
        snprintf(err, errn, "'%s' must be a non-empty string", name);
        return false;
    }
    return true;
}

// `start` to `end`, end exclusive, in the CPU map.
static bool arg_span(cJSON *obj, char *err, size_t errn) {
    int start, end;
    if (!arg_int(obj, "start", -1, 0, 0xFFFF, &start, err, errn) ||
        !arg_int(obj, "end", -1, 0, 0xFFFF, &end, err, errn))
        return false;
    if (start < 0 || end <= start) {
        snprintf(err, errn, "'start' and 'end' are required, with end above start");
        return false;
    }
    return true;
}

static cJSON *tool_perf_history(cJSON *args) {
    static const char *const span_keys[] = {"start", "end", NULL};
    char err[200];
    if (!arg_bool(args, "enabled", err, sizeof err) || !arg_bool(args, "reset", err, sizeof err) ||
        !arg_bool(args, "overwrite", err, sizeof err) || !arg_text(args, "dump", err, sizeof err))
        return tool_fail(err);
    cJSON *caller = args ? cJSON_GetObjectItemCaseSensitive(args, "caller") : NULL;
    if (caller) {
        cJSON *s = cJSON_IsObject(caller) ? cJSON_GetObjectItemCaseSensitive(caller, "start") : NULL;
        cJSON *e = cJSON_IsObject(caller) ? cJSON_GetObjectItemCaseSensitive(caller, "end") : NULL;
        const bool off = cJSON_IsNumber(s) && cJSON_IsNumber(e) && s->valuedouble == 0 && e->valuedouble == 0;
        if (!only_keys(caller, span_keys, "caller", err, sizeof err) ||
            (!off && !arg_span(caller, err, sizeof err)))
            return tool_fail(err);
    }
    return tool_forward("x16/perfHist", args);
}

static cJSON *tool_perf_config(cJSON *args) {
    static const char *const zone_keys[] = {"name", "start", "end", "bank", "idle", NULL};
    char err[200];
    int n;
    if (!arg_bool(args, "enabled", err, sizeof err) || !arg_bool(args, "reset", err, sizeof err) ||
        !arg_int(args, "historyFrames", 2, 2, 65536, &n, err, sizeof err))
        return tool_fail(err);
    cJSON *fps = args ? cJSON_GetObjectItemCaseSensitive(args, "targetFps") : NULL;
    if (fps && (!cJSON_IsNumber(fps) || fps->valuedouble < 1 || fps->valuedouble > 1000))
        return tool_fail("'targetFps' must be a number from 1 to 1000");
    cJSON *idle = args ? cJSON_GetObjectItemCaseSensitive(args, "idleMode") : NULL;
    if (idle && (!cJSON_IsString(idle) || (strcmp(idle->valuestring, "auto") &&
                                           strcmp(idle->valuestring, "markers") &&
                                           strcmp(idle->valuestring, "none"))))
        return tool_fail("'idleMode' must be \"auto\", \"markers\" or \"none\"");
    cJSON *zones = args ? cJSON_GetObjectItemCaseSensitive(args, "zones") : NULL;
    if (zones) {
        if (!cJSON_IsArray(zones) || cJSON_GetArraySize(zones) > PERF_MAX_ZONES) {
            snprintf(err, sizeof err, "'zones' must be an array of at most %d", PERF_MAX_ZONES);
            return tool_fail(err);
        }
        cJSON *z;
        cJSON_ArrayForEach(z, zones) {
            if (!only_keys(z, zone_keys, "zone", err, sizeof err) || !arg_span(z, err, sizeof err) ||
                !arg_int(z, "bank", -1, 0, 255, &n, err, sizeof err) ||
                !arg_text(z, "name", err, sizeof err) || !arg_bool(z, "idle", err, sizeof err))
                return tool_fail(err);
        }
    }
    cJSON *none = args ? NULL : cJSON_CreateObject();  // no arguments is a plain read
    cJSON *result = tool_forward("x16/perfConfig", args ? args : none);
    cJSON_Delete(none);
    return result;
}

static cJSON *tool_host_perf(cJSON *args) {
    (void)args;
    return tool_passthrough("x16/hostPerf");
}

static cJSON *tool_get_sdcard(cJSON *args) {
    char err[200];
    int n;
    cJSON *files = args ? cJSON_GetObjectItemCaseSensitive(args, "files") : NULL;
    if (files && (!cJSON_IsString(files) || (strcmp(files->valuestring, "touched") &&
                                             strcmp(files->valuestring, "all") &&
                                             strcmp(files->valuestring, "none"))))
        return tool_fail("'files' must be \"touched\", \"all\" or \"none\"");
    if (!arg_int(args, "history", SDCARD_HISTORY_SIZE, 0, SDCARD_HISTORY_SIZE, &n, err, sizeof err))
        return tool_fail(err);
    dbg_reply_t r;
    debug_server_call("x16/sdcard", args, &r);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    cJSON *body = r.body ? r.body : cJSON_CreateObject();
    const bool attached = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "attached"));
    return tool_ok(body, attached ? NULL : "No SD card is attached.");
}

static cJSON *tool_sprite_mux_stats(cJSON *args) {
    (void)args;
    dbg_reply_t r;
    debug_server_call("x16/spriteMuxStats", NULL, &r);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    cJSON *body = r.body ? r.body : cJSON_CreateObject();
    // `frame` in every tool result is the machine's frame number.
    cJSON *last = cJSON_DetachItemFromObjectCaseSensitive(body, "frame");
    if (last) cJSON_AddItemToObject(body, "lastFrame", last);
    return tool_ok(body, NULL);
}

static cJSON *tool_sprite_mux_config(cJSON *args) {
    char err[200];
    int n;
    if (!arg_bool(args, "enabled", err, sizeof err) || !arg_bool(args, "reset", err, sizeof err) ||
        !arg_int(args, "burstGap", 128, 8, 2048, &n, err, sizeof err))
        return tool_fail(err);
    cJSON *none = args ? NULL : cJSON_CreateObject();  // no arguments is a plain read
    cJSON *result = tool_forward("x16/spriteMuxConfig", args ? args : none);
    cJSON_Delete(none);
    return result;
}

// Answers at once with the state. A capture spans frames, so a client polls.
static cJSON *tool_profile_capture(cJSON *args) {
    static const char *const start_keys[] = {"frames", "seconds", "onMarker", "recorders", NULL};
    static const char *const export_keys[] = {"folded", "json", "trace", "run", NULL};
    char err[200];
    int n;
    if (!arg_bool(args, "stop", err, sizeof err) || !arg_bool(args, "overwrite", err, sizeof err) ||
        !arg_text(args, "load", err, sizeof err) ||
        !arg_int(args, "top", 50, 0, 100000, &n, err, sizeof err))
        return tool_fail(err);
    cJSON *ex = args ? cJSON_GetObjectItemCaseSensitive(args, "export") : NULL;
    if (ex) {
        if (!only_keys(ex, export_keys, "export", err, sizeof err)) return tool_fail(err);
        for (const char *const *k = export_keys; *k; k++) {
            if (!arg_text(ex, *k, err, sizeof err)) return tool_fail(err);
        }
    }
    cJSON *start = args ? cJSON_GetObjectItemCaseSensitive(args, "start") : NULL;
    if (start) {
        if (!only_keys(start, start_keys, "start", err, sizeof err) ||
            !arg_int(start, "frames", 1, 1, 0x7FFFFFFF, &n, err, sizeof err) ||
            !arg_bool(start, "onMarker", err, sizeof err))
            return tool_fail(err);
        cJSON *secs = cJSON_GetObjectItemCaseSensitive(start, "seconds");
        if (secs && (!cJSON_IsNumber(secs) || secs->valuedouble <= 0))
            return tool_fail("'seconds' must be a number above 0");
        cJSON *recs = cJSON_GetObjectItemCaseSensitive(start, "recorders");
        if (recs && !cJSON_IsArray(recs))
            return tool_fail("'recorders' must be an array");
        if (ex || cJSON_HasObjectItem(args, "stop") || cJSON_HasObjectItem(args, "load"))
            return tool_fail("'start' goes alone: poll until the state is done, then export");
    }
    return tool_forward("x16/capture", args);
}

static cJSON *tool_get_vera_state(cJSON *args) {
    (void)args;
    return tool_passthrough("x16/vera");
}

// The lines are the answer, so they are the text a client reads first.
static cJSON *tool_get_screen(cJSON *args) {
    dbg_reply_t r;
    debug_server_call("x16/screen", args, &r);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    size_t size = 1;
    cJSON *lines = cJSON_GetObjectItemCaseSensitive(r.body, "lines");
    cJSON *line;
    cJSON_ArrayForEach(line, lines) size += strlen(line->valuestring) + 1;
    char *text = (char *)malloc(size);
    if (!text) {
        cJSON_Delete(r.body);
        return tool_fail("out of memory");
    }
    char *p = text;
    cJSON_ArrayForEach(line, lines) {
        const size_t n = strlen(line->valuestring);
        memcpy(p, line->valuestring, n);
        p += n;
        *p++ = '\n';
    }
    *p = '\0';
    cJSON *result = tool_ok(r.body, text);
    free(text);
    return result;
}

// The image goes in the content, and only there: a copy in structuredContent
// would send a megabyte of base64 twice.
static cJSON *tool_screenshot(cJSON *args) {
    (void)args;
    dbg_reply_t r;
    debug_server_call("x16/screenshot", NULL, &r);
    if (!r.success) {
        cJSON_Delete(r.body);
        return tool_fail(r.message);
    }
    cJSON *image = cJSON_DetachItemFromObjectCaseSensitive(r.body, "data");
    cJSON *mime  = cJSON_DetachItemFromObjectCaseSensitive(r.body, "mimeType");
    cJSON *result = tool_ok(r.body, NULL);
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "image");
    cJSON_AddItemToObject(item, "data", image);
    cJSON_AddItemToObject(item, "mimeType", mime);
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(result, "content"), item);
    return result;
}

typedef enum {
    TOOL_READ,         // changes nothing in the machine, writes no file, arms nothing
    TOOL_CONTROL,      // moves or feeds it, and nothing is lost
    TOOL_DESTRUCTIVE,  // overwrites state that cannot be got back
} tool_effect_t;

typedef struct {
    const char   *name;
    const char   *description;
    const char   *schema;     // JSON Schema of the arguments
    tool_effect_t effect;
    cJSON       *(*run)(cJSON *args);
} mcp_tool_t;

#define NO_ARGS "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
#define TIMEOUT_ARG "\"timeoutMs\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":30000,\"default\":5000}"
#define ADDRESS_ARG "\"address\":{\"type\":[\"string\",\"number\"],\"description\":\"$C000, 0xC000, 04:C000, or a symbol such as main+4. A bare word is a symbol, not hex. In VRAM, bare hex.\"}"
#define REG_ARG     "{\"type\":[\"string\",\"integer\"]}"
#define PAD_ITEM \
    "{\"type\":\"object\",\"properties\":{" \
    "\"port\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4}," \
    "\"buttons\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}," \
    "\"release\":{\"type\":\"boolean\"}" \
    "},\"required\":[\"port\"],\"additionalProperties\":false}"

static const mcp_tool_t tools[] = {
    { "get_status",
      "Is the machine paused, on which frame, and at how many cycles.",
      NO_ARGS, TOOL_READ, tool_get_status },
    { "pause",
      "Stop the machine. A paused machine stays paused until a client resumes it.",
      NO_ARGS, TOOL_CONTROL, tool_pause },
    { "continue",
      "Resume the machine. Ends a run_frames in progress, which then reports \"cancelled\".",
      NO_ARGS, TOOL_CONTROL, tool_continue },
    { "step",
      "Step a paused machine and answer once it has stopped again. mode is into, over or "
      "out. If it has not stopped by timeoutMs the result has completed: false and the "
      "machine keeps going: follow up with wait_for_stop.",
      "{\"type\":\"object\",\"properties\":{"
      "\"mode\":{\"type\":\"string\",\"enum\":[\"into\",\"over\",\"out\"],\"default\":\"into\"},"
      "\"granularity\":{\"type\":\"string\",\"enum\":[\"instruction\",\"line\"],\"default\":\"instruction\"},"
      TIMEOUT_ARG
      "},\"additionalProperties\":false}",
      TOOL_CONTROL, tool_step },
    { "run_frames",
      "Run a paused machine for a number of frames, stop on a frame boundary, and answer. "
      "Joysticks apply before the first instruction. 'read' returns memory as hex once it "
      "stops. A breakpoint ends it early, and reason says why it ended.",
      "{\"type\":\"object\",\"properties\":{"
      "\"frames\":{\"type\":\"integer\",\"minimum\":1,\"default\":1},"
      "\"joysticks\":{\"type\":\"array\",\"items\":" PAD_ITEM "},"
      "\"read\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
      ADDRESS_ARG ","
      "\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":16},"
      "\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}"
      "},\"required\":[\"address\"],\"additionalProperties\":false}},"
      "\"hash\":{\"type\":\"boolean\",\"description\":\"include a hash of the whole machine state\"},"
      TIMEOUT_ARG
      "},\"additionalProperties\":false}",
      TOOL_CONTROL, tool_run_frames },
    { "wait_for_stop",
      "Wait for the machine to stop. Pass the stopId from an earlier result as sinceStopId "
      "and a stop that landed in between is not missed. With no sinceStopId, a machine "
      "already paused answers at once. At timeoutMs the result has completed: false.",
      "{\"type\":\"object\",\"properties\":{"
      "\"sinceStopId\":{\"type\":\"integer\",\"minimum\":0},"
      TIMEOUT_ARG
      "},\"additionalProperties\":false}",
      TOOL_READ, tool_wait_for_stop },
    { "reset",
      "Reset the machine, as the reset button does. Everything running is lost.",
      NO_ARGS, TOOL_DESTRUCTIVE, tool_reset },
    { "get_registers",
      "CPU registers, including the 65C816 extras and the current RAM and ROM banks.",
      NO_ARGS, TOOL_READ, tool_get_registers },
    { "set_registers",
      "Set CPU registers, by the names get_registers uses. Values are numbers or strings "
      "like \"$FF\". A value wider than the register is cut to fit, and the result shows "
      "what each register now holds.",
      "{\"type\":\"object\",\"properties\":{"
      "\"pc\":" REG_ARG ",\"a\":" REG_ARG ",\"x\":" REG_ARG ",\"y\":" REG_ARG ",\"sp\":" REG_ARG ","
      "\"dp\":" REG_ARG ",\"dbr\":" REG_ARG ",\"pbr\":" REG_ARG ",\"status\":" REG_ARG ","
      "\"ramBank\":" REG_ARG ",\"romBank\":" REG_ARG
      "},\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_set_registers },
    { "read_memory",
      "Read up to 4096 bytes, returned as hex. VRAM is not in the CPU map: read it with "
      "space \"vram\". For banked RAM ($A000-$BFFF) and ROM ($C000-$FFFF), 'bank' reads "
      "that bank without switching to it.",
      "{\"type\":\"object\",\"properties\":{"
      "\"address\":{\"type\":[\"string\",\"number\"],\"description\":\"e.g. \\\"$C000\\\" or \\\"0xC000\\\"\"},"
      "\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":16},"
      "\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}"
      "},\"required\":[\"address\"],\"additionalProperties\":false}",
      TOOL_READ, tool_read_memory },
    { "write_memory",
      "Write up to 4096 bytes, given as hex. VRAM is not in the CPU map: write it with "
      "space \"vram\". CPU writes go through the banks currently selected and reach I/O "
      "registers, so set ramBank or romBank first to write another bank.",
      "{\"type\":\"object\",\"properties\":{"
      ADDRESS_ARG ","
      "\"hex\":{\"type\":\"string\",\"description\":\"e.g. \\\"A9058D0004\\\"\"},"
      "\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"}"
      "},\"required\":[\"address\",\"hex\"],\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_write_memory },
    { "disassemble",
      "Disassemble CPU memory from an address.",
      "{\"type\":\"object\",\"properties\":{"
      "\"address\":{\"type\":[\"string\",\"number\"]},"
      "\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256,\"default\":16}"
      "},\"required\":[\"address\"],\"additionalProperties\":false}",
      TOOL_READ, tool_disassemble },
    { "evaluate",
      "Compute an expression without changing anything. Unsigned 32-bit, wrapping, with C "
      "precedence: || && | ^ & == != < <= > >= << >> + - * / % and unary - ! ~. Operands: $hex, "
      "0xhex, decimal, A X Y SP P PC R0-R15 rambank rombank, byte[e] word[e] vbyte[e], "
      "byte[bank:addr], and symbols such as main+4. A bare word is a register, then a symbol, "
      "and never hex. $C000 is the number, so read memory with byte[$C000].",
      "{\"type\":\"object\",\"properties\":{"
      "\"expression\":{\"type\":\"string\",\"description\":\"e.g. word[R0] + X * 2\"}"
      "},\"required\":[\"expression\"],\"additionalProperties\":false}",
      TOOL_READ, tool_evaluate },
    { "get_stack",
      "The call stack, innermost first. Frame 0 is the current PC. For a caller, address is "
      "where it returns to, callSite is its JSR, and symbol, file and line describe that JSR. "
      "Callers are a best guess, "
      "and the result says so with heuristic: true. The CPU keeps no frame pointer, so this "
      "scans the stack for addresses just past a JSR. Calls through JSL or the KERNAL's far "
      "call are not recognised, stored data can look like a caller, and 'bank' is what is "
      "mapped at that address now. startFrame and levels select part of the stack.",
      "{\"type\":\"object\",\"properties\":{"
      "\"startFrame\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":64,\"default\":0},"
      "\"levels\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":64,\"default\":0,\"description\":\"0 means all\"}"
      "},\"additionalProperties\":false}",
      TOOL_READ, tool_get_stack },
    { "set_breakpoint",
      "Stop when code runs at a CPU address (kind \"exec\"), or when it is written (\"write\"), "
      "read (\"read\") or either (\"access\"), with an optional length. Instead of 'address', "
      "'file' and 'line' set an exec breakpoint on a source line, and a line with code in "
      "several places is refused until 'bank' picks one. "
      "A read watch ignores "
      "bytes fetched as instructions unless excludeFetch is false. For exec, 'bank' pins a RAM or ROM bank, "
      "'condition' is e.g. \"A == $05\" or \"byte[$1234] != 0\", and hitCondition \"N\" "
      "stops from the Nth hit on. For a watch, 'condition' filters the byte moved, e.g. "
      "\"== $05\". With kind \"write\", space \"vram\" watches VERA video memory: it stops "
      "when the program's store lands there through a data port, including FX writes. "
      "A condition is refused where another client's breakpoint differs. "
      "'dump' makes an exec breakpoint write memory to a new host file on each hit instead of "
      "stopping: a 4-byte hit index, then each region. The file must not exist and must be "
      "inside -fsroot, or the dump folder when there is no -fsroot; it stops growing at 256 MiB. "
      "Another client's plain breakpoint at the same address still stops the machine, and "
      "alsoStops says so. list_breakpoints reports the writes. "
      "'log' makes an exec breakpoint print a line and keep running, read with read_log. "
      "{expression} parts use the language of evaluate, {{ and }} are literal braces, and a "
      "bad expression prints its error in the line. Another client's plain breakpoint at the "
      "same address still stops the machine, and alsoStops says so.",
      "{\"type\":\"object\",\"properties\":{"
      ADDRESS_ARG ","
      "\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
      "\"kind\":{\"type\":\"string\",\"enum\":[\"exec\",\"write\",\"read\",\"access\"],\"default\":\"exec\"},"
      "\"excludeFetch\":{\"type\":\"boolean\",\"default\":true},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
      "\"length\":{\"type\":\"integer\",\"minimum\":1,\"default\":1},"
      "\"condition\":{\"type\":\"string\"},"
      "\"hitCondition\":{\"type\":\"string\"},"
      "\"file\":{\"type\":\"string\"},"
      "\"line\":{\"type\":\"integer\",\"minimum\":1},"
      "\"dump\":{\"type\":\"object\",\"properties\":{"
      "\"file\":{\"type\":\"string\"},"
      "\"regions\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":16,\"items\":{\"type\":\"object\",\"properties\":{"
      ADDRESS_ARG ","
      "\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":16},"
      "\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}"
      "},\"required\":[\"address\"],\"additionalProperties\":false}}"
      "},\"required\":[\"file\",\"regions\"],\"additionalProperties\":false},"
      "\"log\":{\"type\":\"string\",\"maxLength\":255,\"description\":\"e.g. A={A} at {word[R0]}\"}"
      "},\"additionalProperties\":false}",
      TOOL_CONTROL, tool_set_breakpoint },
    { "remove_breakpoint",
      "Remove a breakpoint or watch this client set, by 'address' or by 'file' and 'line'. "
      "One another client or the command line also holds at that address stays armed, and "
      "stillArmed says so.",
      "{\"type\":\"object\",\"properties\":{"
      ADDRESS_ARG ","
      "\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
      "\"kind\":{\"type\":\"string\",\"enum\":[\"exec\",\"write\",\"read\",\"access\"],\"default\":\"exec\"},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
      "\"file\":{\"type\":\"string\"},"
      "\"line\":{\"type\":\"integer\",\"minimum\":1}"
      "},\"additionalProperties\":false}",
      TOOL_CONTROL, tool_remove_breakpoint },
    { "enable_breakpoint",
      "Turn a breakpoint or watch this client set on or off, by 'address' or by 'file' and "
      "'line', keeping its condition and hit count. The flag is shared per address, so this is "
      "refused where another client or the command line also holds it.",
      "{\"type\":\"object\",\"properties\":{"
      ADDRESS_ARG ","
      "\"enabled\":{\"type\":\"boolean\"},"
      "\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
      "\"kind\":{\"type\":\"string\",\"enum\":[\"exec\",\"write\",\"read\",\"access\"],\"default\":\"exec\"},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
      "\"file\":{\"type\":\"string\"},"
      "\"line\":{\"type\":\"integer\",\"minimum\":1}"
      "},\"required\":[\"enabled\"],\"additionalProperties\":false}",
      TOOL_CONTROL, tool_enable_breakpoint },
    { "list_breakpoints",
      "Every breakpoint and watchpoint armed in the machine, and who asked for each.",
      NO_ARGS, TOOL_READ, tool_list_breakpoints },
    { "read_log",
      "Lines printed by logpoints, from every client, oldest first. Changes nothing: pass the "
      "returned cursor back to get what came after it, so several clients can each follow the "
      "log. Without a cursor, starts at the oldest line held. The server keeps the last 1024 "
      "lines, and 'lost' counts those overwritten between your cursor and the oldest. 'more' "
      "means another call would return lines now.",
      "{\"type\":\"object\",\"properties\":{"
      "\"cursor\":{\"type\":\"integer\",\"minimum\":0},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256,\"default\":100}"
      "},\"additionalProperties\":false}",
      TOOL_READ, tool_read_log },
    { "load_symbols",
      "Load an ld65 -Ln label file (lines like \"al 00C000 .name\"), so its names work as "
      "addresses and appear in disassembly. Loading the same file again replaces its labels. "
      "'bank' says which RAM or ROM bank its labels at $A000 and up belong to. The file must "
      "be inside -fsroot (the working folder without one), the dump folder or the -prg folder.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}"
      "},\"required\":[\"path\"],\"additionalProperties\":false}",
      TOOL_CONTROL, tool_load_symbols },
    { "lookup_symbol",
      "Find a symbol. With 'name', every place it resolves to. With 'address', the nearest "
      "label at or below it and the offset from that label.",
      "{\"type\":\"object\",\"properties\":{"
      "\"name\":{\"type\":\"string\"},"
      "\"address\":{\"type\":[\"string\",\"number\"]}"
      "},\"additionalProperties\":false}",
      TOOL_READ, tool_lookup_symbol },
    { "list_symbols",
      "List known labels in address order, optionally only those containing 'filter'.",
      "{\"type\":\"object\",\"properties\":{"
      "\"filter\":{\"type\":\"string\"},"
      "\"start\":{\"type\":\"integer\",\"minimum\":0,\"default\":0},"
      "\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000,\"default\":100}"
      "},\"additionalProperties\":false}",
      TOOL_READ, tool_list_symbols },
    { "list_sources",
      "The source files named by the loaded debug info, including generated ones.",
      NO_ARGS, TOOL_READ, tool_list_sources },
    { "read_source",
      "Lines of a source file from the loaded debug info. 'file' is a name from list_sources, "
      "or enough of its path to pick one. The file must be inside -fsroot (the working folder "
      "without one), the dump folder or the -prg folder, even if the debug info names it "
      "elsewhere. Lines count from 1. Bytes that are not UTF-8 appear as U+FFFD.",
      "{\"type\":\"object\",\"properties\":{"
      "\"file\":{\"type\":\"string\"},"
      "\"startLine\":{\"type\":\"integer\",\"minimum\":1,\"default\":1},"
      "\"lineCount\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2000,\"default\":200}"
      "},\"required\":[\"file\"],\"additionalProperties\":false}",
      TOOL_READ, tool_read_source },
    { "source_location",
      "Map between code and source. With 'address' (a number, \"$C000\" or a symbol), the file "
      "and line there; bankKnown is false when that line may belong to another bank at the same "
      "address. With 'file' and 'line', every segment with code for that line, at its lowest "
      "address, with bank null until a LOAD has shown it. 'bank' narrows either. An empty list "
      "means no code.",
      "{\"type\":\"object\",\"properties\":{"
      "\"address\":{\"type\":[\"string\",\"number\"]},"
      "\"file\":{\"type\":\"string\"},"
      "\"line\":{\"type\":\"integer\",\"minimum\":1},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}"
      "},\"additionalProperties\":false}",
      TOOL_READ, tool_source_location },
    { "type_text",
      "Type text into the keyboard buffer, as a paste does. \\Xnn is a raw PETSCII byte. "
      "The machine has to be running to read it.",
      "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
      "\"required\":[\"text\"],\"additionalProperties\":false}",
      TOOL_CONTROL, tool_type_text },
    { "send_key",
      "Press, hold or release one key by its SDL name, such as \"Return\", \"F1\" or \"A\".",
      "{\"type\":\"object\",\"properties\":{"
      "\"key\":{\"type\":\"string\"},"
      "\"action\":{\"type\":\"string\",\"enum\":[\"press\",\"down\",\"up\"],\"default\":\"press\"},"
      "\"modifiers\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":8}"
      "},\"required\":[\"key\"],\"additionalProperties\":false}",
      TOOL_CONTROL, tool_send_key },
    { "joystick",
      "Drive a controller port, numbered 1 to 4 as BASIC's JOY(n) does. 'buttons' is the "
      "whole held state, so anything left out is released and [] is a connected pad with "
      "nothing held. 'release' empties the port. With neither, reports the port. Buttons: "
      "up down left right a b x y start select l r.",
      PAD_ITEM,
      TOOL_CONTROL, tool_joystick },
    { "mouse",
      "Move the mouse by 'dx' and 'dy' (down is positive) and set the held 'buttons', the "
      "whole state: left, right, middle. The program sees it once the KERNAL mouse is on, "
      "which the reply reports as 'kernalMouse'.",
      "{\"type\":\"object\",\"properties\":{"
      "\"dx\":{\"type\":\"integer\",\"minimum\":-255,\"maximum\":255},"
      "\"dy\":{\"type\":\"integer\",\"minimum\":-255,\"maximum\":255},"
      "\"wheel\":{\"type\":\"integer\",\"minimum\":-7,\"maximum\":7},"
      "\"buttons\":{\"type\":\"array\",\"items\":{\"enum\":[\"left\",\"right\",\"middle\"]}}"
      "},\"additionalProperties\":false}",
      TOOL_CONTROL, tool_mouse },
    { "set_warp",
      "Turn warp on or off. Asking for the state it is already in changes nothing.",
      "{\"type\":\"object\",\"properties\":{\"enabled\":{\"type\":\"boolean\"}},"
      "\"required\":[\"enabled\"],\"additionalProperties\":false}",
      TOOL_CONTROL, tool_set_warp },
    { "hash_memory",
      "CRC-32 and SHA-256 of a range of CPU memory or VRAM, read without side effects: no "
      "read watch fires and VERA's data port does not move. Up to 1 MiB.",
      "{\"type\":\"object\",\"properties\":{" ADDRESS_ARG ","
      "\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1048576},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
      "\"space\":{\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"}"
      "},\"required\":[\"address\",\"length\"],\"additionalProperties\":false}",
      TOOL_READ, tool_hash_memory },
    { "dump_memory",
      "Write a range of CPU memory or VRAM to a host file, read without side effects. The "
      "file must be inside -fsroot, or the dump folder when there is no -fsroot. An existing "
      "file is replaced only with 'overwrite'. Up to 1 MiB.",
      "{\"type\":\"object\",\"properties\":{" ADDRESS_ARG ","
      "\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1048576},"
      "\"path\":{\"type\":\"string\"},"
      "\"overwrite\":{\"type\":\"boolean\",\"default\":false},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
      "\"space\":{\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"}"
      "},\"required\":[\"address\",\"length\",\"path\"],\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_dump_memory },
    { "load_prg",
      "Place a .PRG in RAM while paused, at its 2-byte load address or 'address', and load "
      "the .dbg beside it. It only places the bytes: it does not run them or update BASIC's "
      "program pointers, so set the PC for machine code, and use LOAD for a BASIC program. "
      "The file must be inside -fsroot (the working folder without one), the dump folder or "
      "the -prg folder.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"address\":{\"type\":[\"string\",\"number\"]}"
      "},\"required\":[\"path\"],\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_load_prg },
    { "save_state",
      "Save the machine to a named slot in memory. It must be paused at a frame boundary: "
      "run_frames with frames 1 gets it to one.",
      "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"string\"}},"
      "\"required\":[\"slot\"],\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_save_state },
    { "load_state",
      "Replace the machine with a state from a named slot. It must be paused. Breakpoints "
      "are configuration and are kept.",
      "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"string\"}},"
      "\"required\":[\"slot\"],\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_load_state },
    { "perf_stats",
      "CPU budget and VERA bandwidth statistics. The first call turns collection on.",
      "{\"type\":\"object\",\"properties\":{"
      "\"windows\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},"
      "\"description\":\"window lengths in seconds; 0 means since start\"}"
      "},\"additionalProperties\":false}",
      TOOL_CONTROL, tool_perf_stats },
    { "perf_config",
      "Configure the CPU budget and VERA bandwidth statistics. With no arguments, reports the "
      "settings. 'enabled' is this client's own claim: statistics keep running while any other "
      "client or panel wants them, and the claim ends with the session. targetFps, idleMode, "
      "historyFrames and zones are shared with every client and the Performance panel and are "
      "not put back afterwards. 'zones' replaces the whole list, historyFrames and 'reset' "
      "discard history. Those are refused while a capture is recording.",
      "{\"type\":\"object\",\"properties\":{"
      "\"enabled\":{\"type\":\"boolean\"},"
      "\"targetFps\":{\"type\":\"number\",\"minimum\":1,\"maximum\":1000},"
      "\"idleMode\":{\"type\":\"string\",\"enum\":[\"auto\",\"markers\",\"none\"]},"
      "\"historyFrames\":{\"type\":\"integer\",\"minimum\":2,\"maximum\":65536},"
      "\"zones\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"type\":\"object\",\"properties\":{"
      "\"name\":{\"type\":\"string\"},"
      "\"start\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535},"
      "\"end\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65535,\"description\":\"exclusive\"},"
      "\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
      "\"idle\":{\"type\":\"boolean\"}"
      "},\"required\":[\"start\",\"end\"],\"additionalProperties\":false}},"
      "\"reset\":{\"type\":\"boolean\"}"
      "},\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_perf_config },
    { "perf_history",
      "The whole-program cycle histogram: totals, and a table of tasks called from a main loop "
      "named by 'caller' (start, end exclusive; 0 and 0 turn it off). Applied in order: enabled "
      "(allocates about 32 MB), caller, reset, dump. The histogram is one shared set of counters "
      "with no owner, so these affect every client and stay as set after the session. They are "
      "refused while a capture is recording. 'dump' writes the counts to a host file inside "
      "-fsroot, or the dump folder when there is no -fsroot. An existing file is replaced only "
      "with 'overwrite'. 'dumped' is the records written, or -1 if it could not be written.",
      "{\"type\":\"object\",\"properties\":{"
      "\"enabled\":{\"type\":\"boolean\"},"
      "\"caller\":{\"type\":\"object\",\"properties\":{"
      "\"start\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535},"
      "\"end\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535}"
      "},\"required\":[\"start\",\"end\"],\"additionalProperties\":false},"
      "\"reset\":{\"type\":\"boolean\"},"
      "\"dump\":{\"type\":\"string\"},"
      "\"overwrite\":{\"type\":\"boolean\",\"default\":false}"
      "},\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_perf_history },
    { "host_perf",
      "Where the host's time goes per emulated frame: emulation, input, present, the debugger "
      "UI, sleep and the debug servers, with each panel, spikes and the worst frames. Guest "
      "cycles are in perf_stats. Totals run from the last reset, which starting a capture or loading a state does.",
      NO_ARGS, TOOL_READ, tool_host_perf },
    { "profile_capture",
      "Record a stretch of running, then read where the guest's time went by routine. Always "
      "answers at once with 'state' (idle, armed, running or done): a capture spans frames, so "
      "call again with no arguments to poll, and keep the machine running or it never ends. "
      "'start' takes frames, seconds, onMarker (wait for the guest to write $F8 to $9FBC) and "
      "recorders (hist, tree, budget, vram, waterfall; default hist and tree), and goes alone. "
      "It discards the last report and the shared histogram. 'stop' ends this client's capture; "
      "another client's is refused, and this client's also ends with its session. 'load' shows "
      "a saved run, from inside -fsroot (the working folder without one), the dump folder or "
      "the -prg folder. 'export' writes the report to host files inside -fsroot, or the dump "
      "folder when there is no -fsroot: folded stacks, json, a Chrome trace (needs waterfall) "
      "or a run to load later. An existing file is replaced only with 'overwrite'. Each count "
      "in 'exported' is -1 if that file could not be written. 'top' limits the report's rows.",
      "{\"type\":\"object\",\"properties\":{"
      "\"start\":{\"type\":\"object\",\"properties\":{"
      "\"frames\":{\"type\":\"integer\",\"minimum\":1},"
      "\"seconds\":{\"type\":\"number\",\"exclusiveMinimum\":0},"
      "\"onMarker\":{\"type\":\"boolean\"},"
      "\"recorders\":{\"type\":\"array\",\"items\":{\"enum\":[\"hist\",\"tree\",\"budget\",\"vram\",\"waterfall\"]}}"
      "},\"additionalProperties\":false},"
      "\"stop\":{\"type\":\"boolean\"},"
      "\"load\":{\"type\":\"string\"},"
      "\"export\":{\"type\":\"object\",\"properties\":{"
      "\"folded\":{\"type\":\"string\"},\"json\":{\"type\":\"string\"},"
      "\"trace\":{\"type\":\"string\"},\"run\":{\"type\":\"string\"}"
      "},\"additionalProperties\":false},"
      "\"overwrite\":{\"type\":\"boolean\",\"default\":false},"
      "\"top\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100000,\"default\":50}"
      "},\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_profile_capture },
    { "get_sdcard",
      "The SD card: state and counters, the image's file index, and recent block accesses, newest "
      "first. With no card, 'attached' is false and nothing else is reported. 'image' is a host "
      "path. Other paths are inside the card's filesystem. An index the machine has made stale is "
      "brought up to date first, unless indexing is turned off in the I/O panel; then 'index.ready' "
      "may be false.",
      "{\"type\":\"object\",\"properties\":{"
      "\"files\":{\"type\":\"string\",\"enum\":[\"touched\",\"all\",\"none\"],\"default\":\"touched\","
      "\"description\":\"which index entries to list\"},"
      "\"history\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":128,"
      "\"description\":\"most history rows to return; default all held\"}"
      "},\"additionalProperties\":false}",
      TOOL_READ, tool_get_sdcard },
    { "sprite_mux_stats",
      "Per-frame sprite multiplexing: how many sprites the frame really drew, attribute writes "
      "during it, and lines that ran out of render time. The first call turns recording on "
      "(about 600 KB) as this client's claim, which ends with the session. 'lastFrame' appears "
      "once a frame completes, so keep the machine running. Its 'peakLine' is a scanline.",
      NO_ARGS, TOOL_CONTROL, tool_sprite_mux_stats },
    { "sprite_mux_config",
      "Configure sprite multiplex recording. With no arguments, reports the settings. 'enabled' "
      "is this client's own claim: recording keeps running while any other client or panel wants "
      "it, and the claim ends with the session. 'burstGap' (cycles) is one global value shared "
      "with every client and the Sprite Multiplex panel, and is not put back afterwards. 'reset' "
      "discards the shared recording.",
      "{\"type\":\"object\",\"properties\":{"
      "\"enabled\":{\"type\":\"boolean\"},"
      "\"burstGap\":{\"type\":\"integer\",\"minimum\":8,\"maximum\":2048},"
      "\"reset\":{\"type\":\"boolean\"}"
      "},\"additionalProperties\":false}",
      TOOL_DESTRUCTIVE, tool_sprite_mux_config },
    { "get_vera_state",
      "VERA's internal state, which its registers do not all read back: both data ports "
      "(address, increment, prefetch, nibble) whatever ADDRSEL is, interrupts, the scanline, "
      "and the FX state whatever DCSEL is. Fields ending 16_16 are the emulator's 16.16 "
      "fixed point. Reading it changes nothing.",
      NO_ARGS, TOOL_READ, tool_get_vera_state },
    { "get_screen",
      "The text on screen, one string per row, read from VRAM. With no 'layer' it uses the "
      "first of layers 1 and 0 that is showing text, and is an error if neither is. VERA "
      "does not record the character set, so pass 'charset' if the program has changed it: "
      "a code with no text equivalent, such as a PETSCII graphic, comes back as U+FFFD.",
      "{\"type\":\"object\",\"properties\":{"
      "\"layer\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1},"
      "\"charset\":{\"type\":\"string\",\"enum\":[\"petscii-upper\",\"petscii-lower\",\"iso\"],"
      "\"default\":\"petscii-upper\"}"
      "},\"additionalProperties\":false}",
      TOOL_READ, tool_get_screen },
    { "screenshot",
      "The picture as a PNG, with atFrameBoundary and warp. VERA draws a scanline at a "
      "time, so unless atFrameBoundary is true the image can be torn: pause and run_frames "
      "with frames 1 gets a whole one. Under warp most frames are not drawn, and the image "
      "can be up to 63 frames old.",
      NO_ARGS, TOOL_READ, tool_screenshot },
};

#define NUM_TOOLS ((int)(sizeof tools / sizeof tools[0]))

// ─── Methods ────────────────────────────────────────────────────────

static void handle_initialize(const cJSON *id, cJSON *params) {
    cJSON *pv = params ? cJSON_GetObjectItemCaseSensitive(params, "protocolVersion") : NULL;
    const char *version = mcp_protocol_versions[0];
    if (pv && cJSON_IsString(pv)) {
        for (int i = 0; i < mcp_protocol_version_count; i++) {
            if (!strcmp(pv->valuestring, mcp_protocol_versions[i])) version = mcp_protocol_versions[i];
        }
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", version);
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "x16emu");
    cJSON_AddStringToObject(info, "version", VER);
    cJSON_AddItemToObject(result, "serverInfo", info);
    cJSON_AddStringToObject(result, "instructions",
        "A Commander X16 emulator. The machine keeps running between calls unless it is "
        "paused, and every result reports paused, frame, stopId and stopReason. VRAM is a "
        "separate address space from the CPU map. step, run_frames and wait_for_stop answer "
        "when the machine stops, or with completed: false at timeoutMs while it keeps going; "
        "continue with wait_for_stop, passing the last stopId.");
    if (read_only) {
        cJSON *note = cJSON_GetObjectItemCaseSensitive(result, "instructions");
        size_t n = strlen(note->valuestring) + 128;
        char *text = (char *)malloc(n);
        if (text) {
            snprintf(text, n, "%s This server is read-only: only tools that change nothing are "
                              "offered.", note->valuestring);
            cJSON_ReplaceItemInObjectCaseSensitive(result, "instructions", cJSON_CreateString(text));
            free(text);
        }
    }
    rpc_result(id, result);
}

static void handle_tools_list(const cJSON *id) {
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < NUM_TOOLS; i++) {
        if (read_only && tools[i].effect != TOOL_READ) continue;
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", tools[i].name);
        cJSON_AddStringToObject(t, "description", tools[i].description);
        cJSON *schema = cJSON_Parse(tools[i].schema);
        cJSON_AddItemToObject(t, "inputSchema", schema ? schema : cJSON_CreateObject());
        cJSON *notes = cJSON_CreateObject();
        cJSON_AddBoolToObject(notes, "readOnlyHint", tools[i].effect == TOOL_READ);
        // Stated either way: a client assumes destructive when it is left out.
        if (tools[i].effect != TOOL_READ)
            cJSON_AddBoolToObject(notes, "destructiveHint", tools[i].effect == TOOL_DESTRUCTIVE);
        cJSON_AddItemToObject(t, "annotations", notes);
        cJSON_AddItemToArray(list, t);
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", list);
    rpc_result(id, result);
}

// The name of an argument this tool does not take, if there is one.
static const char *unknown_argument(const mcp_tool_t *tool, cJSON *args) {
    if (!args) return NULL;
    cJSON *schema = cJSON_Parse(tool->schema);
    cJSON *props = schema ? cJSON_GetObjectItemCaseSensitive(schema, "properties") : NULL;
    const char *bad = NULL;
    cJSON *a;
    cJSON_ArrayForEach(a, args) {
        if (!props || !cJSON_HasObjectItem(props, a->string)) {
            bad = a->string;
            break;
        }
    }
    cJSON_Delete(schema);
    return bad;
}

static void handle_tools_call(const cJSON *id, cJSON *params) {
    cJSON *name = params ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
    cJSON *args = params ? cJSON_GetObjectItemCaseSensitive(params, "arguments") : NULL;
    if (!name || !cJSON_IsString(name)) {
        rpc_error(id, RPC_INVALID_PARAMS, "tools/call needs a tool 'name'");
        return;
    }
    if (args && !cJSON_IsObject(args)) {
        rpc_error(id, RPC_INVALID_PARAMS, "'arguments' must be an object");
        return;
    }
    const mcp_tool_t *tool = NULL;
    for (int i = 0; i < NUM_TOOLS; i++) {
        if (!strcmp(name->valuestring, tools[i].name)) tool = &tools[i];
    }
    if (!tool) {
        char msg[128];
        snprintf(msg, sizeof msg, "Unknown tool: %.80s", name->valuestring);
        rpc_error(id, RPC_INVALID_PARAMS, msg);
        return;
    }
    if (read_only && tool->effect != TOOL_READ) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s is not available: this server is read-only (-mcp-readonly)",
                 tool->name);
        rpc_result(id, tool_fail(msg));
        return;
    }
    const char *bad = unknown_argument(tool, args);
    if (bad) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s takes no argument '%.60s'", tool->name, bad);
        rpc_result(id, tool_fail(msg));
        return;
    }
    lease_renew();
    current_id = id;
    cJSON *result = tool->run(args);
    current_id = NULL;
    if (result) rpc_result(id, result);   // otherwise it answers when it is done
}

// Ends the wait and sends nothing: a cancelled request gets no response. The
// machine carries on, since stopping it is what pause is for.
static void handle_cancelled(cJSON *params) {
    cJSON *rid = params ? cJSON_GetObjectItemCaseSensitive(params, "requestId") : NULL;
    if (rid && waiter.kind != WAIT_NONE && cJSON_Compare(rid, waiter.id, true)) {
        // An HTTP request is still open, and closing it is how it gets no answer.
        if (waiter.conn) {
            waiter.conn->waiting = false;
            waiter.conn->closing = true;
        }
        waiter_clear();
    }
}

static void handle_line(const char *line) {
    cJSON *msg = cJSON_Parse(line);
    if (!msg) {
        rpc_error(NULL, RPC_PARSE_ERROR, "Parse error");
        return;
    }
    if (!cJSON_IsObject(msg)) {
        rpc_error(NULL, RPC_INVALID_REQUEST, "Expected one JSON-RPC message per line");
        cJSON_Delete(msg);
        return;
    }
    cJSON *ver    = cJSON_GetObjectItemCaseSensitive(msg, "jsonrpc");
    cJSON *id     = cJSON_GetObjectItemCaseSensitive(msg, "id");
    cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");

    // A response to something, and this server asks nothing.
    if (!method && (cJSON_HasObjectItem(msg, "result") || cJSON_HasObjectItem(msg, "error"))) {
        cJSON_Delete(msg);
        return;
    }
    const bool id_ok = !id || cJSON_IsString(id) || cJSON_IsNumber(id);
    if (!ver || !cJSON_IsString(ver) || strcmp(ver->valuestring, "2.0") ||
        !method || !cJSON_IsString(method) || !id_ok) {
        rpc_error(id_ok ? id : NULL, RPC_INVALID_REQUEST, "Invalid JSON-RPC request");
        cJSON_Delete(msg);
        return;
    }
    if (params && !cJSON_IsObject(params)) {
        if (id) rpc_error(id, RPC_INVALID_PARAMS, "'params' must be an object");
        cJSON_Delete(msg);
        return;
    }

    // A notification gets no reply, whatever it is.
    if (!id) {
        if (!strcmp(method->valuestring, "notifications/cancelled")) handle_cancelled(params);
        cJSON_Delete(msg);
        return;
    }

    const char *m = method->valuestring;
    if (!strcmp(m, "initialize"))      handle_initialize(id, params);
    else if (!strcmp(m, "ping"))       rpc_result(id, cJSON_CreateObject());
    else if (!strcmp(m, "tools/list")) handle_tools_list(id);
    else if (!strcmp(m, "tools/call")) handle_tools_call(id, params);
    else                               rpc_error(id, RPC_METHOD_NOT_FOUND, "Method not found");
    cJSON_Delete(msg);
}

// ─── HTTP ───────────────────────────────────────────────────────────

static void conn_flush(http_conn *c) {
    while (c->out_off < c->out_len) {
        const size_t left = c->out_len - c->out_off;
        const int n = (int)send(c->sock, c->out + c->out_off,
                                (int)(left > (1u << 20) ? (1u << 20) : left), 0);
        if (n > 0) {
            c->out_off += (size_t)n;
            c->touched = SDL_GetTicks();
            continue;
        }
        if (n < 0 && MCP_WOULD_BLOCK) return;
        c->dead = true;
        return;
    }
    c->out_off = c->out_len = 0;
    if (c->closing && !c->half_closed) {
        // Closing outright with a request body still unread would reset the
        // connection and can destroy the response on its way to the client.
        shutdown(c->sock, MCP_SHUT_WRITE);
        c->half_closed = true;
        c->drain_until = SDL_GetTicks() + MCP_DRAIN_MS;
    }
}

static void conn_queue(http_conn *c, const char *data, size_t n) {
    if (c->dead) return;
    if (c->out_len + n > MCP_MAX_OUT_BYTES) {
        c->dead = true;   // nobody is reading
        return;
    }
    if (c->out_len + n > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        while (cap < c->out_len + n) cap *= 2;
        char *grown = (char *)realloc(c->out, cap);
        if (!grown) {
            c->dead = true;
            return;
        }
        c->out = grown;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, data, n);
    c->out_len += n;
}

static void http_write(http_conn *c, int status, const char *json, size_t len) {
    if (!c->keep_alive) c->closing = true;
    char head[256];
    const int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\n%s%sContent-Length: %lu\r\nCache-Control: no-store\r\nConnection: %s\r\n\r\n",
        status, mcp_http_reason(status),
        json ? "Content-Type: application/json\r\n" : "",
        status == 405 ? "Allow: POST\r\n" : "",
        (unsigned long)len, c->keep_alive ? "keep-alive" : "close");
    conn_queue(c, head, (size_t)n);
    if (json) conn_queue(c, json, len);
    conn_flush(c);
}

static void http_answer(const char *json, size_t len) {
    if (!reply_conn) return;
    replied = true;
    http_write(reply_conn, reply_status, json, len);
}

static void conn_refuse(http_conn *c, int status, const char *message) {
    c->keep_alive = false;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddNullToObject(msg, "id");
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", RPC_INVALID_REQUEST);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(msg, "error", err);
    char *printed = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    http_write(c, status, printed, printed ? strlen(printed) : 0);
    cJSON_free(printed);
}

static void http_dispatch(http_conn *c, const mcp_http_request_t *rq) {
    c->keep_alive = rq->keep_alive;
    if (rq->body_len && memchr(rq->body, '\0', rq->body_len)) {
        conn_refuse(c, 400, "the body is not JSON");
        return;
    }
    char *text = (char *)malloc(rq->body_len + 1);
    if (!text) {
        conn_refuse(c, 500, "out of memory");
        return;
    }
    memcpy(text, rq->body, rq->body_len);
    text[rq->body_len] = '\0';

    reply_conn = c;
    reply_status = 200;
    replied = false;
    handle_line(text);
    free(text);
    reply_conn = NULL;

    if (replied) return;
    if (waiter.conn == c) c->waiting = true;   // it answers when the machine does
    else                  http_write(c, 202, NULL, 0);
}

static void conn_service(http_conn *c) {
    while (c->fresh && !c->waiting && !c->closing && !c->dead) {
        mcp_http_request_t rq;
        const mcp_http_state_t st = mcp_http_parse(c->in, c->in_len, http_port, &rq);
        if (st == MCP_HTTP_INCOMPLETE) {
            if (rq.expects_continue && !c->sent_continue) {
                static const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
                c->sent_continue = true;
                conn_queue(c, go, sizeof go - 1);
                conn_flush(c);
            }
            c->fresh = false;
            return;
        }
        if (st == MCP_HTTP_REFUSED) {
            conn_refuse(c, rq.status, rq.message);
            return;
        }
        c->sent_continue = false;
        http_dispatch(c, &rq);
        memmove(c->in, c->in + rq.consumed, c->in_len - rq.consumed);
        c->in_len -= rq.consumed;
    }
}

static void conn_read(http_conn *c) {
    if (c->closing) {
        char discard[1024];
        const int n = (int)recv(c->sock, discard, sizeof discard, 0);
        if (n == 0 || (n < 0 && !MCP_WOULD_BLOCK)) c->dead = true;
        return;
    }
    const size_t space = MCP_HTTP_MAX_REQUEST - c->in_len;
    if (!space) return;   // requests sent ahead wait for this one to be answered
    const int n = (int)recv(c->sock, c->in + c->in_len, (int)space, 0);
    if (n > 0) {
        c->in_len += (size_t)n;
        c->fresh = true;
        c->touched = SDL_GetTicks();
    } else if (n == 0) {
        c->peer_closed = true;
    } else if (!MCP_WOULD_BLOCK) {
        c->dead = true;
    }
}

static void conn_release(http_conn *c) {
    // A client that hung up on a wait has cancelled it. The machine carries on.
    if (waiter.conn == c) waiter_clear();
    MCP_CLOSE_SOCK(c->sock);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->sock = MCP_NO_SOCK;
}

static void sock_set_nonblocking(mcp_sock_t s) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void http_accept(void) {
    const mcp_sock_t s = accept(listen_sock, NULL, NULL);
    if (s == MCP_NO_SOCK) return;
#ifndef _WIN32
    if (s >= FD_SETSIZE) {   // select could not watch it
        MCP_CLOSE_SOCK(s);
        return;
    }
#endif
    sock_set_nonblocking(s);
    const int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);

    for (int i = 0; i < MCP_MAX_CONNS; i++) {
        http_conn *c = &conns[i];
        if (c->sock != MCP_NO_SOCK) continue;
        c->in = (char *)malloc(MCP_HTTP_MAX_REQUEST);
        if (!c->in) break;
        c->sock = s;
        c->touched = SDL_GetTicks();
        return;
    }
    static const char busy[] =
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    (void)!send(s, busy, (int)sizeof busy - 1, 0);
    MCP_CLOSE_SOCK(s);
}

static void http_poll(void) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(listen_sock, &rd);
    mcp_sock_t high = listen_sock;
    for (int i = 0; i < MCP_MAX_CONNS; i++) {
        if (conns[i].sock == MCP_NO_SOCK) continue;
        FD_SET(conns[i].sock, &rd);
        if (conns[i].sock > high) high = conns[i].sock;
    }
    struct timeval now_only = { 0, 0 };
    const int ready = select((int)high + 1, &rd, NULL, NULL, &now_only);
    if (ready > 0 && FD_ISSET(listen_sock, &rd)) http_accept();

    const Uint32 now = SDL_GetTicks();
    for (int i = 0; i < MCP_MAX_CONNS; i++) {
        http_conn *c = &conns[i];
        if (c->sock == MCP_NO_SOCK) continue;
        if (ready > 0 && FD_ISSET(c->sock, &rd)) conn_read(c);
        conn_service(c);
        if (!c->dead) conn_flush(c);
        // Nothing more can arrive, so once its answers are out it is finished.
        // One still waiting on the machine has nobody left to tell.
        if (c->peer_closed && (c->waiting || c->out_len == c->out_off)) c->dead = true;
        if (c->half_closed && SDL_TICKS_PASSED(now, c->drain_until)) c->dead = true;
        if (!c->waiting && SDL_TICKS_PASSED(now, c->touched + idle_ms)) c->dead = true;
    }
    // Last, and for everyone: a reply to one connection can come out of another's
    // request.
    for (int i = 0; i < MCP_MAX_CONNS; i++) {
        if (conns[i].sock != MCP_NO_SOCK && conns[i].dead) conn_release(&conns[i]);
    }
}

// ─── Public API ─────────────────────────────────────────────────────

static Uint32 env_ms(const char *name, Uint32 fallback) {
    const char *v = SDL_getenv(name);
    if (!v || !*v) return fallback;
    const long ms = strtol(v, NULL, 10);
    return ms > 0 ? (Uint32)ms : fallback;
}

static const dbg_frontend_t mcp_frontend = { mcp_holds_run_state, on_frames_done, mcp_can_resume };

// Why the last socket call failed, for a message a person can act on.
static const char *mcp_sock_error(void) {
#ifdef _WIN32
    static char text[160];
    const int code = WSAGetLastError();
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)code,
                        0, text, sizeof text, NULL))
        snprintf(text, sizeof text, "error %d", code);
    return text;
#else
    return strerror(errno);
#endif
}

int mcp_server_init_http(int port) {
    if (enabled) return 0;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#else
    signal(SIGPIPE, SIG_IGN);   // a client hanging up is an error return, not an exit
#endif
    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == MCP_NO_SOCK) {
        fprintf(stderr, "[mcp] socket: %s\n", mcp_sock_error());
        return -1;
    }
    const int on = 1;
#ifdef _WIN32
    // SO_REUSEADDR on Windows would let another process share this port.
    setsockopt(listen_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on);
#else
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    // Spelled out in network order. htonl and htons are unsafe here: src/endian.h
    // shadows the system's for <netinet/in.h>, and with these includes glibc then
    // takes this for a big-endian machine and leaves the bytes alone.
    const uint8_t loopback[4] = { 127, 0, 0, 1 };
    const uint8_t port_be[2]  = { (uint8_t)(port >> 8), (uint8_t)port };
    memcpy(&addr.sin_addr, loopback, sizeof loopback);
    memcpy(&addr.sin_port, port_be, sizeof port_be);
    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(listen_sock, 8) != 0) {
        fprintf(stderr, "[mcp] cannot listen on 127.0.0.1:%d: %s\n", port, mcp_sock_error());
        MCP_CLOSE_SOCK(listen_sock);
        listen_sock = MCP_NO_SOCK;
        return -1;
    }
    sock_set_nonblocking(listen_sock);
    for (int i = 0; i < MCP_MAX_CONNS; i++) conns[i].sock = MCP_NO_SOCK;
    idle_ms  = env_ms("X16_MCP_IDLE_MS", MCP_IDLE_MS);
    lease_ms = env_ms("X16_MCP_LEASE_MS", MCP_LEASE_MS);
    http_port = port;
    http_mode = true;
    enabled = true;
    debug_server_set_frontend(&mcp_frontend);
    printf("[mcp] Model Context Protocol server listening on http://127.0.0.1:%d/mcp\n", port);
    return 0;
}

int mcp_server_sockets(uintptr_t *out, int max) {
    if (!enabled || !http_mode) return 0;
    int n = 0;
    if (n < max) out[n++] = (uintptr_t)listen_sock;
    for (int i = 0; i < MCP_MAX_CONNS && n < max; i++) {
        if (conns[i].sock != MCP_NO_SOCK) out[n++] = (uintptr_t)conns[i].sock;
    }
    return n;
}

int mcp_server_init_stdio(void) {
    if (enabled) return 0;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   // a client hanging up is an error return, not an exit
#endif
    fflush(stdout);
    out_fd = MCP_DUP(1);
    if (out_fd < 0) return -1;
    // Everything the emulator prints now lands on stderr, where it cannot be
    // mistaken for a protocol message.
    if (MCP_DUP2(2, 1) < 0) {
        const int null_fd = MCP_OPEN(MCP_NULL_DEVICE, MCP_O_WRONLY);
        if (null_fd < 0 || MCP_DUP2(null_fd, 1) < 0) return -1;
    }
#ifdef _WIN32
    _setmode(out_fd, _O_BINARY);
    _setmode(0, _O_BINARY);
#endif

    lock      = SDL_CreateMutex();
    in_space  = SDL_CreateCond();
    out_ready = SDL_CreateCond();
    out_idle  = SDL_CreateCond();
    if (!lock || !in_space || !out_ready || !out_idle) return -1;

    SDL_Thread *reader = SDL_CreateThread(reader_main, "mcp-stdin", NULL);
    SDL_Thread *writer = SDL_CreateThread(writer_main, "mcp-stdout", NULL);
    if (!reader || !writer) return -1;
    // Each can be blocked in the OS for as long as the client likes, so neither
    // is ever joined. They end with the process.
    SDL_DetachThread(reader);
    SDL_DetachThread(writer);

    enabled = true;
    debug_server_set_frontend(&mcp_frontend);
    return 0;
}

bool mcp_server_is_enabled(void) {
    return enabled;
}

void mcp_server_set_readonly(bool on) {
    read_only = on;
}

bool mcp_server_is_readonly(void) {
    return read_only;
}

void mcp_server_note_stopped(void) {
    if (!enabled || was_stopped) return;
    was_stopped = true;
    stop_id++;
}

void mcp_server_note_resumed(void) {
    was_stopped = false;
}

int mcp_server_poll(void) {
    if (!enabled) return 0;

    track_stops();

    const int mode_before = currentMode;
    if (http_mode) {
        waiter_poll();
        http_poll();
        lease_poll();
        return currentMode != mode_before;
    }
    for (int n = 0; n < MCP_LINES_PER_POLL; n++) {
        SDL_LockMutex(lock);
        in_line *l = in_head;
        if (l) {
            in_head = l->next;
            if (!in_head) in_tail = NULL;
            in_count--;
            SDL_CondSignal(in_space);
        }
        SDL_UnlockMutex(lock);
        if (!l) break;
        handle_line(l->text);
        free(l->text);
        free(l);
    }
    waiter_poll();

    // The client owns this process's lifetime: it launched it over these pipes.
    SDL_LockMutex(lock);
    const bool gone = (in_eof && !in_head) || out_broken;
    SDL_UnlockMutex(lock);
    if (gone && !quit_sent) {
        quit_sent = true;
        SDL_Event quit_event;
        memset(&quit_event, 0, sizeof quit_event);
        quit_event.type = SDL_QUIT;
        SDL_PushEvent(&quit_event);
    }
    return currentMode != mode_before;
}

void mcp_server_shutdown(void) {
    if (!enabled) return;
    enabled = false;
    debug_server_set_frontend(NULL);
    waiter_clear();
    if (http_mode) {
        for (int i = 0; i < MCP_MAX_CONNS; i++) {
            if (conns[i].sock == MCP_NO_SOCK) continue;
            conn_flush(&conns[i]);
            conn_release(&conns[i]);
        }
        MCP_CLOSE_SOCK(listen_sock);
        listen_sock = MCP_NO_SOCK;
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }
    // Let replies already queued reach the client, without trusting it to read.
    SDL_LockMutex(lock);
    const Uint32 deadline = SDL_GetTicks() + 500;
    while ((out_head || out_busy) && !out_broken && !SDL_TICKS_PASSED(SDL_GetTicks(), deadline))
        SDL_CondWaitTimeout(out_idle, lock, 20);
    out_stop = true;
    SDL_CondBroadcast(out_ready);
    SDL_CondBroadcast(in_space);
    SDL_UnlockMutex(lock);
}
