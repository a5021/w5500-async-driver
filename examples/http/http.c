/**
 * @file http.c
 * @brief Minimal HTTP/1.x codec used by the examples (see http.h).
 */
#include <string.h>

#include "http.h"

#define HTTP_CRLF_CRLF "\r\n\r\n"

static const char *http_reason(uint16_t status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:  return "Status";
    }
}

const char *http_header_value(const uint8_t *headers, uint16_t len,
                              const char *name, uint16_t *value_len)
{
    size_t nlen = strlen(name);
    uint16_t off = 0u;

    while ((uint32_t)off < (uint32_t)len) {
        uint16_t line_end = off;
        uint16_t colon = 0u;
        uint8_t found = 0u;
        uint8_t has_crlf = 0u;

        while ((uint32_t)line_end + 1u < (uint32_t)len) {
            if (headers[line_end] == '\r' &&
                headers[line_end + 1u] == '\n') {
                has_crlf = 1u;
                break;
            }
            line_end++;
        }
        if (has_crlf == 0u) {
            line_end = len; /* last header line has no trailing CRLF here */
        }

        {
            uint16_t i;
            for (i = off; i < line_end; i++) {
                if (headers[i] == ':') {
                    colon = i;
                    found = 1u;
                    break;
                }
            }
        }
        if (found != 0u && (uint16_t)(colon - off) == (uint16_t)nlen &&
            memcmp(headers + off, name, nlen) == 0) {
            uint16_t vstart = (uint16_t)(colon + 1u);
            while (vstart < line_end && headers[vstart] == ' ') {
                vstart++;
            }
            if (value_len != NULL) {
                *value_len = (uint16_t)(line_end - vstart);
            }
            return (const char *)(headers + vstart);
        }
        if (has_crlf == 0u) {
            break; /* last header consumed */
        }
        off = (uint16_t)(line_end + 2u);
    }
    return NULL;
}

int http_parse_response(const uint8_t *buf, uint16_t len,
                        http_response_t *out)
{
    uint16_t i;
    uint16_t sep = 0u;
    uint16_t status = 0u;
    uint8_t sep_found = 0u;
    uint16_t hdr_start;
    const char *cl;
    uint16_t cl_len = 0u;
    uint32_t content_len = 0u;

    if (buf == NULL || out == NULL || len < 16u) {
        return -1;
    }
    /* Find the header/body separator; a partial response returns -1. */
    for (i = 0; (uint32_t)i + 4u <= (uint32_t)len; i++) {
        if (memcmp(buf + i, HTTP_CRLF_CRLF, 4u) == 0) {
            sep = i;
            sep_found = 1u;
            break;
        }
    }
    if (sep_found == 0u) {
        return -1;
    }
    if (memcmp(buf, "HTTP/1.", 7u) != 0) {
        return -1;
    }
    {
        uint16_t p = 9u; /* first status digit (after "HTTP/1.x ") */
        if (p + 4u > sep) {
            return -1;
        }
        for (; p < sep && buf[p] >= '0' && buf[p] <= '9'; p++) {
            status = (uint16_t)(status * 10u + (uint16_t)(buf[p] - '0'));
        }
        if (p >= sep || status == 0u || buf[p] != ' ') {
            return -1;
        }
        out->reason = (const char *)(buf + p + 1u); /* skip the space */
        {
            uint16_t r = (uint16_t)(p + 1u);
            while (r < sep && buf[r] != '\r') {
                r++;
            }
            out->reason_len = (uint8_t)(r - p - 1u);
        }
    }

    hdr_start = (uint16_t)(sep + 4u);
    out->status = status;
    out->headers = buf + 9u;
    out->headers_len = (uint16_t)(sep - 9u);

    cl = http_header_value(buf + 9u, (uint16_t)(sep - 9u), "Content-Length",
                           &cl_len);
    if (cl == NULL || cl_len == 0u) {
        content_len = 0u; /* no body */
    } else {
        uint16_t k;
        for (k = 0; k < cl_len; k++) {
            if (cl[k] < '0' || cl[k] > '9') {
                return -1;
            }
            content_len = content_len * 10u + (uint32_t)(cl[k] - '0');
            if (content_len > 0xFFFFu) {
                return -1;
            }
        }
    }

    out->body = buf + hdr_start;
    out->body_len = (uint16_t)(len - hdr_start);
    out->truncated = (uint8_t)((content_len > out->body_len) ? 1u : 0u);
    return 0;
}

