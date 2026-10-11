/**
 * @file mock_w5500.c
 * @brief Host-side W5500 emulator used by the unit tests.
 */
#include <string.h>

#include "w5500_registers.h"
#include "mock_w5500.h"

mock_w5500_t g_mock;

/* ------------------------------------------------------------------ */
/* Little helpers                                                      */
/* ------------------------------------------------------------------ */

static uint16_t sreg16(uint8_t id, uint8_t off)
{
    return (uint16_t)(((uint16_t)g_mock.sreg[id][off] << 8) |
                      g_mock.sreg[id][(uint8_t)(off + 1u)]);
}

static void sreg16_set(uint8_t id, uint8_t off, uint16_t v)
{
    g_mock.sreg[id][off] = (uint8_t)(v >> 8);
    g_mock.sreg[id][(uint8_t)(off + 1u)] = (uint8_t)(v & 0xFFu);
}

static uint16_t tx_size(uint8_t id)
{
    uint8_t kb = g_mock.sreg[id][W5500_Sn_TXBUF_SIZE];
    if (kb == 0u) {
        kb = 2u;
    }
    return (uint16_t)kb * 1024u;
}

static uint16_t rx_size(uint8_t id)
{
    uint8_t kb = g_mock.sreg[id][W5500_Sn_RXBUF_SIZE];
    if (kb == 0u) {
        kb = 2u;
    }
    return (uint16_t)kb * 1024u;
}

static uint16_t tx_free(uint8_t id)
{
    uint16_t used = (uint16_t)(sreg16(id, W5500_Sn_TX_WR) -
                               sreg16(id, W5500_Sn_TX_RD));
    return (uint16_t)(tx_size(id) - used);
}

static uint16_t rx_used(uint8_t id)
{
    return (uint16_t)(sreg16(id, W5500_Sn_RX_WR) -
                      sreg16(id, W5500_Sn_RX_RD));
}

/* ------------------------------------------------------------------ */
/* Reset / defaults                                                    */
/* ------------------------------------------------------------------ */

void mock_w5500_reset(void)
{
    uint8_t kn_cr = g_mock.cr_stuck;
    uint8_t kn_conn = g_mock.connect_ok;
    uint32_t locks = g_mock.lock_count;
    uint32_t unlocks = g_mock.unlock_count;
    uint32_t ms = g_mock.elapsed_ms;

    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.cr_stuck = kn_cr;
    g_mock.connect_ok = kn_conn;
    g_mock.lock_count = locks;
    g_mock.unlock_count = unlocks;
    g_mock.elapsed_ms = ms;
    g_mock.connect_ok = 1u;

    g_mock.common[W5500_REG_VERSIONR & 0x3Fu] = 0x04u;
    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x01u; /* link up */
    g_mock.common[W5500_REG_RTR & 0x3Fu] = 0x07u;
    g_mock.common[(W5500_REG_RTR & 0x3Fu) + 1u] = 0xD0u;
    g_mock.common[W5500_REG_RCR & 0x3Fu] = 8u;
}

/* ------------------------------------------------------------------ */
/* Socket command side effects                                         */
/* ------------------------------------------------------------------ */

static void mock_do_send(uint8_t id)
{
    uint16_t rd = sreg16(id, W5500_Sn_TX_RD);
    uint16_t wr = sreg16(id, W5500_Sn_TX_WR);
    uint16_t n = (uint16_t)(wr - rd);
    uint16_t sz = tx_size(id);
    uint16_t i;

    if (n > MOCK_CAPTURE_MAX) {
        n = MOCK_CAPTURE_MAX;
    }
    for (i = 0; i < n; i++) {
        g_mock.last_sent[id][i] =
            g_mock.tx_mem[id][(uint16_t)(rd + i) & (uint16_t)(sz - 1u)];
    }
    g_mock.last_sent_len[id] = n;
    sreg16_set(id, W5500_Sn_TX_RD, wr);
    g_mock.sreg[id][W5500_Sn_IR] |= W5500_Sn_IR_SENDOK;
}

void mock_w5500_complete_send(uint8_t sock)
{
    mock_do_send(sock);
}

