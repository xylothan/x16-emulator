// Commander X16 Emulator — HTTP request checks for the MCP server
//
// The server listens on loopback, and that alone does not keep a web page out:
// a browser will happily send a request to 127.0.0.1, and DNS rebinding can make
// one look same-origin. So anything carrying an Origin is refused, the Host has
// to name this server, and a body has to be application/json, which a page
// cannot send without a CORS preflight that is refused too.
//
// Every refusal closes the connection. Nothing tries to skip a body it did not
// accept, so two requests can never be read out of one stream differently from
// how a proxy or client framed them.

#include "mcp_http.h"

#include <stdio.h>
#include <string.h>

const char *const mcp_protocol_versions[] = {
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05",
};

const int mcp_protocol_version_count =
    (int)(sizeof mcp_protocol_versions / sizeof mcp_protocol_versions[0]);

bool mcp_protocol_version_supported(const char *version, size_t len) {
    for (int i = 0; i < mcp_protocol_version_count; i++) {
        if (strlen(mcp_protocol_versions[i]) == len &&
            !memcmp(mcp_protocol_versions[i], version, len))
            return true;
    }
    return false;
}

const char *mcp_http_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 'a' - 'A') : c;
}

// A span against a lowercase literal, ignoring ASCII case.
static bool span_is(const char *s, size_t n, const char *literal) {
    if (strlen(literal) != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (lower(s[i]) != literal[i]) return false;
    }
    return true;
}

static bool is_token_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

typedef struct {
    const char *value;
    size_t      len;
    int         count;
} header_t;

static void note(header_t *h, const char *value, size_t len) {
    h->value = value;
    h->len = len;
    h->count++;
}

static mcp_http_state_t refuse(mcp_http_request_t *out, int status, const char *message) {
    out->status = status;
    out->message = message;
    out->keep_alive = false;
    return MCP_HTTP_REFUSED;
}

static bool host_allowed(const header_t *host, int port) {
    char want[32];
    snprintf(want, sizeof want, "127.0.0.1:%d", port);
    if (span_is(host->value, host->len, want)) return true;
    snprintf(want, sizeof want, "localhost:%d", port);
    if (span_is(host->value, host->len, want)) return true;
    // A client leaves the port out only when it is the scheme's default.
    return port == 80 && (span_is(host->value, host->len, "127.0.0.1") ||
                          span_is(host->value, host->len, "localhost"));
}

static bool content_type_is_json(const header_t *ct) {
    size_t n = 0;
    while (n < ct->len && ct->value[n] != ';') n++;
    while (n > 0 && (ct->value[n - 1] == ' ' || ct->value[n - 1] == '\t')) n--;
    return span_is(ct->value, n, "application/json");
}

// Whether a comma-separated header lists this token.
static bool lists_token(const header_t *h, const char *token) {
    size_t i = 0;
    while (i < h->len) {
        size_t start = i;
        while (i < h->len && h->value[i] != ',') i++;
        size_t end = i;
        while (start < end && (h->value[start] == ' ' || h->value[start] == '\t')) start++;
        while (end > start && (h->value[end - 1] == ' ' || h->value[end - 1] == '\t')) end--;
        if (span_is(h->value + start, end - start, token)) return true;
        i++;
    }
    return false;
}

static const char *find_header_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i;
    }
    return NULL;
}

