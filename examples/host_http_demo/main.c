/**
 * @file main.c
 * @brief Host-runnable HTTP demo: codec exercised against the W5500 emulator.
 *
 * Builds and runs via `make examples`. Exit code 0 means every check held.
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "mock_w5500.h"
#include "w5500_http.h"

static int g_failures;

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

static unsigned g_events;
static uint8_t g_rx[512];
static uint16_t g_rx_len;

/* The canned HTTP response the emulated server "sends". */
static const uint8_t g_reply[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 5\r\n"
    "\r\n"
    "hello";

static void on_event(w5500_socket_t sock, w5500_event_t event,
                     w5500_result_t status, void *arg)
{
    uint8_t chunk[64];
    uint16_t n;

    (void)status;
    (void)arg;
    g_events++;
    if (event == W5500_EVENT_CONNECTED) {
        static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
        (void)w5500_socket_send(sock, (const uint8_t *)req,
                                (uint16_t)(sizeof(req) - 1u), &n);
    } else if (event == W5500_EVENT_DATA_RECEIVED) {
        while (g_rx_len < sizeof(g_rx)) {
            uint16_t room = (uint16_t)(sizeof(g_rx) - g_rx_len);
            if (room > sizeof(chunk)) {
                room = sizeof(chunk);
            }
            n = 0u;
            if (w5500_socket_recv(sock, g_rx + g_rx_len, room, &n) !=
                W5500_OK || n == 0u) {
                break;
            }
            g_rx_len = (uint16_t)(g_rx_len + n);
        }
    }
}

/* ------------------------------------------------------------------ */

static void test_client(void)
{
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;
    w5500_socket_t sock = NULL;
    uint8_t buf[128];
    w5500_http_response_t r;
    unsigned i;

    mock_w5500_reset();
    mock_w5500_plat(&plat);
    CHECK_EQ(w5500_init(&h, &plat), W5500_OK);
    CHECK_EQ(w5500_set_timeout(h, 50u), W5500_OK);

    g_events = 0u;
    g_rx_len = 0u;
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 40000u, on_event, NULL,
                               &sock), W5500_OK);
    CHECK_EQ(w5500_socket_connect(sock, (const uint8_t[]){10, 0, 0, 5}, 80u),
             W5500_OK);
    for (i = 0; i < 3u; i++) {
        (void)w5500_poll(h);
    }
    CHECK(g_events != 0u);
    CHECK_EQ(g_mock.last_sent_len[0], 37u); /* GET request length */
    {
        uint16_t n = w5500_http_build_get(buf, sizeof(buf), "/", "example.com");
        CHECK_EQ(n, g_mock.last_sent_len[0]);
        CHECK_EQ(memcmp(buf, g_mock.last_sent[0], n), 0);
    }

    /* Inject the canned reply; TCP mode has no UDP header. */
    mock_w5500_inject_rx(0, g_reply, (uint16_t)(sizeof(g_reply) - 1u));
    for (i = 0; i < 3u; i++) {
        (void)w5500_poll(h);
    }
    CHECK_EQ(w5500_http_parse_response(g_rx, g_rx_len, &r), 0);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.reason_len, 2);
    CHECK_EQ(memcmp(r.reason, "OK", 2), 0);
    CHECK_EQ(r.body_len, 5);
    CHECK_EQ(memcmp(r.body, "hello", 5), 0);

    CHECK_EQ(w5500_socket_close(sock), W5500_OK);
    w5500_deinit(h);
}

static void test_server(void)
{
    static const uint8_t req[] =
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    uint8_t out[256];
    w5500_http_request_t r;
    uint16_t n;
    static const uint8_t body[] = "hi";

    CHECK_EQ(w5500_http_parse_request_line(req, sizeof(req) - 1u, &r), 0);
    CHECK_EQ(r.method_len, 3);
    CHECK_EQ(memcmp(r.method, "GET", 3), 0);
    CHECK_EQ(r.path_len, 1);
    CHECK_EQ(r.path[0], '/');

    n = w5500_http_build_response(out, sizeof(out), 200, "text/plain", body, 2u);
    CHECK(n != 0u);
    {
        w5500_http_response_t parsed;
        uint8_t full[256];
        /* Wrap the generated response plus its body into a buffer the
           parser can consume (build_response already includes the body). */
        memcpy(full, out, n);
        CHECK_EQ(w5500_http_parse_response(full, n, &parsed), 0);
        CHECK_EQ(parsed.status, 200);
        CHECK_EQ(parsed.body_len, 2);
        CHECK_EQ(memcmp(parsed.body, "hi", 2), 0);
    }
}

int main(void)
{
    test_client();
    test_server();

    if (g_failures == 0) {
        printf("OK: host http demo passed\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
