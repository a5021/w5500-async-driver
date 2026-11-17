/**
 * @file main.c
 * @brief Bare-metal STM32F4 demo: minimal HTTP server on port 80.
 *
 * One listening socket accepts a client, the request line is parsed with the
 * example HTTP codec, and a short response is sent. After SEND_DONE the
 * connection is closed and the listener is re-armed. No RTOS needed.
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_port_stm32.h"
#include "w5500_http.h"

#define HTTP_BUF_BYTES 1024u

static w5500_handle_t g_nic;
static w5500_socket_t g_listen;
static uint8_t g_buf[HTTP_BUF_BYTES];
static uint16_t g_rx_len;
static uint8_t g_reply_pending;

static const uint8_t g_ip[4]   = {192, 168, 1, 42};
static const uint8_t g_mask[4] = {255, 255, 255, 0};
static const uint8_t g_gw[4]   = {192, 168, 1, 1};
static const uint8_t g_mac[6]  = {0x02, 0x57, 0x45, 0x35, 0x30, 0x32};

static const uint8_t g_body[] =
    "<html><body><h1>W5500</h1><p>hello from stm32</p></body></html>";

static void arm_listener(void);
static void on_client_event(w5500_socket_t sock, w5500_event_t event,
                            w5500_result_t status, void *arg);

static void arm_listener(void)
{
    if (w5500_socket_open(g_nic, W5500_PROTO_TCP, 80u, on_client_event,
                          NULL, &g_listen) == W5500_OK) {
        (void)w5500_socket_listen(g_listen);
    } else {
        g_listen = NULL;
    }
}

static void send_reply(w5500_socket_t sock, uint16_t status,
                       const char *content_type,
                       const uint8_t *body, uint16_t body_len)
{
    uint16_t n = w5500_http_build_response(g_buf, sizeof(g_buf), status,
                                     content_type, body, body_len);
    if (n == 0u) {
        (void)w5500_socket_close(sock);
        g_reply_pending = 0u;
        arm_listener();
        return;
    }
    (void)w5500_socket_send(sock, g_buf, n, &n);
    g_reply_pending = 1u;
}

static void on_client_event(w5500_socket_t sock, w5500_event_t event,
                            w5500_result_t status, void *arg)
{
    (void)arg;
    switch (event) {
    case W5500_EVENT_CONNECTED:
        /* The listening socket turned established: this is the client. */
        g_listen = NULL;
        g_rx_len = 0u;
        g_reply_pending = 0u;
        break;
    case W5500_EVENT_DATA_RECEIVED: {
        uint8_t chunk[128];
        uint16_t n;
        w5500_http_request_t req;

        while (g_rx_len < sizeof(g_buf)) {
            uint16_t room = (uint16_t)(sizeof(g_buf) - g_rx_len);
            if (room > sizeof(chunk)) {
                room = sizeof(chunk);
            }
            n = 0u;
            if (w5500_socket_recv(sock, g_buf + g_rx_len, room, &n) !=
                W5500_OK || n == 0u) {
                break;
            }
            g_rx_len = (uint16_t)(g_rx_len + n);
        }
        if (w5500_http_parse_request_line(g_buf, g_rx_len, &req) != 0) {
            break; /* wait for more bytes of the request line */
        }
        if (req.method_len == 3u && memcmp(req.method, "GET", 3u) == 0 &&
            req.path_len == 1u && req.path[0] == '/') {
            send_reply(sock, 200, "text/html", g_body,
                       (uint16_t)(sizeof(g_body) - 1u));
        } else {
            static const uint8_t not_found[] = "Not Found";
            send_reply(sock, 404, "text/plain", not_found,
                       (uint16_t)(sizeof(not_found) - 1u));
        }
        break;
    }
    case W5500_EVENT_SEND_DONE:
        if (g_reply_pending != 0u) {
            g_reply_pending = 0u;
            (void)w5500_socket_close(sock);
            arm_listener();
        }
        break;
    case W5500_EVENT_DISCONNECTED:
    case W5500_EVENT_ERROR:
        (void)status;
        g_reply_pending = 0u;
        if (sock == g_listen) {
            g_listen = NULL;
        }
        arm_listener();
        break;
    default:
        break;
    }
}

int main(void)
{
    w5500_plat_if_t plat;

    w5500_port_stm32_init();
    memset(&plat, 0, sizeof(plat));
    w5500_port_stm32_fill_plat(&plat);
    if (w5500_init(&g_nic, &plat) != W5500_OK ||
        w5500_check(g_nic) != W5500_OK) {
        for (;;) { }
    }
    (void)w5500_set_mac(g_nic, g_mac);
    (void)w5500_set_ip(g_nic, g_ip);
    (void)w5500_set_subnet_mask(g_nic, g_mask);
    (void)w5500_set_gateway(g_nic, g_gw);
    arm_listener();

    for (;;) {
        (void)w5500_poll(g_nic);
        w5500_port_stm32_delay_ms(10u);
    }
}
