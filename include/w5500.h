/**
 * @file w5500.h
 * @brief Public core API of the asynchronous W5500 driver (Layer 2).
 *
 * Pure C99/C11. No vendor headers. No global driver state: every function
 * takes a @ref w5500_handle_t that points to a caller-owned context.
 */
#ifndef W5500_H
#define W5500_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Expected value of the chip VERSIONR register (0x0039). */
#define W5500_HW_VERSION            0x04u

/** Number of hardware sockets implemented by the W5500. */
#define W5500_SOCKET_COUNT          4u

/** Opaque driver instance handle (multi-context, no globals). */
typedef struct w5500_context *w5500_handle_t;

/** Status codes returned by every fallible driver function. */
typedef enum {
    W5500_OK           =  0, /**< Success */
    W5500_ERR_PARAM    = -1, /**< Invalid argument */
    W5500_ERR_TIMEOUT  = -2, /**< Chip did not respond in time */
    W5500_ERR_SPI      = -3, /**< SPI/platform layer failure */
    W5500_ERR_STATE    = -4, /**< Operation not valid in current state */
    W5500_ERR_NOMEM    = -5, /**< Out of memory / out of socket slots */
    W5500_ERR_CHIP     = -6, /**< Unexpected chip version / device error */
    W5500_ERR_BUSY     = -7  /**< Resource is busy, try again later */
} w5500_result_t;

/**
 * @brief Platform interface (Layer 1).
 *
 * The core never touches MCU registers; all I/O goes through these hooks.
 * @c lock/@c unlock are optional RTOS mutex hooks; leave them NULL in
 * single-threaded bare-metal builds.
 *
 * Burst transfer notes: for @c spi_transfer_burst either @c tx_buf or
 * @c rx_buf may be NULL (TX-only or RX-only), enabling DMA ping-pong use.
 */
typedef struct {
    void     (*spi_select)(void);                          /**< Assert CS (active low) */
    void     (*spi_deselect)(void);                        /**< Deassert CS */
    uint8_t  (*spi_transfer_byte)(uint8_t byte);           /**< Full-duplex byte exchange */
    void     (*spi_transfer_burst)(uint8_t *tx_buf, uint8_t *rx_buf, uint16_t len); /**< Optional; NULL => byte loop */
    void     (*delay_ms)(uint32_t ms);                     /**< Blocking delay, used by timeout waits */
    void     (*lock)(void);                                /**< Optional mutex take */
    void     (*unlock)(void);                              /**< Optional mutex give */
} w5500_plat_if_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a driver instance (allocates the context with malloc()).
 * @param out_handle  Receives the new handle on success.
 * @param plat        Platform interface; copied by value, must stay valid.
 */
w5500_result_t w5500_init(w5500_handle_t *out_handle,
                          const w5500_plat_if_t *plat);

/**
 * @brief Create a driver instance using caller-provided storage (no malloc).
 * @param out_handle  Receives the new handle on success.
 * @param mem         Storage of at least @c mem_size bytes, properly aligned.
 * @param mem_size    Size of @c mem in bytes (see w5500_context_size()).
 * @param plat        Platform interface; copied by value.
 */
w5500_result_t w5500_init_static(w5500_handle_t *out_handle,
                                 void *mem,
                                 size_t mem_size,
                                 const w5500_plat_if_t *plat);

/** Minimum storage size required by w5500_init_static(). */
size_t w5500_context_size(void);

/**
 * @brief Destroy an instance created by w5500_init() / w5500_init_static().
 *
 * Releases all sockets. Frees memory only for instances created by
 * w5500_init(). Safe to pass NULL.
 */
void w5500_deinit(w5500_handle_t handle);

/* ------------------------------------------------------------------ */
/* Chip control                                                       */
/* ------------------------------------------------------------------ */

/** Software-reset the chip (MR.RST) and wait for completion. */
w5500_result_t w5500_sw_reset(w5500_handle_t handle);

/** Read VERSIONR and verify it equals @ref W5500_HW_VERSION. */
w5500_result_t w5500_check(w5500_handle_t handle);

/** Read the raw chip version from VERSIONR (0x04 for a healthy W5500). */
w5500_result_t w5500_get_hw_version(w5500_handle_t handle, uint8_t *version);

/**
 * @brief Set the internal timeout used for chip command completion waits.
 * @param timeout_ms  Default is 1000 ms; 0 is rejected (keeps previous).
 */
w5500_result_t w5500_set_timeout(w5500_handle_t handle, uint32_t timeout_ms);

/** Read PHYCFGR and report link state (bit 0: LNK_ON). */
w5500_result_t w5500_get_link_status(w5500_handle_t handle, bool *link_up);

/* ------------------------------------------------------------------ */
/* Network configuration                                              */
/* ------------------------------------------------------------------ */

w5500_result_t w5500_set_mac(w5500_handle_t handle, const uint8_t mac[6]);
w5500_result_t w5500_get_mac(w5500_handle_t handle, uint8_t mac[6]);
w5500_result_t w5500_set_ip(w5500_handle_t handle, const uint8_t ip[4]);
w5500_result_t w5500_get_ip(w5500_handle_t handle, uint8_t ip[4]);
w5500_result_t w5500_set_subnet_mask(w5500_handle_t handle, const uint8_t mask[4]);
w5500_result_t w5500_get_subnet_mask(w5500_handle_t handle, uint8_t mask[4]);
w5500_result_t w5500_set_gateway(w5500_handle_t handle, const uint8_t gw[4]);
w5500_result_t w5500_get_gateway(w5500_handle_t handle, uint8_t gw[4]);

/**
 * @brief Configure TX/RX buffer size (KB) for every socket.
 * @param size_kb  One of 1, 2, 4, 8, 16. The sum over the 4 sockets must
 *                 not exceed 16 KB per direction (2 is the chip default).
 *
 * Must be called before sockets are opened. Socket handles opened earlier
 * are invalidated.
 */
w5500_result_t w5500_set_socket_buffer_size(w5500_handle_t handle,
                                             uint16_t size_kb);

/* ------------------------------------------------------------------ */
/* Raw register access (advanced/debug)                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Read @c len bytes starting at @c addr in register/buffer block @c bsb.
 *
 * The block-select encoding lives in src/w5500_registers.h (W5500_BSB_*).
 * Prefer the high-level API; these exist for diagnostics and custom needs.
 */
w5500_result_t w5500_read_regs(w5500_handle_t handle,
                               uint16_t addr,
                               uint8_t bsb,
                               uint8_t *buf,
                               uint16_t len);

/** Write @c len bytes starting at @c addr in block @c bsb. */
w5500_result_t w5500_write_regs(w5500_handle_t handle,
                                uint16_t addr,
                                uint8_t bsb,
                                const uint8_t *buf,
                                uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* W5500_H */