mcp_http_state_t mcp_http_parse(const char *buf, size_t len, int port, mcp_http_request_t *out) {
    memset(out, 0, sizeof *out);

    const size_t search = len < MCP_HTTP_MAX_HEADERS ? len : MCP_HTTP_MAX_HEADERS;
    const char *header_end = find_header_end(buf, search);
    if (!header_end) {
        if (len >= MCP_HTTP_MAX_HEADERS)
            return refuse(out, 431, "the request headers are too large");
        return MCP_HTTP_INCOMPLETE;
    }

    // The request line: METHOD SP target SP version.
    const char *line_end = buf;
    while (line_end < header_end && *line_end != '\r' && *line_end != '\n') line_end++;
    if (line_end < header_end && (line_end[0] != '\r' || line_end[1] != '\n'))
        return refuse(out, 400, "malformed request line");

    const char *method = buf;
    const char *sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (!sp1 || sp1 == method) return refuse(out, 400, "malformed request line");
    for (const char *p = method; p < sp1; p++) {
        if (!is_token_char(*p)) return refuse(out, 400, "malformed request line");
    }
    const char *target = sp1 + 1;
    const char *sp2 = memchr(target, ' ', (size_t)(line_end - target));
    if (!sp2 || sp2 == target) return refuse(out, 400, "malformed request line");
    const char *version = sp2 + 1;
    const size_t version_len = (size_t)(line_end - version);
    const bool http11 = version_len == 8 && !memcmp(version, "HTTP/1.1", 8);
    const bool http10 = version_len == 8 && !memcmp(version, "HTTP/1.0", 8);
    if (!http11 && !http10) return refuse(out, 400, "only HTTP/1.x is supported");

    header_t host = {0}, origin = {0}, content_type = {0}, content_length = {0};
    header_t transfer = {0}, protocol = {0}, connection = {0}, expect = {0};

    const char *p = line_end < header_end ? line_end + 2 : header_end;
    while (p < header_end) {
        const char *eol = p;
        while (eol < header_end && *eol != '\r' && *eol != '\n') eol++;
        if (eol < header_end && (eol[0] != '\r' || eol[1] != '\n'))
            return refuse(out, 400, "malformed header");

        const char *colon = p;
        while (colon < eol && is_token_char(*colon)) colon++;
        // Catches an empty name, a space before the colon, and a folded line.
        if (colon == p || colon == eol || *colon != ':')
            return refuse(out, 400, "malformed header");
        const size_t name_len = (size_t)(colon - p);

        const char *value = colon + 1;
        const char *value_end = eol;
        while (value < value_end && (*value == ' ' || *value == '\t')) value++;
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) value_end--;
        for (const char *v = value; v < value_end; v++) {
            const unsigned char c = (unsigned char)*v;
            if ((c < 0x20 && c != '\t') || c == 0x7F)
                return refuse(out, 400, "malformed header");
        }
        const size_t value_len = (size_t)(value_end - value);

        if      (span_is(p, name_len, "host"))                 note(&host, value, value_len);
        else if (span_is(p, name_len, "origin"))               note(&origin, value, value_len);
        else if (span_is(p, name_len, "content-type"))         note(&content_type, value, value_len);
        else if (span_is(p, name_len, "content-length"))       note(&content_length, value, value_len);
        else if (span_is(p, name_len, "transfer-encoding"))    note(&transfer, value, value_len);
        else if (span_is(p, name_len, "mcp-protocol-version")) note(&protocol, value, value_len);
        else if (span_is(p, name_len, "connection"))           note(&connection, value, value_len);
        else if (span_is(p, name_len, "expect"))               note(&expect, value, value_len);

        p = eol < header_end ? eol + 2 : header_end;
    }

    // Who is asking comes before what is being asked, so that a page learns
    // nothing about this server from which error it gets.
    if (origin.count) return refuse(out, 403, "requests from a web page are not accepted");
    if (host.count != 1 || !host_allowed(&host, port))
        return refuse(out, 403, "the Host header does not name this server");

    const size_t method_len = (size_t)(sp1 - method);
    if (method_len == 7 && !memcmp(method, "OPTIONS", 7))
        return refuse(out, 403, "cross-origin requests are not accepted");

    size_t path_len = 0;
    const size_t target_len = (size_t)(sp2 - target);
    while (path_len < target_len && target[path_len] != '?') path_len++;
    if (path_len != 4 || memcmp(target, "/mcp", 4)) return refuse(out, 404, "the endpoint is /mcp");
    if (method_len != 4 || memcmp(method, "POST", 4))
        return refuse(out, 405, "this endpoint takes POST");

    if (transfer.count) return refuse(out, 501, "Transfer-Encoding is not supported");
    if (content_type.count != 1 || !content_type_is_json(&content_type))
        return refuse(out, 415, "Content-Type must be application/json");
    if (protocol.count > 1) return refuse(out, 400, "more than one MCP-Protocol-Version");
    if (protocol.count && !mcp_protocol_version_supported(protocol.value, protocol.len))
        return refuse(out, 400, "unsupported MCP-Protocol-Version");

    if (!content_length.count) return refuse(out, 411, "Content-Length is required");
    if (content_length.count > 1) return refuse(out, 400, "more than one Content-Length");
    if (!content_length.len) return refuse(out, 400, "malformed Content-Length");
    size_t body_len = 0;
    for (size_t i = 0; i < content_length.len; i++) {
        const char c = content_length.value[i];
        if (c < '0' || c > '9') return refuse(out, 400, "malformed Content-Length");
        body_len = body_len * 10 + (size_t)(c - '0');
        if (body_len > MCP_HTTP_MAX_BODY) return refuse(out, 413, "the request body is too large");
    }

    bool wants_continue = false;
    if (expect.count) {
        if (expect.count > 1 || !span_is(expect.value, expect.len, "100-continue"))
            return refuse(out, 417, "only Expect: 100-continue is supported");
        wants_continue = http11;
    }

    const bool closes = connection.count && lists_token(&connection, "close");
    const bool keeps  = connection.count && lists_token(&connection, "keep-alive");
    out->keep_alive = !closes && (http11 || keeps);

    const size_t header_size = (size_t)(header_end - buf) + 4;
    if (len - header_size < body_len) {
        out->expects_continue = wants_continue;
        return MCP_HTTP_INCOMPLETE;
    }
    out->body = buf + header_size;
    out->body_len = body_len;
    out->consumed = header_size + body_len;
    return MCP_HTTP_READY;
}
