/**
 * @file fuzz_dns.c
 * @brief Fuzzes the DNS response decoder with arbitrary bytes.
 *
 * Properties:
 *   1. No out-of-bounds access for any input (ASan), compression-pointer
 *      loops included.
 *   2. On failure the output address is untouched; on success it is a
 *      deterministic copy of the first A record.
 *   3. A response is only accepted when the transaction id matches.
 *   4. Inputs shorter than the 12-byte header never parse.
 */
#include <stdlib.h>
#include <string.h>

#include "w5500_dns_internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t ip[4];
    uint8_t ip2[4];
    uint8_t ip3[4];
    uint16_t qid = 0x1234u;
    uint16_t other;
    int rc;
    unsigned i;

    if (size > 0xFFFFu) {
        return 0;
    }
    if (size >= 2u) {
        qid = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    }

    memset(ip, 0xEE, sizeof(ip));
    rc = w5500_dns_parse_response(data, (uint16_t)size, qid, ip);
    if (rc != 0 && rc != -1) {
        abort();
    }
    if (rc != 0) {
        for (i = 0; i < sizeof(ip); i++) {
            if (ip[i] != 0xEEu) {
                abort(); /* failed parse must not write the result */
            }
        }
    }
    if (size < 12u && rc == 0) {
        abort();
    }

    /* Deterministic decode. */
    memset(ip2, 0xEE, sizeof(ip2));
    if (w5500_dns_parse_response(data, (uint16_t)size, qid, ip2) != rc) {
        abort();
    }
    if (rc == 0 && memcmp(ip, ip2, sizeof(ip)) != 0) {
        abort();
    }

    /* A mismatched transaction id is always rejected. */
    other = (uint16_t)(qid ^ 0xFFFFu);
    memset(ip3, 0, sizeof(ip3));
    if (w5500_dns_parse_response(data, (uint16_t)size, other, ip3) == 0) {
        abort();
    }

    if (w5500_dns_parse_response(data, (uint16_t)size, qid, NULL) != -1) {
        abort();
    }
    return 0;
}
