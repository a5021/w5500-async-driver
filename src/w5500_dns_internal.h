/**
 * @file w5500_dns_internal.h
 * @brief DNS response codec shared by the resolver and the fuzz harness.
 */
#ifndef W5500_DNS_INTERNAL_H
#define W5500_DNS_INTERNAL_H

#include <stdint.h>

/**
 * @brief Parse a DNS response and extract the first A record.
 *
 * Handles compression pointers in owner names, skips the question section
 * and non-A answers. Every read is bounds checked; malformed input returns
 * -1 without touching @p ip_out.
 *
 * @param qid     Expected transaction id.
 * @param ip_out  Receives the IPv4 address on success.
 * @return 0 when an A record was found, -1 otherwise.
 */
int w5500_dns_parse_response(const uint8_t *pkt,
                             uint16_t len,
                             uint16_t qid,
                             uint8_t ip_out[4]);

#endif /* W5500_DNS_INTERNAL_H */
