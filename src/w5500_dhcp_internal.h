/**
 * @file w5500_dhcp_internal.h
 * @brief DHCP packet codec shared by the client and the fuzz harnesses.
 */
#ifndef W5500_DHCP_INTERNAL_H
#define W5500_DHCP_INTERNAL_H

#include <stdint.h>

/** Decoded BOOTREPLY/DHCP message (only the fields the client uses). */
typedef struct {
    uint8_t  msg_type;      /**< DHCP option 53 value; 0 = absent */
    uint8_t  ip[4];         /**< yiaddr */
    uint8_t  mask[4];       /**< option 1 */
    uint8_t  gw[4];         /**< option 3 (first router) */
    uint8_t  dns[4];        /**< option 6 (first server) */
    uint8_t  server[4];     /**< option 54 */
    uint8_t  has_ip;
    uint8_t  has_mask;
    uint8_t  has_gw;
    uint8_t  has_dns;
    uint8_t  has_server;
    uint8_t  has_lease;
    uint32_t lease_s;       /**< option 51, seconds (already > 0) */
} w5500_dhcp_parse_t;

/**
 * @brief Parse a DHCP reply and decode the options the client needs.
 * @return 0 on success, -1 when the packet is not a well-formed reply.
 *
 * Robust against arbitrary input: every option is bounds checked, unknown
 * options are skipped, malformed lengths abort the walk.
 */
int w5500_dhcp_parse_packet(const uint8_t *pkt,
                            uint16_t len,
                            w5500_dhcp_parse_t *out);

#endif /* W5500_DHCP_INTERNAL_H */
