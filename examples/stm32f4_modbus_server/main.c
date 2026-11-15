/**
 * @file main.c
 * @brief Bare-metal STM32F4 demo: Modbus TCP server (FC 3/4/6/16).
 *
 * The register map is plain RAM: 32 holding registers (FC 3/6/16) and
 * 16 input registers (FC 4, refreshed from a counter once per second).
 * Link flaps restart the listeners; the driver reopens released sockets.
 *
 * Not built by the host Makefile: integrate into your firmware project
 * (CMSIS device headers + startup file required). Wiring defaults:
 * SPI1: PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI (override via -D macros).
 */
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_modbus.h"
#include "w5500_port_stm32.h"

#define MODBUS_PORT     502u
#define HOLDING_COUNT   32u
#define INPUT_COUNT     16u
#define SERVER_MEM      4096u

static uint8_t g_srv_mem[SERVER_MEM];
static w5500_modbus_server_t *g_srv;
static w5500_handle_t g_nic;
static uint32_t g_now_ms;
static uint16_t g_holding[HOLDING_COUNT];
static uint16_t g_input[INPUT_COUNT];

static const uint8_t g_mac[6] = {0x02, 0x57, 0x45, 0x35, 0x30, 0x31};
static const uint8_t g_ip[4] = {192, 168, 1, 50};
static const uint8_t g_mask[4] = {255, 255, 255, 0};
static const uint8_t g_gw[4] = {192, 168, 1, 1};

/* ------------------------------------------------------------------ */
/* Register map callbacks                                              */
/* ------------------------------------------------------------------ */

static w5500_result_t map_read(uint8_t unit, uint8_t fc, uint16_t addr,
                               uint16_t count, uint16_t *regs, void *arg)
{
    const uint16_t *src;
    uint16_t cap;

    (void)arg;
    if (unit != 1u) {
        return W5500_ERR_PARAM; /* answered as exception 02 */
    }
    if (fc == W5500_MB_FC_READ_INPUT) {
        src = g_input;
        cap = INPUT_COUNT;
    } else {
        src = g_holding;
        cap = HOLDING_COUNT;
    }
    if ((uint32_t)addr + (uint32_t)count > (uint32_t)cap) {
        return W5500_ERR_PARAM;
    }
    memcpy(regs, &src[addr], (size_t)count * sizeof(uint16_t));
    return W5500_OK;
}

static w5500_result_t map_write(uint8_t unit, uint16_t addr, uint16_t count,
                                const uint16_t *regs, void *arg)
{
    (void)arg;
    if (unit != 1u) {
        return W5500_ERR_PARAM;
    }
    if ((uint32_t)addr + (uint32_t)count > HOLDING_COUNT) {
        return W5500_ERR_PARAM;
    }
    memcpy(&g_holding[addr], regs, (size_t)count * sizeof(uint16_t));
    /* TODO: mirror the values into your process state here. */
    return W5500_OK;
}

/* ------------------------------------------------------------------ */
/* Link                                                                */
/* ------------------------------------------------------------------ */

static void on_link(w5500_handle_t nic, const w5500_link_info_t *info,
                    void *arg)
{
    (void)nic;
    (void)arg;
    if (info->up != 0u && g_srv != NULL) {
        (void)w5500_modbus_server_start(g_srv);
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
    uint32_t last_tick = 0u;

    w5500_port_stm32_init();

    memset(&plat, 0, sizeof(plat));
    w5500_port_stm32_fill_plat(&plat);

    if (w5500_init(&g_nic, &plat) != W5500_OK) {
        for (;;) { }
    }
    if (w5500_check(g_nic) != W5500_OK) {
        for (;;) { /* wiring or SPI problem */ }
    }

    (void)w5500_set_mac(g_nic, g_mac);
    (void)w5500_set_ip(g_nic, g_ip);
    (void)w5500_set_subnet_mask(g_nic, g_mask);
    (void)w5500_set_gateway(g_nic, g_gw);

    if (w5500_modbus_server_init(&g_srv, g_srv_mem, sizeof(g_srv_mem), g_nic,
                                 MODBUS_PORT, 2u, map_read, map_write,
                                 NULL) != W5500_OK) {
        for (;;) { }
    }
    if (w5500_modbus_server_start(g_srv) != W5500_OK) {
        for (;;) { }
    }
    (void)w5500_set_link_cb(g_nic, on_link, NULL);

    for (;;) {
        (void)w5500_poll(g_nic);            /* socket events */
        (void)w5500_modbus_server_poll(g_srv); /* ADUs in, replies out */

        if (g_now_ms - last_tick >= 1000u) {
            last_tick = g_now_ms;
            /* Publish live values through FC 4. */
            g_input[0] = (uint16_t)(g_now_ms / 1000u);
            g_input[1] = (uint16_t)(g_now_ms & 0xFFFFu);
        }
        app_delay_5ms();
    }
}
