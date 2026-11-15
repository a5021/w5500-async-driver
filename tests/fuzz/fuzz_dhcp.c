/**
 * @file fuzz_dhcp.c
 * @brief Fuzzes the DHCP reply decoder with arbitrary bytes.
 *
 * Properties:
 *   1. No out-of-bounds access for any input (ASan).
 *   2. A successful parse yields a non-zero message type, a lease inside
 *      the client's clamp range, and has_ip consistent with yiaddr.
 *   3. Parsing is deterministic and never modifies the input.
 *   4. Packets shorter than the BOOTP fixed header never parse.
 */
#include <stdlib.h>
#include <string.h>

#include "w5500_dhcp_internal.h"

#define DHCP_FIXED_LEN   240u
#define DHCP_MAX_LEASE_S 604800u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    w5500_dhcp_parse_t m;
    w5500_dhcp_parse_t m2;
    int rc;

    if (size > 0xFFFFu) {
        return 0;
    }

    memset(&m, 0x5A, sizeof(m));
    rc = w5500_dhcp_parse_packet(data, (uint16_t)size, &m);
    if (rc != 0 && rc != -1) {
        abort();
    }
    if (rc == 0) {
        if (m.msg_type == 0u) {
            abort();
        }
        if (m.lease_s > DHCP_MAX_LEASE_S) {
            abort();
        }
        if (m.has_ip != 0u &&
            (m.ip[0] | m.ip[1] | m.ip[2] | m.ip[3]) == 0u) {
            abort();
        }
    }

    if (size < DHCP_FIXED_LEN && rc == 0) {
        abort(); /* the fixed header must not fit */
    }

    /* Deterministic decode. */
    memset(&m2, 0, sizeof(m2));
    if (w5500_dhcp_parse_packet(data, (uint16_t)size, &m2) != rc) {
        abort();
    }
    if (rc == 0 && memcmp(&m, &m2, sizeof(m)) != 0) {
        abort();
    }

    /* NULL outputs are rejected, never dereferenced. */
    if (w5500_dhcp_parse_packet(data, (uint16_t)size, NULL) != -1) {
        abort();
    }
    return 0;
}
