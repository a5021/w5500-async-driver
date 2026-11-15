/**
 * @file w5500_dhcp.h
 * @brief Minimal DHCP client (RFC 2131) for the async W5500 driver.
 *
 * Built on top of the normal UDP socket API: one hardware socket and one
 * caller-provided state block, no heap, no threads. Drive it from the same
 * task that calls w5500_poll():
 *
 * @code
 * w5500_dhcp_t *dhcp;
 * w5500_dhcp_init(&dhcp, mem, sizeof mem, h, mac, on_dhcp, NULL);
 * w5500_dhcp_start(dhcp, now_ms());
 * for (;;) {
 *     w5500_poll(h);
 *     w5500_dhcp_poll(dhcp, now_ms());
 * }
 * @endcode
 *
 * On W5500_DHCP_EVENT_BOUND the lease is already applied to the chip
 * (SIPR/SUBR/GAR), so sockets can be opened from the callback.
 */
#ifndef W5500_DHCP_H
#define W5500_DHCP_H

#include <stddef.h>
#include <stdint.h>

#include "w5500.h"

#ifdef __cplusplus
extern "C" {
#endif

/** DHCP client instance; opaque, lives in caller storage. */
typedef struct w5500_dhcp w5500_dhcp_t;

/** Events delivered from w5500_dhcp_poll(). */
typedef enum {
    W5500_DHCP_EVENT_BOUND,   /**< Lease acquired; chip reconfigured */
    W5500_DHCP_EVENT_RENEWED, /**< Lease renewed with the same server */
    W5500_DHCP_EVENT_FAILED   /**< No server answered; client stopped */
} w5500_dhcp_event_t;

typedef void (*w5500_dhcp_cb_t)(w5500_dhcp_t *dhcp,
                                w5500_dhcp_event_t event,
                                void *user_arg);

/** Bytes required for one client instance (for static storage sizing). */
size_t w5500_dhcp_size(void);

/**
 * @brief Initialize a client in caller-provided storage.
 * @param out_dhcp  Receives the client pointer.
 * @param mem       Storage of at least w5500_dhcp_size() bytes.
 * @param mac       Station MAC used as DHCP client identifier (6 bytes).
 * @param cb        Event callback; required.
 */
w5500_result_t w5500_dhcp_init(w5500_dhcp_t **out_dhcp,
                               void *mem,
                               size_t mem_size,
                               w5500_handle_t chip,
                               const uint8_t mac[6],
                               w5500_dhcp_cb_t cb,
                               void *user_arg);

/** Open UDP port 68 and send the first DISCOVER (re)start). */
w5500_result_t w5500_dhcp_start(w5500_dhcp_t *dhcp, uint32_t now_ms);

/**
 * @brief Service the client: receive replies, retransmit, renew, expire.
 *
 * Retries every 4 s, gives up after 4 attempts (FAILED event). While BOUND
 * the lease is renewed at 50% and re-acquired at 100% of its lifetime.
 *
 * @param now_ms  Free-running millisecond counter (wraps allowed).
 */
w5500_result_t w5500_dhcp_poll(w5500_dhcp_t *dhcp, uint32_t now_ms);

/** Close the socket and return to the idle state (safe to call twice). */
void w5500_dhcp_stop(w5500_dhcp_t *dhcp);

/** Read the current lease; any output pointer may be NULL. */
w5500_result_t w5500_dhcp_get_config(const w5500_dhcp_t *dhcp,
                                     uint8_t ip[4],
                                     uint8_t mask[4],
                                     uint8_t gw[4],
                                     uint8_t dns[4]);

#ifdef __cplusplus
}
#endif

#endif /* W5500_DHCP_H */
