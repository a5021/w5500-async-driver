/**
 * @file main.c
 * @brief Bare-metal STM32F4 benchmark for the W5500 driver.
 *
 * Prints cycle counts for typical operations so optimisation work (DMA
 * bursts, smaller poll traffic) can be justified by measurements instead of
 * guesses. Requires a W5500 on SPI1 with a live link; open a serial console
 * for the output (`printf` must be retargeted by the firmware project).
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_port_stm32.h"

static w5500_handle_t g_nic;
static uint8_t g_payload[1024];

static const uint8_t g_mac[6]  = {0x02, 0x57, 0x45, 0x35, 0x30, 0x30};
static const uint8_t g_ip[4]   = {192, 168, 1, 42};
static const uint8_t g_mask[4] = {255, 255, 255, 0};
static const uint8_t g_gw[4]   = {192, 168, 1, 1};
static const uint8_t g_srv[4]  = {192, 168, 1, 10};

static void on_event(w5500_socket_t sock, w5500_event_t event,
                     w5500_result_t status, void *arg)
{
    (void)sock;
    (void)event;
    (void)status;
    (void)arg;
}

static void report(const char *what, uint32_t cycles, uint32_t iterations)
{
    printf("%-28s %8lu cycles total, %5lu per call\n", what,
           (unsigned long)cycles,
           (unsigned long)((iterations != 0u) ? cycles / iterations : 0u));
}

int main(void)
{
    w5500_plat_if_t plat;
    w5500_socket_t sock = NULL;
    uint32_t t0;
    uint32_t t1;
    uint16_t sent = 0u;
    unsigned i;

    w5500_port_stm32_init();
    memset(&plat, 0, sizeof(plat));
    w5500_port_stm32_fill_plat(&plat);

    if (w5500_init(&g_nic, &plat) != W5500_OK) {
        for (;;) { }
    }

    t0 = w5500_port_stm32_cycles();
    (void)w5500_check(g_nic);
    t1 = w5500_port_stm32_cycles();
    report("w5500_check", (uint32_t)(t1 - t0), 1u);

    (void)w5500_set_mac(g_nic, g_mac);
    (void)w5500_set_ip(g_nic, g_ip);
    (void)w5500_set_subnet_mask(g_nic, g_mask);
    (void)w5500_set_gateway(g_nic, g_gw);

    if (w5500_socket_open(g_nic, W5500_PROTO_TCP, 5000u, on_event, NULL,
                          &sock) != W5500_OK) {
        printf("open failed\n");
        return 1;
    }
    (void)w5500_socket_connect(sock, g_srv, 80u);
    for (i = 0; i < 1000u; i++) {
        (void)w5500_poll(g_nic);
    }

    memset(g_payload, 0xA5, sizeof(g_payload));
    t0 = w5500_port_stm32_cycles();
    (void)w5500_socket_send(sock, g_payload, sizeof(g_payload), &sent);
    t1 = w5500_port_stm32_cycles();
    report("send 1 KiB (staging)", (uint32_t)(t1 - t0), 1u);
    printf("%-28s %u bytes accepted\n", "send accepted", sent);

    t0 = w5500_port_stm32_cycles();
    for (i = 0; i < 1000u; i++) {
        (void)w5500_poll(g_nic);
    }
    t1 = w5500_port_stm32_cycles();
    report("w5500_poll (idle, 1 socket)", (uint32_t)(t1 - t0), 1000u);

    (void)w5500_socket_close(sock);
    w5500_deinit(g_nic);
    printf("done\n");
    return 0;
}
