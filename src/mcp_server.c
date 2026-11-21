// Commander X16 Emulator — Model Context Protocol server
//
// Two small threads move bytes and touch nothing else: one blocks on stdin and
// queues complete lines, the other blocks on stdout and drains a queue of
// replies. Everything that reads or changes the machine runs on the emulation
// thread, from mcp_server_poll. A client that stops reading therefore never
// stalls emulation, and neither does a client that sends nothing.

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "mcp_server.h"
#include "debug_server_call.h"
#include "debugger.h"
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
#else
  #include <fcntl.h>
  #include <unistd.h>
  #define MCP_READ(fd, buf, n)  read((fd), (buf), (n))
  #define MCP_WRITE(fd, buf, n) write((fd), (buf), (n))
  #define MCP_DUP               dup
  #define MCP_DUP2              dup2
  #define MCP_NULL_DEVICE       "/dev/null"
  #define MCP_OPEN              open
  #define MCP_O_WRONLY          O_WRONLY
#endif

#define MCP_MAX_LINE       (1024 * 1024)
#define MCP_MAX_QUEUED     256                 // lines read ahead of the machine
#define MCP_MAX_OUT_BYTES  (64u * 1024 * 1024) // past this the client is gone
#define MCP_LINES_PER_POLL 16
#define MCP_READ_LIMIT     4096
#define MCP_DISASM_LIMIT   256

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

static void rpc_send(cJSON *msg) {
    char *printed = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!printed) return;
    const size_t len = strlen(printed);
    char *text = (char *)malloc(len + 2);
    if (text) {
        memcpy(text, printed, len + 1);
        out_send(text);
    }
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
static void add_machine_state(cJSON *data) {
    dbg_reply_t st;
    debug_server_call("x16/status", NULL, &st);
    bool paused = false;
    if (st.body) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(st.body, "paused");
        cJSON *f = cJSON_GetObjectItemCaseSensitive(st.body, "frame");
        paused = p && cJSON_IsTrue(p);
        if (!cJSON_HasObjectItem(data, "paused"))
            cJSON_AddBoolToObject(data, "paused", paused);
        if (f && !cJSON_HasObjectItem(data, "frame"))
            cJSON_AddNumberToObject(data, "frame", f->valuedouble);
        cJSON_Delete(st.body);
    }
    cJSON_AddNumberToObject(data, "stopId", stop_id);
    const char *why = paused ? DEBUGGetStopReason() : NULL;
    if (why && *why) cJSON_AddStringToObject(data, "stopReason", why);
    else             cJSON_AddNullToObject(data, "stopReason");
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
static bool arg_address(cJSON *args, bool vram, char *out, size_t n, char *err, size_t errn) {
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
        snprintf(out, n, "%s%s", prefix, a->valuestring);
        return true;
    }
    snprintf(err, errn, "'address' is required: a number, or a string like \"$C000\" or \"0xC000\"");
    return false;
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
    char err[160], ref[48];
    cJSON *sp = args ? cJSON_GetObjectItemCaseSensitive(args, "space") : NULL;
    if (sp && (!cJSON_IsString(sp) ||
               (strcmp(sp->valuestring, "cpu") && strcmp(sp->valuestring, "vram"))))
        return tool_fail("'space' must be \"cpu\" or \"vram\"");
    const bool vram = sp && !strcmp(sp->valuestring, "vram");

    int length, bank;
    if (!arg_address(args, vram, ref, sizeof ref, err, sizeof err) ||
        !arg_int(args, "length", 16, 1, MCP_READ_LIMIT, &length, err, sizeof err) ||
        !arg_int(args, "bank", -1, 0, 255, &bank, err, sizeof err))
        return tool_fail(err);
    if (vram && bank >= 0)
        return tool_fail("VRAM has no banks: leave out 'bank' when 'space' is \"vram\"");

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
    char err[160], ref[48];
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
    char *text = (char *)malloc((size_t)rows * 96 + 1);
    size_t o = 0;
    if (text) {
        text[0] = '\0';
        cJSON *row;
        cJSON_ArrayForEach(row, list) {
            cJSON *a = cJSON_GetObjectItemCaseSensitive(row, "address");
            cJSON *b = cJSON_GetObjectItemCaseSensitive(row, "instructionBytes");
            cJSON *i = cJSON_GetObjectItemCaseSensitive(row, "instruction");
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

typedef struct {
    const char *name;
    const char *description;
    const char *schema;     // JSON Schema of the arguments
    bool        read_only;
    cJSON     *(*run)(cJSON *args);
} mcp_tool_t;

#define NO_ARGS "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"

static const mcp_tool_t tools[] = {
    { "get_status",
      "Is the machine paused, on which frame, and at how many cycles.",
      NO_ARGS, true, tool_get_status },
    { "get_registers",
      "CPU registers, including the 65C816 extras and the current RAM and ROM banks.",
      NO_ARGS, true, tool_get_registers },
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
      true, tool_read_memory },
    { "disassemble",
      "Disassemble CPU memory from an address.",
      "{\"type\":\"object\",\"properties\":{"
      "\"address\":{\"type\":[\"string\",\"number\"]},"
      "\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256,\"default\":16}"
      "},\"required\":[\"address\"],\"additionalProperties\":false}",
      true, tool_disassemble },
    { "list_breakpoints",
      "Every breakpoint and watchpoint armed in the machine, and who asked for each.",
      NO_ARGS, true, tool_list_breakpoints },
};

#define NUM_TOOLS ((int)(sizeof tools / sizeof tools[0]))

// ─── Methods ────────────────────────────────────────────────────────

static const char *const protocol_versions[] = {
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05",
};

static void handle_initialize(const cJSON *id, cJSON *params) {
    cJSON *pv = params ? cJSON_GetObjectItemCaseSensitive(params, "protocolVersion") : NULL;
    const char *version = protocol_versions[0];
    if (pv && cJSON_IsString(pv)) {
        for (size_t i = 0; i < sizeof protocol_versions / sizeof protocol_versions[0]; i++) {
            if (!strcmp(pv->valuestring, protocol_versions[i])) version = protocol_versions[i];
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
        "separate address space from the CPU map.");
    rpc_result(id, result);
}

static void handle_tools_list(const cJSON *id) {
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < NUM_TOOLS; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", tools[i].name);
        cJSON_AddStringToObject(t, "description", tools[i].description);
        cJSON *schema = cJSON_Parse(tools[i].schema);
        cJSON_AddItemToObject(t, "inputSchema", schema ? schema : cJSON_CreateObject());
        cJSON *notes = cJSON_CreateObject();
        cJSON_AddBoolToObject(notes, "readOnlyHint", tools[i].read_only);
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
    const char *bad = unknown_argument(tool, args);
    if (bad) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s takes no argument '%.60s'", tool->name, bad);
        rpc_result(id, tool_fail(msg));
        return;
    }
    rpc_result(id, tool->run(args));
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

// ─── Public API ─────────────────────────────────────────────────────

int mcp_server_init_stdio(void) {
    if (enabled) return 0;
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
    return 0;
}

bool mcp_server_is_enabled(void) {
    return enabled;
}

int mcp_server_poll(void) {
    if (!enabled) return 0;

    const bool stopped = DEBUGIsPaused();
    if (stopped && !was_stopped) stop_id++;
    was_stopped = stopped;

    const int mode_before = currentMode;
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
