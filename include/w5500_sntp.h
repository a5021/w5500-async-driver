/**
 * @file w5500_sntp.h
 * @brief Minimal SNTP client (RFC 4330 mode 3) for the async W5500 driver.
 *
 * Same shape as the DNS resolver: one UDP socket, caller-provided storage,
 * tick driven, no heap. The callback receives Unix time (seconds).
 */
#ifndef W5500_SNTP_H
#define W5500_SNTP_H

#include <stddef.h>
#include <stdint.h>

#include "w5500.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SNTP client instance; opaque, lives in caller storage. */
typedef struct w5500_sntp w5500_sntp_t;

/**
 * @brief Result callback (exactly once per started request).
 * @param status  W5500_OK (unix_time valid), W5500_ERR_TIMEOUT (no answer
 *                after the retries) or W5500_ERR_STATE (bad server reply).
 */
typedef void (*w5500_sntp_cb_t)(w5500_sntp_t *sntp,
                                uint32_t unix_time,
                                w5500_result_t status,
                                void *user_arg);

/** Bytes required for one client instance. */
size_t w5500_sntp_size(void);

/** Initialize a client in caller-provided storage. */
w5500_result_t w5500_sntp_init(w5500_sntp_t **out_sntp,
                               void *mem,
                               size_t mem_size,
                               w5500_handle_t chip,
                               const uint8_t server[4],
                               w5500_sntp_cb_t cb,
                               void *user_arg);

/** Change the server address (takes effect on the next request). */
w5500_result_t w5500_sntp_set_server(w5500_sntp_t *sntp,
                                     const uint8_t server[4]);

/** Send a mode-3 request; cancels any request in flight silently. */
w5500_result_t w5500_sntp_start(w5500_sntp_t *sntp, uint32_t now_ms);

/**
 * @brief Service the client: receive the answer, retry, time out.
 *
 * Retries every 2 s, three attempts, then W5500_ERR_TIMEOUT.
 */
w5500_result_t w5500_sntp_poll(w5500_sntp_t *sntp, uint32_t now_ms);

/** Close the socket and cancel any pending request (no callback). */
void w5500_sntp_stop(w5500_sntp_t *sntp);

#ifdef __cplusplus
}
#endif

#endif /* W5500_SNTP_H */
