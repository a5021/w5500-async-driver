/**
 * @file w5500_internal.h
 * @brief Private context layout and low-level helpers shared by the core
 *        (w5500.c) and the socket layer (w5500_sockets.c).
 *
 * Not installed; opaque to library users.
 */
#ifndef W5500_INTERNAL_H
#define W5500_INTERNAL_H

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_registers.h"

/** Default chip command completion timeout (ms). */
#define W5500_DEFAULT_TIMEOUT_MS 1000u

/** Application-level socket slot state (SSR = socket software state). */
typedef enum {
    W5500_SSR_FREE = 0,
    W5500_SSR_INITIATED,   /**< OPEN accepted; TCP INIT or UDP state */
    W5500_SSR_LISTENING,
    W5500_SSR_CONNECTING,
    W5500_SSR_ESTABLISHED,
    W5500_SSR_CLOSING      /**< DISCON issued, waiting for CLOSED */
} w5500_sock_state_t;

/** One hardware socket slot, embedded in the context (no heap). */
struct w5500_socket {
    struct w5500_context *owner;
    uint8_t               id;        /**< Hardware socket index 0..3 */
    uint8_t               in_use;
    uint8_t               proto;     /**< W5500_Sn_MR_TCP / W5500_Sn_MR_UDP */
    w5500_sock_state_t    state;
    w5500_event_cb_t      cb;
    void                 *user_arg;
};

/** Driver instance. All chip state lives here; never a file-scope global. */
struct w5500_context {
    w5500_plat_if_t       plat;
    uint32_t              timeout_ms;
    uint8_t               owns_mem;  /**< 1 => allocated by w5500_init() */
    struct w5500_socket   sockets[W5500_SOCKET_COUNT];
};

/* --- platform lock helpers (no-ops when hooks are NULL) ------------ */
void w5500_lock(w5500_handle_t h);
void w5500_unlock(w5500_handle_t h);

/* --- low-level register / buffer access (caller holds the lock) --- */

w5500_result_t w5500_ll_read(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                             uint8_t *buf, uint16_t len);
w5500_result_t w5500_ll_write(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                              const uint8_t *buf, uint16_t len);

w5500_result_t w5500_read8(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                           uint8_t *val);
w5500_result_t w5500_write8(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                            uint8_t val);
w5500_result_t w5500_read16(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                            uint16_t *val);   /* big endian on the wire */
w5500_result_t w5500_write16(w5500_handle_t h, uint16_t addr, uint8_t bsb,
                             uint16_t val);

/**
 * Read a 16-bit counter register (Sn_TX_FSR / Sn_RX_RSR) twice and retry
 * until both samples match, per the datasheet recommendation.
 */
w5500_result_t w5500_read16_stable(w5500_handle_t h, uint16_t addr,
                                   uint8_t bsb, uint16_t *val);

/* --- timed waits ---------------------------------------------------- */

/** Poll @c addr until (value & mask) == 0 or timeout; 1 ms steps. */
w5500_result_t w5500_wait_reg_clear(w5500_handle_t h, uint16_t addr,
                                    uint8_t bsb, uint8_t mask);

/** Poll @c addr until value == expect or timeout; 1 ms steps. */
w5500_result_t w5500_wait_reg_eq(w5500_handle_t h, uint16_t addr,
                                 uint8_t bsb, uint8_t expect);

/**
 * Issue a socket command (Sn_CR) and wait until the register self-clears.
 * Returns W5500_ERR_TIMEOUT when the chip does not accept the command.
 */
w5500_result_t w5500_sn_exec_cmd(w5500_handle_t h, uint8_t sock_id,
                                 uint8_t cmd);

/* --- fuzz-only surface (compiled in with -DW5500_TEST_HARNESS) ------- */
#ifdef W5500_TEST_HARNESS
/** SPI control byte: (BSB << 3) | (RWB << 2) | OM. Exposed for fuzzing. */
uint8_t w5500_ctrl_byte(uint16_t addr, uint8_t bsb, uint8_t rwb);
#endif

#endif /* W5500_INTERNAL_H */
