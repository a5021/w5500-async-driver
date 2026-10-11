/**
 * @file main.c
 * @brief FreeRTOS + STM32F4 demo: network task with mutex-protected SPI.
 *
 * Not built by the host Makefile — integrate into your firmware project.
 * Requires FreeRTOS and the CMSIS device headers.
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_port_stm32.h"
#include "w5500_port_rtos.h"

static w5500_handle_t g_nic;
static w5500_socket_t g_listen_sock;

static const uint8_t g_ip[4]   = {192, 168, 1, 42};
static const uint8_t g_mask[4] = {255, 255, 255, 0};
static const uint8_t g_gw[4]   = {192, 168, 1, 1};
static const uint8_t g_mac[6]  = {0x02, 0x57, 0x45, 0x35, 0x30, 0x30};

static void on_client_event(w5500_socket_t sock, w5500_event_t event,
                            w5500_result_t status, void *user_arg)
{
    uint8_t buf[256];
    uint16_t n = 0u;

    (void)user_arg;

    switch (event) {
    case W5500_EVENT_CONNECTED: {
        static const uint8_t greet[] = "welcome\r\n";
        (void)w5500_socket_send(sock, greet, (uint16_t)(sizeof(greet) - 1u),
                                &n);
        break;
    }
    case W5500_EVENT_DATA_RECEIVED:
        while (w5500_socket_recv(sock, buf, sizeof(buf), &n) == W5500_OK &&
               n > 0u) {
            /* Echo back; drop when the TX buffer is full. */
            (void)w5500_socket_send(sock, buf, n, &n);
        }
        break;
    case W5500_EVENT_DISCONNECTED:
    case W5500_EVENT_ERROR:
        /* Re-arm the server socket. */
        (void)w5500_socket_open(g_nic, W5500_PROTO_TCP, 80u, on_client_event,
                                NULL, &g_listen_sock);
        if (g_listen_sock != NULL) {
            (void)w5500_socket_listen(g_listen_sock);
        }
        break;
    default:
        break;
    }
}

/* Single "network" task owns w5500_poll(); callbacks run in this task. */
static void network_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();

    (void)arg;
    for (;;) {
        (void)w5500_poll(g_nic);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20u));
    }
}

int main(void)
{
    w5500_plat_if_t plat;

    w5500_port_stm32_init();

    if (w5500_rtos_lock_init() != W5500_OK) {
        for (;;) { }
    }

    memset(&plat, 0, sizeof(plat));
    w5500_port_stm32_fill_plat(&plat);
    w5500_rtos_bind_plat(&plat);

    if (w5500_init(&g_nic, &plat) != W5500_OK ||
        w5500_check(g_nic) != W5500_OK) {
        for (;;) { }
    }

    (void)w5500_set_mac(g_nic, g_mac);
    (void)w5500_set_ip(g_nic, g_ip);
    (void)w5500_set_subnet_mask(g_nic, g_mask);
    (void)w5500_set_gateway(g_nic, g_gw);

    if (w5500_socket_open(g_nic, W5500_PROTO_TCP, 80u, on_client_event, NULL,
                          &g_listen_sock) == W5500_OK) {
        (void)w5500_socket_listen(g_listen_sock);
    }

    xTaskCreate(network_task, "w5500", 1024u, NULL, tskIDLE_PRIORITY + 2u,
                NULL);
    vTaskStartScheduler();

    for (;;) { }
}
