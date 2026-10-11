/**
 * @file w5500.c
 * @brief W5500 core: SPI framing, register access, chip and network setup.
 *
 * Pure C99, no vendor headers, no global state. Every transaction is
 * guarded by the optional platform lock and every wait is bounded by a
 * software timeout.
 */
#include <stdlib.h>
#include <string.h>

#include "w5500.h"
#include "w5500_internal.h"

/* ------------------------------------------------------------------ */
/* Lock helpers                                                        */
/* ------------------------------------------------------------------ */

void w5500_lock(w5500_handle_t h)
{
    if (h->plat.lock != NULL) {
        h->plat.lock();
    }
}

void w5500_unlock(w5500_handle_t h)
{
    if (h->plat.unlock != NULL) {
        h->plat.unlock();
    }
}

/* ------------------------------------------------------------------ */
/* SPI framing                                                         */
/* ------------------------------------------------------------------ */

static uint8_t w5500_ctrl_byte(uint16_t addr, uint8_t bsb, uint8_t rwb)
{
    (void)addr;
    return (uint8_t)(((bsb & 0x1Fu) << 3) |
                     ((rwb & 0x01u) << 2) |
                     W5500_SPI_OM_VDM);
}

static void w5500_spi_byte(w5500_handle_t h, uint8_t byte)
{
    (void)h->plat.spi_transfer_byte(byte);
}

w5500_result_t w5500_ll_read(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                             uint8_t *buf, uint16_t len)
{
    if (h == NULL || buf == NULL || len == 0u) {
        return W5500_ERR_PARAM;
    }
    if (h->plat.spi_select == NULL || h->plat.spi_deselect == NULL ||
        h->plat.spi_transfer_byte == NULL) {
        return W5500_ERR_SPI;
    }

    h->plat.spi_select();
    w5500_spi_byte(h, (uint8_t)(addr >> 8));
    w5500_spi_byte(h, (uint8_t)(addr & 0xFFu));
    w5500_spi_byte(h, w5500_ctrl_byte(addr, bsb, W5500_SPI_RWB_READ));

    if (h->plat.spi_transfer_burst != NULL) {
        h->plat.spi_transfer_burst(NULL, buf, len);
    } else {
        uint16_t i;
        for (i = 0; i < len; i++) {
            buf[i] = h->plat.spi_transfer_byte(0x00u);
        }
    }
    h->plat.spi_deselect();
    return W5500_OK;
}

w5500_result_t w5500_ll_write(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                              const uint8_t *buf, uint16_t len)
{
    if (h == NULL || buf == NULL || len == 0u) {
        return W5500_ERR_PARAM;
    }
    if (h->plat.spi_select == NULL || h->plat.spi_deselect == NULL ||
        h->plat.spi_transfer_byte == NULL) {
        return W5500_ERR_SPI;
    }

    h->plat.spi_select();
    w5500_spi_byte(h, (uint8_t)(addr >> 8));
    w5500_spi_byte(h, (uint8_t)(addr & 0xFFu));
    w5500_spi_byte(h, w5500_ctrl_byte(addr, bsb, W5500_SPI_RWB_WRITE));

    if (h->plat.spi_transfer_burst != NULL) {
        h->plat.spi_transfer_burst((uint8_t *)(uintptr_t)buf, NULL, len);
    } else {
        uint16_t i;
        for (i = 0; i < len; i++) {
            w5500_spi_byte(h, buf[i]);
        }
    }
    h->plat.spi_deselect();
    return W5500_OK;
}

w5500_result_t w5500_read8(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                           uint8_t *val)
{
    return w5500_ll_read(h, addr, bsb, val, 1u);
}

w5500_result_t w5500_write8(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                            uint8_t val)
{
    return w5500_ll_write(h, addr, bsb, &val, 1u);
}

w5500_result_t w5500_read16(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                            uint16_t *val)
{
    uint8_t raw[2];
    w5500_result_t rc = w5500_ll_read(h, addr, bsb, raw, 2u);
    if (rc == W5500_OK) {
        *val = (uint16_t)(((uint16_t)raw[0] << 8) | raw[1]);
    }
    return rc;
}

w5500_result_t w5500_write16(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                             uint16_t val)
{
    uint8_t raw[2];
    raw[0] = (uint8_t)(val >> 8);
    raw[1] = (uint8_t)(val & 0xFFu);
    return w5500_ll_write(h, addr, bsb, raw, 2u);
}

