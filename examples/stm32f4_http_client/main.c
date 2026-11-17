/**
 * @file main.c
 * @brief Bare-metal STM32F4 demo: DHCP -> DNS -> TCP -> HTTP GET with a
 *        parsed response (status, headers, body).
 *
 * The socket layer drives everything through events; the response is
 * accumulated into a static buffer and parsed with the example HTTP codec.
 * Not built by the host Makefile: integrate into your firmware project.
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_dhcp.h"
#include "w5500_dns.h"
#include "w5500_port_stm32.h"
#include "w5500_http.h"

#define DHCP_MEM_BYTES 2048u
#define DNS_MEM_BYTES  2048u
#define HTTP_BUF_BYTES 2048u

static uint8_t g_dhcp_mem[DHCP_MEM_BYTES];
static uint8_t g_dns_mem[DNS_MEM_BYTES];
static uint8_t g_http_buf[HTTP_BUF_BYTES];
static w5500_handle_t g_nic;
static w5500_dhcp_t  *g_dhcp;
static w5500_dns_t   *g_dns;
static w5500_socket_t g_sock;
static uint8_t g_dns_server[4];
static uint8_t g_srv_ip[4];
static uint16_t g_http_len;
static uint8_t g_dns_active;
static uint8_t g_http_done;
static uint32_t g_now_ms;

static const uint8_t  g_mac[6] = {0x02, 0x57, 0x45, 0x35, 0x30, 0x31};
static const char     g_host[] = "example.com";
static const char     g_path[] = "/";
static const uint16_t g_port   = 80u;

static void app_start_request(void);

/* ------------------------------------------------------------------ */
/* DHCP / DNS                                                          */
/* ------------------------------------------------------------------ */

static void on_dhcp(w5500_dhcp_t *dhcp, w5500_dhcp_event_t event, void *arg)
{
    (void)arg;
    if (event == W5500_DHCP_EVENT_FAILED) {
        (void)w5500_dhcp_start(dhcp, g_now_ms);
        return;
    }
    (void)w5500_dhcp_get_config(dhcp, NULL, NULL, NULL, g_dns_server);
    (void)w5500_dns_set_server(g_dns, g_dns_server);
    if (w5500_dns_start(g_dns, g_host, g_now_ms) == W5500_OK) {
        g_dns_active = 1u;
    }
}

static void on_dns(w5500_dns_t *dns, const uint8_t ip[4],
                   w5500_result_t status, void *arg)
{
    (void)dns;
    (void)arg;
    g_dns_active = 0u;
    if (status != W5500_OK) {
        return; /* retry on next DHCP renewal / link event */
    }
    memcpy(g_srv_ip, ip, 4u);
    app_start_request();
}

/* ------------------------------------------------------------------ */
/* TCP / HTTP                                                          */
/* ------------------------------------------------------------------ */

static void http_report(void)
{
    w5500_http_response_t r;

    if (w5500_http_parse_response(g_http_buf, g_http_len, &r) != 0) {
        printf("http: incomplete response (%u bytes)\n", g_http_len);
        return;
    }
    printf("http: status %u, reason %.*s, body %u bytes%s\n",
           r.status, r.reason_len, r.reason, r.body_len,
           r.truncated ? " (truncated)" : "");
    printf("http: body: %.*s\n", r.body_len, (const char *)r.body);
}

static int http_has_header_end(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    for (i = 0u; (uint32_t)i + 4u <= (uint32_t)len; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4u) == 0) {
            return 1;
        }
    }
    return 0;
}

static void on_socket_event(w5500_socket_t sock, w5500_event_t event,
                            w5500_result_t status, void *user_arg)
{
    uint8_t chunk[256];
    uint16_t n;

    (void)user_arg;
    switch (event) {
    case W5500_EVENT_CONNECTED: {
        uint16_t len = w5500_http_build_get(g_http_buf, sizeof(g_http_buf),
                                      g_path, g_host);
        if (len == 0u) {
            (void)w5500_socket_close(sock);
            g_sock = NULL;
            break;
        }
        (void)w5500_socket_send(sock, g_http_buf, len, &n);
        break;
    }
    case W5500_EVENT_DATA_RECEIVED:
        while (g_http_len < sizeof(g_http_buf)) {
            uint16_t room = (uint16_t)(sizeof(g_http_buf) - g_http_len);
            if (room > sizeof(chunk)) {
                room = sizeof(chunk);
            }
            n = 0u;
            if (w5500_socket_recv(sock, g_http_buf + g_http_len, room, &n) !=
                W5500_OK || n == 0u) {
                break;
            }
            g_http_len = (uint16_t)(g_http_len + n);
        }
        if (g_http_len >= sizeof(g_http_buf) ||
            http_has_header_end(g_http_buf, g_http_len)) {
            http_report();
            g_http_done = 1u;
            (void)w5500_socket_close(sock);
            g_sock = NULL;
        }
        break;
    case W5500_EVENT_SEND_DONE:
        break;
    case W5500_EVENT_DISCONNECTED:
    case W5500_EVENT_ERROR:
        (void)status;
        g_sock = NULL;
        if (!g_http_done && g_http_len != 0u) {
            http_report(); /* peer closed: parse whatever arrived */
        }
        break;
    default:
        break;
    }
}

static void app_start_request(void)
{
    if (g_sock != NULL || g_http_done != 0u) {
        return;
    }
    g_http_len = 0u;
    if (w5500_socket_open(g_nic, W5500_PROTO_TCP, 40000u, on_socket_event,
                          NULL, &g_sock) == W5500_OK) {
        (void)w5500_socket_connect(g_sock, g_srv_ip, g_port);
    } else {
        g_sock = NULL;
    }
}

/* ------------------------------------------------------------------ */

static void app_delay_5ms(void)
{
    w5500_port_stm32_delay_ms(5u);
    g_now_ms += 5u;
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

    if (w5500_dhcp_init(&g_dhcp, g_dhcp_mem, sizeof(g_dhcp_mem), g_nic,
                        g_mac, on_dhcp, NULL) != W5500_OK ||
        w5500_dns_init(&g_dns, g_dns_mem, sizeof(g_dns_mem), g_nic,
                       g_dns_server, on_dns, NULL) != W5500_OK) {
        for (;;) { }
    }
    if (w5500_dhcp_start(g_dhcp, g_now_ms) != W5500_OK) {
        for (;;) { }
    }

    for (;;) {
        (void)w5500_poll(g_nic);
        (void)w5500_dhcp_poll(g_dhcp, g_now_ms);
        if (g_dns_active != 0u) {
            (void)w5500_dns_poll(g_dns, g_now_ms);
        }
        app_delay_5ms();
    }
}
