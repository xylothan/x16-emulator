// Checks for the MCP server's HTTP request rules (src/mcp_http.c).
//
// Each case is a whole request as a client would send it, against a server on
// port 9100, so the order of the checks is covered as well as the checks.

#include "mcp_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT 9100

static int failures = 0;

static void check(bool cond, const char *what) {
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    } else {
        printf("ok  : %s\n", what);
    }
}

static mcp_http_state_t parse(const char *text, mcp_http_request_t *rq) {
    return mcp_http_parse(text, strlen(text), PORT, rq);
}

static void expect_refused(const char *text, int status, const char *what) {
    mcp_http_request_t rq;
    const mcp_http_state_t st = parse(text, &rq);
    check(st == MCP_HTTP_REFUSED && rq.status == status && !rq.keep_alive, what);
}

#define GOOD_HEADERS "Host: 127.0.0.1:9100\r\nContent-Type: application/json\r\n"

static void test_accepts(void) {
    mcp_http_request_t rq;
    const char *plain = "POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: 2\r\n\r\n{}";
    check(parse(plain, &rq) == MCP_HTTP_READY, "a plain request");
    check(rq.body_len == 2 && !memcmp(rq.body, "{}", 2), "its body");
    check(rq.consumed == strlen(plain), "its length");
    check(rq.keep_alive, "HTTP/1.1 keeps the connection");

    char two[256];
    snprintf(two, sizeof two, "%s%s", plain, "POST /mcp");
    check(mcp_http_parse(two, strlen(two), PORT, &rq) == MCP_HTTP_READY &&
          rq.consumed == strlen(plain), "the next request is left in the buffer");

    check(parse("POST /mcp HTTP/1.1\r\nhost: LOCALHOST:9100\r\ncontent-type: Application/JSON; charset=utf-8\r\n"
                "content-length: 0\r\n\r\n", &rq) == MCP_HTTP_READY, "header names and values ignore case");
    check(parse("POST /mcp?x=1 HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: 0\r\n\r\n", &rq) == MCP_HTTP_READY,
          "a query string is not a different path");
    check(parse("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Connection: close\r\nContent-Length: 0\r\n\r\n", &rq)
          == MCP_HTTP_READY && !rq.keep_alive, "Connection: close");
    check(parse("POST /mcp HTTP/1.0\r\n" GOOD_HEADERS "Content-Length: 0\r\n\r\n", &rq)
          == MCP_HTTP_READY && !rq.keep_alive, "HTTP/1.0 closes by default");
    check(parse("POST /mcp HTTP/1.0\r\n" GOOD_HEADERS "Connection: keep-alive\r\nContent-Length: 0\r\n\r\n", &rq)
          == MCP_HTTP_READY && rq.keep_alive, "HTTP/1.0 can ask to stay open");

    for (int i = 0; i < mcp_protocol_version_count; i++) {
        char text[256];
        snprintf(text, sizeof text, "POST /mcp HTTP/1.1\r\n" GOOD_HEADERS
                 "MCP-Protocol-Version: %s\r\nContent-Length: 0\r\n\r\n", mcp_protocol_versions[i]);
        check(parse(text, &rq) == MCP_HTTP_READY, mcp_protocol_versions[i]);
    }
}

static void test_incomplete(void) {
    mcp_http_request_t rq;
    check(parse("", &rq) == MCP_HTTP_INCOMPLETE, "nothing yet");
    check(parse("POST /mcp HTTP/1.1\r\nHost: 127", &rq) == MCP_HTTP_INCOMPLETE, "half the headers");
    check(parse("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: 5\r\n\r\n{}", &rq)
          == MCP_HTTP_INCOMPLETE && !rq.expects_continue, "half the body");
    check(parse("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Expect: 100-continue\r\nContent-Length: 5\r\n\r\n", &rq)
          == MCP_HTTP_INCOMPLETE && rq.expects_continue, "a client waiting for 100 Continue");
}

