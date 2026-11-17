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

/** Library version (semantic, independent of the chip revision). */
#define W5500_VERSION_MAJOR         0u
#define W5500_VERSION_MINOR         5u
#define W5500_VERSION_PATCH         0u
#define W5500_VERSION_STRING        "0.5.0"

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
    void     (*delay_us)(uint32_t us);                     /**< Optional us delay; enables 100 us command polling */
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

/**
 * @brief Read VERSIONR and RTR to verify a healthy W5500 is on the bus.
 *
 * A real chip always reports VERSIONR == 0x04 and boots with the factory
 * retransmission timer (RTR == 0x07D0). Two independent probes make a
 * floating or miswired bus much less likely to pass by accident.
 */
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

/**
 * @brief Enable/disable the chip's ping block (MR.PB).
 *
 * With ping blocking on, the chip stops answering ICMP echo requests, which
 * hides the node from LAN scanners (WIZnet recommendation for exposed
 * devices). Off by default.
 */
w5500_result_t w5500_set_ping_block(w5500_handle_t handle, bool enable);

/**
 * @brief Set the TCP retransmission timer and retry count (RTR/RCR).
 * @param rtr_100us  Retransmission timeout in 100 us units (0x07D0 = 2 s).
 * @param rcr        Retry count (chip default 8).
 */
w5500_result_t w5500_set_retransmit(w5500_handle_t handle,
                                    uint16_t rtr_100us,
                                    uint8_t rcr);

/** Read back the current RTR (100 us units) and RCR values. */
w5500_result_t w5500_get_retransmit(w5500_handle_t handle,
                                    uint16_t *rtr_100us,
                                    uint8_t *rcr);

/** PHY link state decoded from PHYCFGR (LNK/SPD/DPX bits). */
typedef struct {
    uint8_t up;          /**< 1 while LNK_ON is set */
    uint8_t speed_100;   /**< 1 = 100BASE-TX, 0 = 10BASE-T */
    uint8_t full_duplex; /**< 1 = full duplex */
} w5500_link_info_t;

/**
 * @brief Link-state change callback, invoked from w5500_poll().
 * @param handle   Driver instance the change was observed on.
 * @param info     New link state.
 * @param user_arg Opaque pointer passed to w5500_set_link_cb().
 *
 * Runs in the poll caller's context with the driver lock released, so
 * re-entering the driver is safe.
 */
typedef void (*w5500_link_cb_t)(w5500_handle_t handle,
                                const w5500_link_info_t *info,
                                void *user_arg);

/** Read PHYCFGR and decode link state, speed and duplex. */
w5500_result_t w5500_get_link_info(w5500_handle_t handle,
                                   w5500_link_info_t *info);

/**
 * @brief Register a link-change callback (NULL disables it).
 *
 * w5500_poll() samples PHYCFGR when no interrupt is pending and fires the
 * callback once per observed LNK/SPD/DPX change. The first sample after
 * init only establishes the baseline and never fires.
 */
w5500_result_t w5500_set_link_cb(w5500_handle_t handle,
                                 w5500_link_cb_t cb,
                                 void *user_arg);

/* ------------------------------------------------------------------ */
/* Optional diagnostics                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Lightweight I/O counters. The caller owns the storage; the driver
 *        only keeps a pointer, so diagnostics cost nothing when disabled.
 */
typedef struct {
    uint32_t spi_transfers;      /**< completed low-level SPI frames */
    uint32_t spi_bytes;          /**< payload bytes shifted over SPI */
    uint32_t spi_errors;         /**< transfers rejected (missing SPI hooks) */
    uint32_t timeouts;           /**< bounded wait/command timeouts */
    uint32_t link_changes;       /**< link state changes seen in poll() */
    uint32_t socket_opens;
    uint32_t socket_closes;
    uint32_t bytes_sent;
    uint32_t bytes_received;
    uint32_t datagrams_sent;     /**< successful UDP sends */
    uint32_t datagrams_received; /**< successful UDP reads */
} w5500_stats_t;

/** Attach a caller-owned counter block (NULL disables accounting). */
w5500_result_t w5500_set_stats(w5500_handle_t handle, w5500_stats_t *stats);
/** Return the attached counter block, or NULL. */
w5500_stats_t *w5500_get_stats(w5500_handle_t handle);
/** Zero a counter block without attaching it. */
void w5500_stats_reset(w5500_stats_t *stats);

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

/**
 * @brief Configure per-socket TX/RX buffer sizes (KB), asymmetric layouts.
 * @param kb  One size per hardware socket (1, 2, 4, 8 or 16 KB each). The
 *            sum must not exceed the chip's 16 KB per direction. The same
 *            size is used for TX and RX of a socket.
 *
 * Must be called before sockets are opened; open socket handles are
 * invalidated on success.
 */
w5500_result_t w5500_set_socket_buffers(w5500_handle_t handle,
                                        const uint8_t kb[W5500_SOCKET_COUNT]);

/**
 * @brief Configure TX and RX buffer sizes independently.
 * @param tx_kb  Per-socket TX size in KB (1, 2, 4, 8, 16); sum <= 16.
 * @param rx_kb  Per-socket RX size in KB (1, 2, 4, 8, 16); sum <= 16.
 *
 * The 16 KB TX and 16 KB RX memories are independent, so e.g. a
 * streaming TX-heavy socket can take 8 KB while its peers take 2 KB each.
 */
w5500_result_t w5500_set_socket_buffers_ex(
    w5500_handle_t handle,
    const uint8_t tx_kb[W5500_SOCKET_COUNT],
    const uint8_t rx_kb[W5500_SOCKET_COUNT]);

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
