/**
 * @file w5500_http.h
 * @brief Minimal HTTP/1.0-1.1 message codec (no heap, no MCU/W5500 deps).
 *
 * Scope (deliberately small):
 *   - responses with a status line, headers and Content-Length body
 *   - GET requests and a server-side request-line parser
 *   - no TLS, no chunked transfer, no keep-alive (one request per socket)
 *   - if Content-Length exceeds the caller's buffer, the body is truncated
 *     and the `truncated` flag is set
 */
#ifndef W5500_HTTP_H
#define W5500_HTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Result of parsing a complete HTTP response. */
typedef struct {
    uint16_t       status;        /**< e.g. 200 or 404 */
    const char    *reason;        /**< pointer into the input buffer */
    uint8_t        reason_len;    /**< length of the reason phrase */
    const uint8_t *headers;       /**< status line + headers block */
    uint16_t       headers_len;   /**< bytes of headers including CRLFs */
    const uint8_t *body;          /**< pointer into the input buffer */
    uint16_t       body_len;      /**< bytes actually available */
    uint8_t        truncated;     /**< Content-Length exceeded the input */
} w5500_http_response_t;

/** Result of parsing an HTTP request line. */
typedef struct {
    const char *method;    /**< pointer into the input buffer */
    uint8_t     method_len;
    const char *path;      /**< pointer into the input buffer */
    uint8_t     path_len;
} w5500_http_request_t;

/**
 * @brief Find a named header in a header block (case-sensitive).
 * @return Value pointer and length, or NULL when absent.
 */
const char *w5500_http_header_value(const uint8_t *headers, uint16_t len,
                              const char *name, uint16_t *value_len);

/**
 * @brief Parse one complete HTTP response from @p buf.
 * @return 0 on success, -1 when the data is incomplete or malformed.
 */
int w5500_http_parse_response(const uint8_t *buf, uint16_t len,
                        w5500_http_response_t *out);

/** Build "GET <path> HTTP/1.0\r\nHost: <host>\r\n\r\n". */
uint16_t w5500_http_build_get(uint8_t *buf, uint16_t cap, const char *path,
                        const char *host);

/**
 * @brief Parse the first line of a request ("GET /path HTTP/1.x").
 * @return 0 on success, -1 on malformed input.
 */
int w5500_http_parse_request_line(const uint8_t *buf, uint16_t len,
                            w5500_http_request_t *out);

/**
 * @brief Build "HTTP/1.0 <status> <reason>\r\nContent-Type: ...\r\n\r\n<body>".
 */
uint16_t w5500_http_build_response(uint8_t *buf, uint16_t cap, uint16_t status,
                             const char *content_type,
                             const uint8_t *body, uint16_t body_len);

#ifdef __cplusplus
}
#endif

#endif /* W5500_HTTP_H */
