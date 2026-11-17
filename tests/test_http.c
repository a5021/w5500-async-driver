/**
 * @file test_http.c
 * @brief Unit tests for the core HTTP codec (w5500_http.h).
 */
#include <stdio.h>
#include <string.h>

#include "w5500_http.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long _a = (long)(a);                                                 \
        long _b = (long)(b);                                                 \
        if (_a != _b) {                                                      \
            printf("FAIL %s:%d: %s == %s (%ld != %ld)\n", __FILE__,          \
                   __LINE__, #a, #b, _a, _b);                                \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_parse_response(void)
{
    static const uint8_t rsp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    w5500_http_response_t out;

    CHECK_EQ(w5500_http_parse_response(rsp, sizeof(rsp) - 1u, &out), 0);
    CHECK_EQ(out.status, 200);
    CHECK_EQ(out.reason_len, 2);
    CHECK_EQ(memcmp(out.reason, "OK", 2), 0);
    CHECK_EQ(out.body_len, 5);
    CHECK_EQ(out.truncated, 0);
    CHECK_EQ(memcmp(out.body, "hello", 5), 0);
    {
        uint16_t vlen = 0u;
        const char *v = w5500_http_header_value(out.headers, out.headers_len,
                                          "Content-Length", &vlen);
        CHECK(v != NULL);
        CHECK_EQ(vlen, 1);
        CHECK_EQ(v != NULL && v[0] == '5', 1);
        v = w5500_http_header_value(out.headers, out.headers_len, "X-Nope", &vlen);
        CHECK(v == NULL);
    }
}

static void test_parse_response_errors(void)
{
    static const uint8_t rsp[] =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    w5500_http_response_t out;

    CHECK_EQ(w5500_http_parse_response(rsp, sizeof(rsp) - 1u, &out), 0);
    CHECK_EQ(out.status, 404);
    CHECK_EQ(out.reason_len, 9);
    CHECK_EQ(out.body_len, 0);

    /* Truncated before the header terminator. */
    CHECK_EQ(w5500_http_parse_response(rsp, 20u, &out), -1);
    /* Bad protocol marker. */
    {
        uint8_t bad[sizeof(rsp)];
        memcpy(bad, rsp, sizeof(rsp));
        bad[1] = 'X';
        CHECK_EQ(w5500_http_parse_response(bad, sizeof(bad) - 1u, &out), -1);
    }
    /* Non-numeric Content-Length. */
    {
        static const uint8_t bad[] =
            "HTTP/1.0 200 OK\r\nContent-Length: xx\r\n\r\n";
        CHECK_EQ(w5500_http_parse_response(bad, sizeof(bad) - 1u, &out), -1);
    }
    /* Missing reason phrase. */
    {
        static const uint8_t bad[] = "HTTP/1.0 200\r\n\r\n";
        CHECK_EQ(w5500_http_parse_response(bad, sizeof(bad) - 1u, &out), -1);
    }
    CHECK_EQ(w5500_http_parse_response(NULL, sizeof(rsp), &out), -1);
    CHECK_EQ(w5500_http_parse_response(rsp, sizeof(rsp), NULL), -1);
}

static void test_parse_truncated_body(void)
{
    static const uint8_t rsp[] =
        "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n1234";
    w5500_http_response_t out;

    CHECK_EQ(w5500_http_parse_response(rsp, sizeof(rsp) - 1u, &out), 0);
    CHECK_EQ(out.body_len, 4);
    CHECK_EQ(out.truncated, 1);
}

static void test_build_get(void)
{
    uint8_t buf[128];
    uint16_t n;

    n = w5500_http_build_get(buf, sizeof(buf), "/index.html", "example.com");
    CHECK_EQ(n, 47);
    CHECK_EQ(memcmp(buf, "GET /index.html HTTP/1.0\r\nHost: example.com\r\n\r\n",
                    n), 0);
    CHECK_EQ(w5500_http_build_get(buf, 10u, "/", "x"), 0); /* too small */
}

static void test_parse_request_line(void)
{
    static const uint8_t req[] = "GET /api/v1 HTTP/1.1\r\nHost: x\r\n\r\n";
    w5500_http_request_t out;

    CHECK_EQ(w5500_http_parse_request_line(req, sizeof(req) - 1u, &out), 0);
    CHECK_EQ(out.method_len, 3);
    CHECK_EQ(memcmp(out.method, "GET", 3), 0);
    CHECK_EQ(out.path_len, 7);
    CHECK_EQ(memcmp(out.path, "/api/v1", 7), 0);

    CHECK_EQ(w5500_http_parse_request_line((const uint8_t *)"GET /", 5u, &out), -1);
    CHECK_EQ(w5500_http_parse_request_line((const uint8_t *)" / HTTP/1.1\r\n", 12u,
                                     &out), -1);
    CHECK_EQ(w5500_http_parse_request_line(NULL, sizeof(req), &out), -1);
}

static void test_build_response(void)
{
    static const uint8_t body[] = "hello";
    uint8_t buf[160];
    uint16_t n;

    n = w5500_http_build_response(buf, sizeof(buf), 200, "text/plain", body, 5u);
    CHECK_EQ(n, 69);
    CHECK_EQ(memcmp(buf,
                    "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                    "Content-Length: 5\r\n\r\nhello", n), 0);
    CHECK_EQ(w5500_http_build_response(buf, 10u, 200, "text/plain", body, 5u), 0);
}

int main(void)
{
    test_parse_response();
    test_parse_response_errors();
    test_parse_truncated_body();
    test_build_get();
    test_parse_request_line();
    test_build_response();

    if (g_failures == 0) {
        printf("OK: all http tests passed\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