static void mock_handle_cmd(uint8_t id, uint8_t cmd)
{
    uint8_t *sr = &g_mock.sreg[id][W5500_Sn_SR];
    uint8_t *ir = &g_mock.sreg[id][W5500_Sn_IR];

    switch (cmd) {
    case W5500_Sn_CR_OPEN: {
        uint8_t proto = g_mock.sreg[id][W5500_Sn_MR] & 0x0Fu;
        if (proto == W5500_Sn_MR_TCP) {
            *sr = W5500_SOCK_INIT;
        } else if (proto == W5500_Sn_MR_UDP) {
            *sr = W5500_SOCK_UDP;
        } else {
            *sr = W5500_SOCK_CLOSED;
        }
        sreg16_set(id, W5500_Sn_TX_RD, 0u);
        sreg16_set(id, W5500_Sn_TX_WR, 0u);
        sreg16_set(id, W5500_Sn_RX_RD, 0u);
        sreg16_set(id, W5500_Sn_RX_WR, 0u);
        break;
    }
    case W5500_Sn_CR_LISTEN:
        if (*sr == W5500_SOCK_INIT) {
            *sr = W5500_SOCK_LISTEN;
        } else {
            *sr = W5500_SOCK_CLOSED;
            *ir |= W5500_Sn_IR_TIMEOUT;
        }
        break;
    case W5500_Sn_CR_CONNECT:
        if (*sr == W5500_SOCK_INIT) {
            if (g_mock.connect_ok != 0u) {
                *sr = W5500_SOCK_ESTABLISHED;
                *ir |= W5500_Sn_IR_CON;
            } else {
                *sr = W5500_SOCK_CLOSED;
                *ir |= W5500_Sn_IR_TIMEOUT;
            }
        }
        break;
    case W5500_Sn_CR_DISCON:
        *sr = W5500_SOCK_CLOSED;
        *ir |= W5500_Sn_IR_DISCON;
        break;
    case W5500_Sn_CR_CLOSE:
        *sr = W5500_SOCK_CLOSED;
        *ir = 0u;
        break;
    case W5500_Sn_CR_SEND:
        if (g_mock.send_stall != 0u) {
            /* Accepted, but transmission never completes. */
            break;
        }
        mock_do_send(id);
        break;
    case W5500_Sn_CR_RECV:
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Memory model                                                        */
/* ------------------------------------------------------------------ */

static uint8_t mock_mem_read(uint8_t bsb, uint16_t addr)
{
    if (bsb == W5500_BSB_COMMON) {
        return g_mock.common[addr & 0x3Fu];
    }
    if (bsb >= 1u && bsb <= 15u) {
        uint8_t kind = (uint8_t)(bsb & 3u);
        uint8_t id = (uint8_t)((bsb - 1u) / 4u);
        if (id >= MOCK_MAX_SOCKS) {
            return 0u;
        }
        if (kind == 1u) {
            uint8_t off = (uint8_t)(addr & 0xFFu);
            if (off == W5500_Sn_TX_FSR || off == W5500_Sn_TX_FSR + 1u) {
                uint16_t v = tx_free(id);
                return ((off & 1u) != 0u) ? (uint8_t)(v & 0xFFu)
                                          : (uint8_t)(v >> 8);
            }
            if (off == W5500_Sn_RX_RSR || off == W5500_Sn_RX_RSR + 1u) {
                uint16_t v = rx_used(id);
                return ((off & 1u) != 0u) ? (uint8_t)(v & 0xFFu)
                                          : (uint8_t)(v >> 8);
            }
            return g_mock.sreg[id][off % 0x30u];
        }
        if (kind == 2u) {
            return g_mock.tx_mem[id][addr & (uint16_t)(tx_size(id) - 1u)];
        }
        if (kind == 3u) {
            return g_mock.rx_mem[id][addr & (uint16_t)(rx_size(id) - 1u)];
        }
    }
    return 0u;
}

static void mock_mem_write(uint8_t bsb, uint16_t addr, uint8_t val)
{
    if (bsb == W5500_BSB_COMMON) {
        uint8_t off = (uint8_t)(addr & 0x3Fu);
        if (off == (W5500_REG_MR & 0x3Fu) && (val & W5500_MR_RST) != 0u) {
            mock_w5500_reset(); /* S/W reset; knobs preserved */
            return;
        }
        g_mock.common[off] = val;
        return;
    }
    if (bsb >= 1u && bsb <= 15u) {
        uint8_t kind = (uint8_t)(bsb & 3u);
        uint8_t id = (uint8_t)((bsb - 1u) / 4u);
        if (id >= MOCK_MAX_SOCKS) {
            return;
        }
        if (kind == 1u) {
            uint8_t off = (uint8_t)(addr & 0xFFu);
            if (off == W5500_Sn_CR) {
                if (g_mock.cr_stuck != 0u) {
                    g_mock.sreg[id][off] = val; /* never self-clears */
                    return;
                }
                g_mock.sreg[id][off] = 0u; /* accepted => self-cleared */
                mock_handle_cmd(id, val);
                return;
            }
            if (off == W5500_Sn_IR) {
                g_mock.sreg[id][off] &= (uint8_t)~val; /* write-1-to-clear */
                return;
            }
            g_mock.sreg[id][off % 0x30u] = val;
            return;
        }
        if (kind == 2u) {
            g_mock.tx_mem[id][addr & (uint16_t)(tx_size(id) - 1u)] = val;
            return;
        }
        if (kind == 3u) {
            g_mock.rx_mem[id][addr & (uint16_t)(rx_size(id) - 1u)] = val;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* SPI state machine                                                   */
/* ------------------------------------------------------------------ */

static uint8_t mock_spi_byte(uint8_t b)
{
    if (g_mock.in_cs == 0u) {
        return 0xFFu; /* bus idle */
    }
    switch (g_mock.phase) {
    case 0u:
        g_mock.addr = (uint16_t)((uint16_t)b << 8);
        g_mock.phase = 1u;
        return 0u;
    case 1u:
        g_mock.addr |= b;
        g_mock.phase = 2u;
        return 0u;
    case 2u:
        g_mock.bsb = (uint8_t)((b >> 3) & 0x1Fu);
        g_mock.rwb = (uint8_t)((b >> 2) & 0x01u);
        g_mock.phase = 3u;
        return 0u;
    default: {
        uint8_t r = 0u;
        if (g_mock.rwb != 0u) {
            r = mock_mem_read(g_mock.bsb, g_mock.addr);
        } else {
            mock_mem_write(g_mock.bsb, g_mock.addr, b);
        }
        g_mock.addr = (uint16_t)(g_mock.addr + 1u);
        return r;
    }
    }
}

static void mock_spi_burst(uint8_t *tx_buf, uint8_t *rx_buf, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++) {
        uint8_t tx = (tx_buf != NULL) ? tx_buf[i] : 0u;
        uint8_t rx = mock_spi_byte(tx);
        if (rx_buf != NULL) {
            rx_buf[i] = rx;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Platform hooks                                                      */
/* ------------------------------------------------------------------ */

static void mock_select(void)
{
    g_mock.in_cs = 1u;
    g_mock.phase = 0u;
}

static void mock_deselect(void)
{
    g_mock.in_cs = 0u;
    g_mock.phase = 0u;
}

static void mock_delay_ms(uint32_t ms)
{
    g_mock.elapsed_ms += ms;
}

static void mock_lock(void)
{
    g_mock.lock_count++;
}

static void mock_unlock(void)
{
    g_mock.unlock_count++;
}

void mock_w5500_plat(w5500_plat_if_t *plat)
{
    plat->spi_select = mock_select;
    plat->spi_deselect = mock_deselect;
    plat->spi_transfer_byte = mock_spi_byte;
    plat->spi_transfer_burst = NULL; /* exercise the byte-loop path */
    plat->delay_ms = mock_delay_ms;
    plat->lock = mock_lock;
    plat->unlock = mock_unlock;
}

/* Same as mock_w5500_plat(), but enables the burst path. */
void mock_w5500_plat_burst(w5500_plat_if_t *plat)
{
    mock_w5500_plat(plat);
    plat->spi_transfer_burst = mock_spi_burst;
}

/* ------------------------------------------------------------------ */
/* Test helpers                                                        */
/* ------------------------------------------------------------------ */

void mock_w5500_inject_rx(uint8_t sock, const uint8_t *data, uint16_t len)
{
    uint16_t wr = sreg16(sock, W5500_Sn_RX_WR);
    uint16_t sz = rx_size(sock);
    uint16_t i;

    for (i = 0; i < len; i++) {
        g_mock.rx_mem[sock][(uint16_t)(wr + i) & (uint16_t)(sz - 1u)] =
            data[i];
    }
    sreg16_set(sock, W5500_Sn_RX_WR, (uint16_t)(wr + len));
    g_mock.sreg[sock][W5500_Sn_IR] |= W5500_Sn_IR_RECV;
}

void mock_w5500_remote_connect(uint8_t sock, const uint8_t peer_ip[4],
                               uint16_t peer_port)
{
    memcpy(&g_mock.sreg[sock][W5500_Sn_DIPR], peer_ip, 4u);
    sreg16_set(sock, W5500_Sn_DPORT, peer_port);
    g_mock.sreg[sock][W5500_Sn_SR] = W5500_SOCK_ESTABLISHED;
    g_mock.sreg[sock][W5500_Sn_IR] |= W5500_Sn_IR_CON;
}

void mock_w5500_remote_disconnect(uint8_t sock)
{
    g_mock.sreg[sock][W5500_Sn_SR] = W5500_SOCK_CLOSE_WAIT;
    g_mock.sreg[sock][W5500_Sn_IR] |= W5500_Sn_IR_DISCON;
}

uint8_t mock_w5500_reg(uint8_t sock, uint8_t offset)
{
    return g_mock.sreg[sock][offset % 0x30u];
}