w5500_result_t w5500_read16_stable(w5500_handle_t h, uint16_t addr,
                                   uint8_t bsb, uint16_t *val)
{
    uint16_t a = 0;
    uint16_t b = 0;
    w5500_result_t rc;
    uint8_t attempt;

    for (attempt = 0; attempt < 8u; attempt++) {
        rc = w5500_read16(h, addr, bsb, &a);
        if (rc != W5500_OK) {
            return rc;
        }
        rc = w5500_read16(h, addr, bsb, &b);
        if (rc != W5500_OK) {
            return rc;
        }
        if (a == b) {
            *val = a;
            return W5500_OK;
        }
    }
    return W5500_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* Timed waits (always bounded — no infinite polling)                  */
/* ------------------------------------------------------------------ */

w5500_result_t w5500_wait_reg_clear(w5500_handle_t h, uint16_t addr,
                                    uint8_t bsb, uint8_t mask)
{
    uint32_t elapsed = 0u;

    for (;;) {
        uint8_t val = 0xFFu;
        w5500_result_t rc = w5500_read8(h, addr, bsb, &val);
        if (rc != W5500_OK) {
            return rc;
        }
        if ((val & mask) == 0u) {
            return W5500_OK;
        }
        if (elapsed >= h->timeout_ms) {
            return W5500_ERR_TIMEOUT;
        }
        if (h->plat.delay_ms != NULL) {
            h->plat.delay_ms(1u);
        }
        elapsed++;
    }
}

w5500_result_t w5500_wait_reg_eq(w5500_handle_t h, uint16_t addr,
                                 uint8_t bsb, uint8_t expect)
{
    uint32_t elapsed = 0u;

    for (;;) {
        uint8_t val = 0xFFu;
        w5500_result_t rc = w5500_read8(h, addr, bsb, &val);
        if (rc != W5500_OK) {
            return rc;
        }
        if (val == expect) {
            return W5500_OK;
        }
        if (elapsed >= h->timeout_ms) {
            return W5500_ERR_TIMEOUT;
        }
        if (h->plat.delay_ms != NULL) {
            h->plat.delay_ms(1u);
        }
        elapsed++;
    }
}

w5500_result_t w5500_sn_exec_cmd(w5500_handle_t h, uint8_t sock_id,
                                 uint8_t cmd)
{
    w5500_result_t rc = w5500_write8(h, W5500_Sn_CR,
                                     W5500_BSB_SREG(sock_id), cmd);
    if (rc != W5500_OK) {
        return rc;
    }
    /* The chip self-clears Sn_CR when the command is accepted. */
    return w5500_wait_reg_clear(h, W5500_Sn_CR, W5500_BSB_SREG(sock_id), 0xFFu);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void w5500_context_reset(struct w5500_context *ctx, uint8_t owns_mem)
{
    uint8_t i;

    memset(ctx, 0, sizeof(*ctx));
    ctx->timeout_ms = W5500_DEFAULT_TIMEOUT_MS;
    ctx->owns_mem = owns_mem;
    for (i = 0; i < W5500_SOCKET_COUNT; i++) {
        ctx->sockets[i].owner = ctx;
        ctx->sockets[i].id = i;
        ctx->sockets[i].in_use = 0u;
        ctx->sockets[i].state = W5500_SSR_FREE;
    }
}

static w5500_result_t w5500_validate_plat(const w5500_plat_if_t *plat)
{
    if (plat == NULL ||
        plat->spi_select == NULL ||
        plat->spi_deselect == NULL ||
        plat->spi_transfer_byte == NULL) {
        return W5500_ERR_PARAM;
    }
    return W5500_OK;
}

w5500_result_t w5500_init(w5500_handle_t *out_handle,
                          const w5500_plat_if_t *plat)
{
    struct w5500_context *ctx;
    w5500_result_t rc = w5500_validate_plat(plat);
    if (rc != W5500_OK || out_handle == NULL) {
        return (rc != W5500_OK) ? rc : W5500_ERR_PARAM;
    }

    ctx = (struct w5500_context *)malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return W5500_ERR_NOMEM;
    }
    w5500_context_reset(ctx, 1u);
    ctx->plat = *plat;
    *out_handle = ctx;
    return W5500_OK;
}

size_t w5500_context_size(void)
{
    return sizeof(struct w5500_context);
}

w5500_result_t w5500_init_static(w5500_handle_t *out_handle,
                                 void *mem,
                                 size_t mem_size,
                                 const w5500_plat_if_t *plat)
{
    struct w5500_context *ctx;
    w5500_result_t rc = w5500_validate_plat(plat);
    if (rc != W5500_OK || out_handle == NULL || mem == NULL) {
        return (rc != W5500_OK) ? rc : W5500_ERR_PARAM;
    }
    if (mem_size < sizeof(*ctx)) {
        return W5500_ERR_NOMEM;
    }

    ctx = (struct w5500_context *)mem;
    w5500_context_reset(ctx, 0u);
    ctx->plat = *plat;
    *out_handle = ctx;
    return W5500_OK;
}

void w5500_deinit(w5500_handle_t handle)
{
    uint8_t owns;

    if (handle == NULL) {
        return;
    }
    owns = handle->owns_mem;
    memset(handle, 0, sizeof(*handle));
    if (owns != 0u) {
        free(handle);
    }
}

/* ------------------------------------------------------------------ */
/* Chip control                                                        */
/* ------------------------------------------------------------------ */

w5500_result_t w5500_sw_reset(w5500_handle_t handle)
{
    w5500_result_t rc;

    if (handle == NULL) {
        return W5500_ERR_PARAM;
    }
    w5500_lock(handle);
    rc = w5500_write8(handle, W5500_REG_MR, W5500_BSB_COMMON, W5500_MR_RST);
    if (rc == W5500_OK) {
        /* MR.RST self-clears once the reset sequence finishes. */
        rc = w5500_wait_reg_clear(handle, W5500_REG_MR, W5500_BSB_COMMON,
                                  W5500_MR_RST);
    }
    w5500_unlock(handle);
    return rc;
}

w5500_result_t w5500_get_hw_version(w5500_handle_t handle, uint8_t *version)
{
    w5500_result_t rc;

    if (handle == NULL || version == NULL) {
        return W5500_ERR_PARAM;
    }
    w5500_lock(handle);
    rc = w5500_read8(handle, W5500_REG_VERSIONR, W5500_BSB_COMMON, version);
    w5500_unlock(handle);
    return rc;
}

w5500_result_t w5500_check(w5500_handle_t handle)
{
    uint8_t ver = 0u;
    w5500_result_t rc = w5500_get_hw_version(handle, &ver);
    if (rc != W5500_OK) {
        return rc;
    }
    return (ver == W5500_HW_VERSION) ? W5500_OK : W5500_ERR_CHIP;
}

w5500_result_t w5500_set_timeout(w5500_handle_t handle, uint32_t timeout_ms)
{
    if (handle == NULL || timeout_ms == 0u) {
        return W5500_ERR_PARAM;
    }
    w5500_lock(handle);
    handle->timeout_ms = timeout_ms;
    w5500_unlock(handle);
    return W5500_OK;
}

w5500_result_t w5500_get_link_status(w5500_handle_t handle, bool *link_up)
{
    uint8_t phy = 0u;
    w5500_result_t rc;

    if (handle == NULL || link_up == NULL) {
        return W5500_ERR_PARAM;
    }
    w5500_lock(handle);
    rc = w5500_read8(handle, W5500_REG_PHYCFGR, W5500_BSB_COMMON, &phy);
    w5500_unlock(handle);
    if (rc == W5500_OK) {
        *link_up = ((phy & W5500_PHYCFGR_LNK_ON) != 0u);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Network configuration                                               */
/* ------------------------------------------------------------------ */

static w5500_result_t w5500_set_net_reg(w5500_handle_t h, uint16_t addr,
                                        const uint8_t *val, uint16_t len)
{
    w5500_result_t rc;
    if (h == NULL || val == NULL) {
        return W5500_ERR_PARAM;
    }
    w5500_lock(h);
    rc = w5500_ll_write(h, addr, W5500_BSB_COMMON, val, len);
    w5500_unlock(h);
    return rc;
}

static w5500_result_t w5500_get_net_reg(w5500_handle_t h, uint16_t addr,
                                        uint8_t *val, uint16_t len)
{
    w5500_result_t rc;
    if (h == NULL || val == NULL) {
        return W5500_ERR_PARAM;
    }
    w5500_lock(h);
    rc = w5500_ll_read(h, addr, W5500_BSB_COMMON, val, len);
    w5500_unlock(h);
    return rc;
}

w5500_result_t w5500_set_mac(w5500_handle_t handle, const uint8_t mac[6])
{
    return w5500_set_net_reg(handle, W5500_REG_SHAR, mac, 6u);
}

w5500_result_t w5500_get_mac(w5500_handle_t handle, uint8_t mac[6])
{
    return w5500_get_net_reg(handle, W5500_REG_SHAR, mac, 6u);
}

w5500_result_t w5500_set_ip(w5500_handle_t handle, const uint8_t ip[4])
{
    return w5500_set_net_reg(handle, W5500_REG_SIPR, ip, 4u);
}

w5500_result_t w5500_get_ip(w5500_handle_t handle, uint8_t ip[4])
{
    return w5500_get_net_reg(handle, W5500_REG_SIPR, ip, 4u);
}

w5500_result_t w5500_set_subnet_mask(w5500_handle_t handle, const uint8_t mask[4])
{
    return w5500_set_net_reg(handle, W5500_REG_SUBR, mask, 4u);
}

w5500_result_t w5500_get_subnet_mask(w5500_handle_t handle, uint8_t mask[4])
{
    return w5500_get_net_reg(handle, W5500_REG_SUBR, mask, 4u);
}

w5500_result_t w5500_set_gateway(w5500_handle_t handle, const uint8_t gw[4])
{
    return w5500_set_net_reg(handle, W5500_REG_GAR, gw, 4u);
}

w5500_result_t w5500_get_gateway(w5500_handle_t handle, uint8_t gw[4])
{
    return w5500_get_net_reg(handle, W5500_REG_GAR, gw, 4u);
}

static int w5500_buf_size_valid(uint16_t kb)
{
    return (kb == 1u || kb == 2u || kb == 4u || kb == 8u || kb == 16u);
}

w5500_result_t w5500_set_socket_buffer_size(w5500_handle_t handle,
                                             uint16_t size_kb)
{
    uint8_t i;
    w5500_result_t rc = W5500_OK;

    if (handle == NULL || !w5500_buf_size_valid(size_kb)) {
        return W5500_ERR_PARAM;
    }
    /* 4 sockets x size_kb must fit into 16 KB per direction. */
    if ((uint32_t)size_kb * W5500_SOCKET_COUNT > 16u) {
        return W5500_ERR_PARAM;
    }

    w5500_lock(handle);
    for (i = 0; i < W5500_SOCKET_COUNT; i++) {
        rc = w5500_write8(handle, W5500_Sn_TXBUF_SIZE, W5500_BSB_SREG(i),
                          (uint8_t)size_kb);
        if (rc != W5500_OK) {
            break;
        }
        rc = w5500_write8(handle, W5500_Sn_RXBUF_SIZE, W5500_BSB_SREG(i),
                          (uint8_t)size_kb);
        if (rc != W5500_OK) {
            break;
        }
    }
    if (rc == W5500_OK) {
        /* Buffer layout changed; stale socket bookkeeping is invalid. */
        for (i = 0; i < W5500_SOCKET_COUNT; i++) {
            handle->sockets[i].in_use = 0u;
            handle->sockets[i].state = W5500_SSR_FREE;
            handle->sockets[i].cb = NULL;
            handle->sockets[i].user_arg = NULL;
        }
    }
    w5500_unlock(handle);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Raw register access                                                 */
/* ------------------------------------------------------------------ */

w5500_result_t w5500_read_regs(w5500_handle_t handle,
                               uint16_t addr,
                               uint8_t bsb,
                               uint8_t *buf,
                               uint16_t len)
{
    w5500_result_t rc;
    if (handle == NULL) {
        return W5500_ERR_PARAM;
    }
    w5500_lock(handle);
    rc = w5500_ll_read(handle, addr, bsb, buf, len);
    w5500_unlock(handle);
    return rc;
}

w5500_result_t w5500_write_regs(w5500_handle_t handle,
                                uint16_t addr,
                                uint8_t bsb,
                                const uint8_t *buf,
                                uint16_t len)
{
    w5500_result_t rc;
    if (handle == NULL) {
        return W5500_ERR_PARAM;
    }
    w5500_lock(handle);
    rc = w5500_ll_write(handle, addr, bsb, buf, len);
    w5500_unlock(handle);
    return rc;
}
