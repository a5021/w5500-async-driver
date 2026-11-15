/**
 * @file w5500_sntp_internal.h
 * @brief SNTP reply codec shared by the client and the fuzz harness.
 */
#ifndef W5500_SNTP_INTERNAL_H
#define W5500_SNTP_INTERNAL_H

#include <stdint.h>

/** SNTP packet size (RFC 4330, no extensions). */
#define W5500_SNTP_PKT_LEN 48u

/** Seconds between the NTP epoch (1900) and the Unix epoch (1970). */
#define W5500_SNTP_UNIX_DELTA 2208988800u

/**
 * @brief Parse a server reply and convert its transmit timestamp to Unix.
 *
 * Every field is bounds checked; a zero timestamp or a non-server mode is
 * rejected. @p unix_time is written only on success.
 *
 * @return 0 on success, -1 otherwise.
 */
int w5500_sntp_parse_response(const uint8_t *pkt,
                              uint16_t len,
                              uint32_t *unix_time);

#endif /* W5500_SNTP_INTERNAL_H */
