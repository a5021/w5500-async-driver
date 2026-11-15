/**
 * @file main.c
 * @brief Bare-metal STM32F4 demo: DHCP lease, DNS lookup, then TCP client.
 *
 * Flow: chip probe -> DHCP -> on BOUND resolve DEMO_HOST against the DNS
 * server handed out by DHCP -> on success open a TCP socket and send a
 * short HTTP request. A link flap restarts the whole chain.
 *
 * Not built by the host Makefile: integrate into your firmware project
 * (CMSIS device headers + startup file required). Wiring defaults:
 * SPI1: PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI (override via -D macros).
 */
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_dhcp.h"
#include "w5500_dns.h"
#include "w5500_port_stm32.h"

/* Everything the protocols need lives in static storage: no heap. */
#define DHCP_MEM_BYTES 2048u
#define DNS_MEM_BYTES  2048u

static uint8_t g_dhcp_mem[DHCP_MEM_BYTES];
static uint8_t g_dns_mem[DNS_MEM_BYTES];

static w5500_handle_t g_nic;
static w5500_dhcp_t  *g_dhcp;
static w5500_dns_t   *g_dns;
static w5500_socket_t g_sock;

static uint8_t g_dns_server[4];
static uint8_t g_srv_ip[4];
static uint8_t g_dns_active;
static uint32_t g_now_ms;

static const uint8_t  g_mac[6]  = {0x02, 0x57, 0x45, 0x35, 0x30, 0x30};
static const char     g_host[]  = "example.com";
static const uint16_t g_port    = 80u;

static void app_start_request(void);

/* ------------------------------------------------------------------ */
/* DHCP                                                                */
/* ------------------------------------------------------------------ */

static void on_dhcp(w5500_dhcp_t *dhcp, w5500_dhcp_event_t event, void *arg)
{
    (void)arg;
    if (event == W5500_DHCP_EVENT_FAILED) {
        /* No server answered; keep trying (cable may be unplugged). */
        (void)w5500_dhcp_start(dhcp, g_now_ms);
        return;
    }

    /* BOUND/RENEWED: the lease is already applied to the chip. */
    (void)w5500_dhcp_get_config(dhcp, NULL, NULL, NULL, g_dns_server);

    (void)w5500_dns_set_server(g_dns, g_dns_server);
    if (w5500_dns_start(g_dns, g_host, g_now_ms) == W5500_OK) {
        g_dns_active = 1u;
    }
}

/* ------------------------------------------------------------------ */
/* DNS                                                                 */
/* ------------------------------------------------------------------ */

static void on_dns(w5500_dns_t *dns, const uint8_t ip[4],
                   w5500_result_t status, void *arg)
{
    (void)dns;
    (void)arg;
    g_dns_active = 0u;
    if (status != W5500_OK) {
        /* Retry on the next DHCP renewal / link event. */
        return;
    }
    memcpy(g_srv_ip, ip, 4u);
    app_start_request();
}

/* ------------------------------------------------------------------ */
/* TCP application                                                     */
/* ------------------------------------------------------------------ */

static void on_socket_event(w5500_socket_t sock, w5500_event_t event,
                            w5500_result_t status, void *user_arg)
{
    static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    uint8_t buf[128];
    uint16_t n = 0u;

    (void)user_arg;

    switch (event) {
    case W5500_EVENT_CONNECTED:
        (void)w5500_socket_send(sock, (const uint8_t *)req,
                                (uint16_t)(sizeof(req) - 1u), &n);
        break;

    case W5500_EVENT_DATA_RECEIVED:
        while (w5500_socket_recv(sock, buf, sizeof(buf), &n) == W5500_OK &&
               n > 0u) {
            /* TODO: feed the HTTP response to the application */
        }
        break;

    case W5500_EVENT_SEND_DONE:
        break;

    case W5500_EVENT_DISCONNECTED:
    case W5500_EVENT_ERROR:
        (void)status;
        g_sock = NULL; /* the driver released the handle */
        break;

    default:
        break;
    }
}

static void app_start_request(void)
{
    if (g_sock != NULL) {
        return; /* a connection attempt is already in flight */
    }
    if (w5500_socket_open(g_nic, W5500_PROTO_TCP, 40000u, on_socket_event,
                          NULL, &g_sock) == W5500_OK) {
        (void)w5500_socket_connect(g_sock, g_srv_ip, g_port);
    } else {
        g_sock = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Link                                                                */
/* ------------------------------------------------------------------ */

static void on_link(w5500_handle_t nic, const w5500_link_info_t *info,
                    void *arg)
{
    (void)nic;
    (void)arg;
    if (info->up == 0u) {
        /* Sockets will report DISCONNECTED/ERROR on their own; just note
           that the network has to be re-established. */
        return;
    }
    if (g_dhcp != NULL) {
        (void)w5500_dhcp_start(g_dhcp, g_now_ms); /* re-acquire the lease */
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
    w5500_port_stm32_fill_plat(&plat); /* lock/unlock stay NULL (bare metal) */

    if (w5500_init(&g_nic, &plat) != W5500_OK) {
        for (;;) { /* out of memory */ }
    }
    if (w5500_check(g_nic) != W5500_OK) {
        for (;;) { /* VERSIONR/RTR mismatch: wiring or SPI problem */ }
    }

    (void)w5500_set_mac(g_nic, g_mac);
    (void)w5500_set_link_cb(g_nic, on_link, NULL);

    if (w5500_dhcp_init(&g_dhcp, g_dhcp_mem, sizeof(g_dhcp_mem), g_nic,
                        g_mac, on_dhcp, NULL) != W5500_OK) {
        for (;;) { }
    }
    {
        static const uint8_t no_dns[4] = {0u, 0u, 0u, 0u};
        if (w5500_dns_init(&g_dns, g_dns_mem, sizeof(g_dns_mem), g_nic,
                           no_dns, on_dns, NULL) != W5500_OK) {
            for (;;) { }
        }
    }
    if (w5500_dhcp_start(g_dhcp, g_now_ms) != W5500_OK) {
        for (;;) { }
    }

    for (;;) {
        (void)w5500_poll(g_nic);      /* socket events */
        (void)w5500_dhcp_poll(g_dhcp, g_now_ms); /* renew/retransmit */
        if (g_dns_active != 0u) {
            (void)w5500_dns_poll(g_dns, g_now_ms);
        }
        app_delay_5ms();
    }
}