static void test_who_is_asking(void) {
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Origin: https://example.com\r\nContent-Length: 0\r\n\r\n",
                   403, "any Origin");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Origin: http://127.0.0.1:9100\r\nContent-Length: 0\r\n\r\n",
                   403, "even a loopback Origin");
    expect_refused("POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n",
                   403, "no Host");
    expect_refused("POST /mcp HTTP/1.1\r\nHost: evil.example:9100\r\nContent-Type: application/json\r\n"
                   "Content-Length: 0\r\n\r\n", 403, "a rebinding Host");
    expect_refused("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:9101\r\nContent-Type: application/json\r\n"
                   "Content-Length: 0\r\n\r\n", 403, "another port");
    expect_refused("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                   "Content-Length: 0\r\n\r\n", 403, "no port in Host");
    expect_refused("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:9100\r\nHost: evil.example\r\n"
                   "Content-Type: application/json\r\nContent-Length: 0\r\n\r\n", 403, "two Hosts");
    expect_refused("OPTIONS /mcp HTTP/1.1\r\nHost: 127.0.0.1:9100\r\n\r\n", 403, "a preflight");
    expect_refused("OPTIONS /anything HTTP/1.1\r\nHost: 127.0.0.1:9100\r\n\r\n", 403, "a preflight anywhere");
    expect_refused("GET /mcp HTTP/1.1\r\nHost: evil.example\r\n\r\n", 403, "who is asking comes first");

    mcp_http_request_t rq;
    const char *on_80 = "POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                        "Content-Length: 0\r\n\r\n";
    check(mcp_http_parse(on_80, strlen(on_80), 80, &rq) == MCP_HTTP_READY, "port 80 can be left out");
}

static void test_what_is_asked(void) {
    expect_refused("GET /mcp HTTP/1.1\r\nHost: 127.0.0.1:9100\r\n\r\n", 405, "GET");
    expect_refused("DELETE /mcp HTTP/1.1\r\nHost: 127.0.0.1:9100\r\n\r\n", 405, "DELETE");
    expect_refused("PUT /mcp HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: 0\r\n\r\n", 405, "PUT");
    expect_refused("POST / HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: 0\r\n\r\n", 404, "another path");
    expect_refused("POST /mcpx HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: 0\r\n\r\n", 404, "a longer path");
    expect_refused("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:9100\r\nContent-Length: 0\r\n\r\n",
                   415, "no Content-Type");
    expect_refused("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:9100\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 0\r\n\r\n", 415, "text/plain, which needs no preflight");
    expect_refused("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:9100\r\nContent-Type: application/jsonx\r\n"
                   "Content-Length: 0\r\n\r\n", 415, "a longer media type");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "MCP-Protocol-Version: 2026-07-28\r\nContent-Length: 0\r\n\r\n",
                   400, "an unsupported revision");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: 65537\r\n\r\n",
                   413, "a body over the limit, before it arrives");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: 99999999999999999999999\r\n\r\n",
                   413, "a length that would overflow");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "\r\n", 411, "no Content-Length");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: -1\r\n\r\n", 400, "a negative length");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: +1\r\n\r\n", 400, "a signed length");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Content-Length: 1\r\nContent-Length: 2\r\n\r\n",
                   400, "two lengths");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Transfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n{}",
                   501, "chunked, which could frame the stream differently");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS "Expect: magic\r\nContent-Length: 0\r\n\r\n",
                   417, "an unknown expectation");
}

static void test_malformed(void) {
    expect_refused("POST\r\nHost: x\r\n\r\n", 400, "a request line with no target");
    expect_refused("POST /mcp\r\nHost: x\r\n\r\n", 400, "a request line with no version");
    expect_refused("POST /mcp HTTP/2.0\r\n" GOOD_HEADERS "Content-Length: 0\r\n\r\n", 400, "another HTTP version");
    expect_refused("POST /mcp HTTP/1.1\r\nHost : 127.0.0.1:9100\r\n\r\n", 400, "a space before the colon");
    expect_refused("POST /mcp HTTP/1.1\r\n" GOOD_HEADERS " folded: value\r\nContent-Length: 0\r\n\r\n",
                   400, "a folded header");
    expect_refused("POST /mcp HTTP/1.1\r\nno colon here\r\n\r\n", 400, "a line that is not a header");
    expect_refused("POST /mcp HTTP/1.1\nHost: 127.0.0.1:9100\r\n\r\n", 400, "a bare newline");
    expect_refused("POST /mcp HTTP/1.1\r\nX-Bad: a\x01z\r\n\r\n", 400, "a control character");

    char *big = (char *)malloc(MCP_HTTP_MAX_HEADERS + 64);
    memset(big, 'a', MCP_HTTP_MAX_HEADERS + 63);
    big[MCP_HTTP_MAX_HEADERS + 63] = '\0';
    expect_refused(big, 431, "headers past the limit");
    free(big);
}

static void test_reasons(void) {
    static const int codes[] = { 200, 202, 400, 403, 404, 405, 411, 413, 415, 417, 431, 500, 501, 503 };
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        check(strcmp(mcp_http_reason(codes[i]), "Error") != 0, "a status line has its own reason");
    }
}

int main(void) {
    test_accepts();
    test_incomplete();
    test_who_is_asking();
    test_what_is_asked();
    test_malformed();
    test_reasons();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