uint16_t http_build_get(uint8_t *buf, uint16_t cap, const char *path,
                        const char *host)
{
    static const char p1[] = "GET ";
    static const char p2[] = " HTTP/1.0\r\nHost: ";
    static const char p3[] = "\r\n\r\n";
    size_t lp = strlen(path);
    size_t lh = strlen(host);
    size_t total = sizeof(p1) - 1u + lp + sizeof(p2) - 1u + lh +
                   sizeof(p3) - 1u;
    uint16_t off = 0u;

    if (buf == NULL || total > cap) {
        return 0u;
    }
    memcpy(buf, p1, sizeof(p1) - 1u);
    off = (uint16_t)(sizeof(p1) - 1u);
    memcpy(buf + off, path, lp);
    off = (uint16_t)(off + lp);
    memcpy(buf + off, p2, sizeof(p2) - 1u);
    off = (uint16_t)(off + sizeof(p2) - 1u);
    memcpy(buf + off, host, lh);
    off = (uint16_t)(off + lh);
    memcpy(buf + off, p3, sizeof(p3) - 1u);
    return (uint16_t)(off + sizeof(p3) - 1u);
}

int http_parse_request_line(const uint8_t *buf, uint16_t len,
                            http_request_t *out)
{
    uint16_t method_end = 0u;
    uint16_t path_start;
    uint16_t path_end;

    if (buf == NULL || out == NULL || len < 12u) {
        return -1;
    }
    while (method_end < len && buf[method_end] != ' ') {
        method_end++;
    }
    if (method_end == 0u || method_end >= len) {
        return -1;
    }
    path_start = (uint16_t)(method_end + 1u);
    path_end = path_start;
    while (path_end < len && buf[path_end] != ' ') {
        path_end++;
    }
    if (path_end == path_start || path_end >= len) {
        return -1;
    }
    if ((uint32_t)path_end + 8u > (uint32_t)len) {
        return -1;
    }
    if (memcmp(buf + path_end, " HTTP/1.", 8u) != 0) {
        return -1;
    }
    out->method = (const char *)buf;
    out->method_len = (uint8_t)method_end;
    out->path = (const char *)(buf + path_start);
    out->path_len = (uint8_t)(path_end - path_start);
    return 0;
}

uint16_t http_build_response(uint8_t *buf, uint16_t cap, uint16_t status,
                             const char *content_type,
                             const uint8_t *body, uint16_t body_len)
{
    static const char p1[] = "HTTP/1.0 ";
    static const char p2[] = "\r\nContent-Type: ";
    static const char p3[] = "\r\nContent-Length: ";
    static const char p4[] = "\r\n\r\n";
    const char *reason = http_reason(status);
    size_t reason_len = strlen(reason);
    size_t ct_len = strlen(content_type);
    size_t num_len = 1u;
    uint32_t v = body_len;
    size_t total;
    uint16_t off = 0u;

    while (v >= 10u) {
        num_len++;
        v /= 10u;
    }
    total = sizeof(p1) - 1u + 3u + 1u + reason_len + sizeof(p2) - 1u +
            ct_len + sizeof(p3) - 1u + num_len + sizeof(p4) - 1u + body_len;
    if (buf == NULL || total > cap) {
        return 0u;
    }

    memcpy(buf, p1, sizeof(p1) - 1u);
    off = (uint16_t)(sizeof(p1) - 1u);
    buf[off++] = (uint8_t)('0' + (status / 100u) % 10u);
    buf[off++] = (uint8_t)('0' + (status / 10u) % 10u);
    buf[off++] = (uint8_t)('0' + status % 10u);
    buf[off++] = ' ';
    memcpy(buf + off, reason, reason_len);
    off = (uint16_t)(off + reason_len);
    memcpy(buf + off, p2, sizeof(p2) - 1u);
    off = (uint16_t)(off + sizeof(p2) - 1u);
    memcpy(buf + off, content_type, ct_len);
    off = (uint16_t)(off + ct_len);
    memcpy(buf + off, p3, sizeof(p3) - 1u);
    off = (uint16_t)(off + sizeof(p3) - 1u);

    {
        char digits[5];
        uint16_t n = 0u;
        uint32_t t = body_len;
        if (t == 0u) {
            digits[n++] = '0';
        } else {
            while (t != 0u) {
                digits[n++] = (char)('0' + t % 10u);
                t /= 10u;
            }
        }
        while (n != 0u) {
            buf[off++] = (uint8_t)digits[--n];
        }
    }
    memcpy(buf + off, p4, sizeof(p4) - 1u);
    off = (uint16_t)(off + sizeof(p4) - 1u);
    if (body_len != 0u) {
        memcpy(buf + off, body, body_len);
        off = (uint16_t)(off + body_len);
    }
    return off;
}
