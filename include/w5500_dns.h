/**
 * @file w5500_dns.h
 * @brief Minimal DNS A-record resolver for the async W5500 driver.
 *
 * One UDP socket, one caller-provided state block, tick driven like the
 * DHCP client:
 *
 * @code
 * w5500_dns_t *dns;
 * w5500_dns_init(&dns, mem, sizeof mem, h, server_ip, on_dns, NULL);
 * w5500_dns_start(dns, "example.com", now_ms());
 * // then call w5500_dns_poll(dns, now_ms()) next to w5500_poll()
 * @endcode
 */
#ifndef W5500_DNS_H
#define W5500_DNS_H

#include <stddef.h>
#include <stdint.h>

#include "w5500.h"

#ifdef __cplusplus
extern "C" {
#endif

/** DNS resolver instance; opaque, lives in caller storage. */
typedef struct w5500_dns w5500_dns_t;

/** Maximum number of A records delivered for one query. */
#define W5500_DNS_MAX_ADDRS 4u

/** One resolved address and its remaining TTL. */
typedef struct {
    uint8_t  ip[4];   /**< IPv4 address */
    uint32_t ttl_s;   /**< TTL in seconds (0 = do not cache) */
} w5500_dns_addr_t;

/**
 * @brief Result callback (always called exactly once per started query).
 * @param status  W5500_OK (ip valid), W5500_ERR_TIMEOUT (no answer after
 *                the retries) or W5500_ERR_STATE (server error/NXDOMAIN).
 */
typedef void (*w5500_dns_cb_t)(w5500_dns_t *dns,
                               const uint8_t ip[4],
                               w5500_result_t status,
                               void *user_arg);
/**
 * @brief Callback variant that receives every A record and its TTL.
 *
 * CNAME chains are followed, so @p addrs may hold records for the canonical
 * name. On error @p count is 0 and @p addrs is NULL.
 */
typedef void (*w5500_dns_multi_cb_t)(w5500_dns_t *dns,
                                     const w5500_dns_addr_t *addrs,
                                     uint8_t count,
                                     w5500_result_t status,
                                     void *user_arg);

/** Bytes required for one resolver instance. */
size_t w5500_dns_size(void);

/**
 * @brief Initialize a resolver in caller-provided storage.
 * @param server  DNS server IPv4 address (e.g. from DHCP option 6).
 */
w5500_result_t w5500_dns_init(w5500_dns_t **out_dns,
                              void *mem,
                              size_t mem_size,
                              w5500_handle_t chip,
                              const uint8_t server[4],
                              w5500_dns_cb_t cb,
                              void *user_arg);
/**
 * @brief Like w5500_dns_init(), but delivers all A records and their TTLs.
 */
w5500_result_t w5500_dns_init_multi(w5500_dns_t **out_dns,
                                    void *mem,
                                    size_t mem_size,
                                    w5500_handle_t chip,
                                    const uint8_t server[4],
                                    w5500_dns_multi_cb_t cb,
                                    void *user_arg);
/**
 * @brief Enable or disable the small answer cache (disabled by default).
 *
 * Cached answers are delivered from the next w5500_dns_poll() call. Entries
 * expire by TTL; changing the server clears them.
 */
void w5500_dns_set_cache(w5500_dns_t *dns, uint8_t enable);
/** Drop all cached answers. */
void w5500_dns_cache_clear(w5500_dns_t *dns);

/** Change the server address (takes effect on the next query). */
w5500_result_t w5500_dns_set_server(w5500_dns_t *dns, const uint8_t server[4]);

/**
 * @brief Send an A query for @p name (dotted host name, <= 253 chars).
 *
 * The name is copied into the state block, so the pointer may die at once.
 * A previous query on the same instance is cancelled silently.
 */
w5500_result_t w5500_dns_start(w5500_dns_t *dns,
                               const char *name,
                               uint32_t now_ms);

/**
 * @brief Service the resolver: receive the answer, retry, time out.
 *
 * Retries every 2 s, three attempts, then W5500_ERR_TIMEOUT. A single
 * answer finishes the query; the callback runs in this context.
 */
w5500_result_t w5500_dns_poll(w5500_dns_t *dns, uint32_t now_ms);

/** Close the socket and cancel any pending query (no callback). */
void w5500_dns_stop(w5500_dns_t *dns);

#ifdef __cplusplus
}
#endif

#endif /* W5500_DNS_H */
