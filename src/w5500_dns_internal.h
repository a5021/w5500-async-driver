/**
 * @file w5500_dns_internal.h
 * @brief DNS response codec shared by the resolver and the fuzz harnesses.
 */
#ifndef W5500_DNS_INTERNAL_H
#define W5500_DNS_INTERNAL_H
#include <stdint.h>
#include "w5500_dns.h"
/**
 * @brief Parse a DNS response, follow CNAMEs and collect A records.
 *
 * Handles compression pointers in owner and CNAME target names, skips the
 * question section, and ignores records that do not belong to @p wanted.
 * Every read is bounds checked; malformed input returns -1 without touching
 * the output.
 *
 * @param qid       Expected transaction id.
 * @param wanted    Dotted name being resolved (case-insensitive), or NULL to
 *                  accept any IN A record.
 * @param addrs     Receives up to @p max_addrs address/TTL pairs.
 * @param max_addrs Capacity of @p addrs (1..W5500_DNS_MAX_ADDRS).
 * @param count     Receives the number of addresses found (may be 0).
 * @return 0 for a well-formed response, -1 on error.
 */
int w5500_dns_parse_response_ex(const uint8_t *pkt,
                                uint16_t len,
                                uint16_t qid,
                                const char *wanted,
                                w5500_dns_addr_t *addrs,
                                uint8_t max_addrs,
                                uint8_t *count);
/**
 * @brief Parse a DNS response and extract the first A record.
 *
 * Thin wrapper around w5500_dns_parse_response_ex() kept for existing users.
 * On failure @p ip_out is untouched.
 */
int w5500_dns_parse_response(const uint8_t *pkt,
                             uint16_t len,
                             uint16_t qid,
                             uint8_t ip_out[4]);
#endif /* W5500_DNS_INTERNAL_H */
