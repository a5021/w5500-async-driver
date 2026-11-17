/**
 * @file fuzz_http.c
 * @brief Fuzzes the core HTTP codec: parsers, header lookup and builders.
 *
 * Properties:
 *   1. No out-of-bounds access for any input (ASan).
 *   2. Both parsers are deterministic; decoded pointers alias the input.
 *   3. A named header lookup returns NULL or a pointer inside the header
 *      block it was given.
 *   4. Builder output round-trips through the matching parser and builders
 *      reject buffers smaller than the encoded message.
 */
#include <stdlib.h>
#include <string.h>
#include "w5500_http.h"

#define FUZZ_HTTP_MAX 2048u

static void make_str(uint8_t *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t i;
    if (n > cap - 1u) {
        n = cap - 1u;
    }
    for (i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
    dst[n] = 0u;
}

/* Builder inputs must be single tokens: whitespace would split the request
   line and make a well-formed builder output fail its own parser. */
static void make_token(uint8_t *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t i;

    make_str(dst, cap, src, n);
    for (i = 0u; dst[i] != 0u; i++) {
        if (dst[i] == ' ' || dst[i] == '\r' || dst[i] == '\n' ||
            dst[i] == '\t') {
            dst[i] = 'x';
        }
    }
}

static int in_buf(const uint8_t *p, const uint8_t *buf, size_t len)
{
    return p == NULL || (p >= buf && p < buf + len);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t buf[FUZZ_HTTP_MAX];
    static uint8_t s1[64];
    static uint8_t s2[64];
    static uint8_t out[FUZZ_HTTP_MAX];
    w5500_http_response_t rsp;
    w5500_http_response_t rsp2;
    w5500_http_request_t req;
    w5500_http_request_t req2;
    uint16_t vlen;
    const char *v;
    uint16_t n;
    size_t n1;
    size_t n2;
    int rc;

    if (size == 0u || size > FUZZ_HTTP_MAX || size > 0xFFFFu) {
        return 0;
    }
    memcpy(buf, data, size);

    /* Response parser: deterministic, pointers inside the input. */
    rc = w5500_http_parse_response(buf, (uint16_t)size, &rsp);
    if (rc != 0 && rc != -1) {
        abort();
    }
    if (w5500_http_parse_response(buf, (uint16_t)size, &rsp2) != rc) {
        abort();
    }
    if (rc == 0) {
        if (rsp.status == 0u || rsp.reason == NULL || rsp.headers == NULL ||
            rsp.body == NULL) {
            abort();
        }
        if (!in_buf((const uint8_t *)rsp.reason, buf, size) ||
            !in_buf(rsp.headers, buf, size) || !in_buf(rsp.body, buf, size)) {
            abort();
        }
        if (rsp.headers_len > size || rsp.body_len > size) {
            abort();
        }
        if (rsp2.status != rsp.status || rsp2.body_len != rsp.body_len ||
            rsp2.reason_len != rsp.reason_len ||
            rsp2.headers_len != rsp.headers_len) {
            abort();
        }
    }
    if (w5500_http_parse_response(buf, (uint16_t)size, NULL) != -1) {
        abort();
    }

    /* Header lookup with a derived name. */
    n1 = size % (sizeof(s1) - 1u);
    make_str(s1, sizeof(s1), data, n1);
    v = w5500_http_header_value(buf, (uint16_t)size, (const char *)s1, &vlen);
    if (v != NULL && !in_buf((const uint8_t *)v, buf, size)) {
        abort();
    }
    if (w5500_http_header_value(buf, (uint16_t)size, (const char *)s1,
                                NULL) != v) {
        abort();
    }
    if (w5500_http_header_value(NULL, 0u, (const char *)s1, &vlen) != NULL) {
        abort();
    }

    /* Request-line parser: deterministic, pointers inside the input. */
    rc = w5500_http_parse_request_line(buf, (uint16_t)size, &req);
    if (rc != 0 && rc != -1) {
        abort();
    }
    if (w5500_http_parse_request_line(buf, (uint16_t)size, &req2) != rc) {
        abort();
    }
    if (rc == 0) {
        if (req.method == NULL || req.path == NULL ||
            !in_buf((const uint8_t *)req.method, buf, size) ||
            !in_buf((const uint8_t *)req.path, buf, size)) {
            abort();
        }
        if (req2.method_len != req.method_len ||
            req2.path_len != req.path_len) {
            abort();
        }
    }
    if (w5500_http_parse_request_line(buf, (uint16_t)size, NULL) != -1) {
        abort();
    }

    /* Builders: derived strings, short-buffer rejection, round trips. */
    n1 = size % (sizeof(s1) - 1u);
    n2 = (size - n1) % (sizeof(s2) - 1u);
    make_token(s1, sizeof(s1), data, n1);
    make_token(s2, sizeof(s2), data + n1, n2);
    if (s1[0] == 0u) {
        s1[0] = 'x'; /* path must be a non-empty token */
    }
    if (w5500_http_build_get(out, 1u, (const char *)s1,
                             (const char *)s2) != 0u) {
        abort();
    }
    n = w5500_http_build_get(out, sizeof(out), (const char *)s1,
                             (const char *)s2);
    if (n > 0u) {
        if ((size_t)n > sizeof(out) ||
            w5500_http_parse_request_line(out, n, &req) != 0 ||
            req.method_len != 3u) {
            abort();
        }
    }

    if (w5500_http_build_response(out, 1u, 200u, (const char *)s1,
                                  s2, (uint16_t)n2) != 0u) {
        abort();
    }
    n = w5500_http_build_response(out, sizeof(out), 200u, (const char *)s1,
                                  s2, (uint16_t)n2);
    if (n > 0u) {
        if ((size_t)n > sizeof(out) ||
            w5500_http_parse_response(out, n, &rsp) != 0 || rsp.status != 200u) {
            abort();
        }
    }
    return 0;
}
