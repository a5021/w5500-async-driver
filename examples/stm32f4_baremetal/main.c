/**
 * @file main.c
 * @brief Bare-metal STM32F4 demo: DHCP-free static config, TCP client.
 *
 * Not built by the host Makefile — integrate into your firmware project
 * (CMSIS device headers + startup file required). Wiring defaults:
 * SPI1: PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI (override via -D macros).
 */
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_port_stm32.h"

static w5500_handle_t g_nic;
static w5500_socket_t g_sock;

static const uint8_t g_ip[4]     = {192, 168, 1, 42};
static const uint8_t g_mask[4]   = {255, 255, 255, 0};
static const uint8_t g_gw[4]     = {192, 168, 1, 1};
static const uint8_t g_mac[6]    = {0x02, 0x57, 0x45, 0x35, 0x30, 0x30};
static const uint8_t g_srv_ip[4] = {192, 168, 1, 10};
static const uint16_t g_srv_port = 5000u;

static void on_socket_event(w5500_socket_t sock, w5500_event_t event,
                            w5500_result_t status, void *user_arg)
{
    uint8_t buf[128];
    uint16_t n = 0u;

    (void)user_arg;

    switch (event) {
    case W5500_EVENT_CONNECTED:
        (void)w5500_socket_send(sock, (const uint8_t *)"hello\n", 6u, &n);
        break;

    case W5500_EVENT_DATA_RECEIVED:
        /* Zero-copy read straight from the chip into the stack buffer. */
        while (w5500_socket_recv(sock, buf, sizeof(buf), &n) == W5500_OK &&
               n > 0u) {
            /* TODO: process n bytes from buf */
        }
        break;

    case W5500_EVENT_SEND_DONE:
        break;

    case W5500_EVENT_DISCONNECTED:
    case W5500_EVENT_ERROR:
        /* Handle is released by the driver; reopen if needed. */
        (void)status;
        g_sock = NULL;
        (void)w5500_socket_open(g_nic, W5500_PROTO_TCP, 5000u,
                                on_socket_event, NULL, &g_sock);
        if (g_sock != NULL) {
            (void)w5500_socket_connect(g_sock, g_srv_ip, g_srv_port);
        }
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
    w5500_port_stm32_fill_plat(&plat); /* lock/unlock stay NULL (bare metal) */

    if (w5500_init(&g_nic, &plat) != W5500_OK) {
        for (;;) { /* out of memory */ }
    }

    if (w5500_check(g_nic) != W5500_OK) {
        /* VERSIONR != 0x04: wiring/SPI problem */
        for (;;) { }
    }

    (void)w5500_set_mac(g_nic, g_mac);
    (void)w5500_set_ip(g_nic, g_ip);
    (void)w5500_set_subnet_mask(g_nic, g_mask);
    (void)w5500_set_gateway(g_nic, g_gw);

    if (w5500_socket_open(g_nic, W5500_PROTO_TCP, 5000u, on_socket_event,
                          NULL, &g_sock) == W5500_OK) {
        (void)w5500_socket_connect(g_sock, g_srv_ip, g_srv_port);
    }

    for (;;) {
        (void)w5500_poll(g_nic);
        w5500_port_stm32_delay_ms(10u);
    }
}
