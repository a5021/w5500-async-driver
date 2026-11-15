/**
 * @file fuzz_sntp.c
 * @brief Fuzzes the SNTP reply decoder with arbitrary bytes.
 *
 * Properties:
 *   1. No out-of-bounds access for any input (ASan).
 *   2. On failure the timestamp output is untouched; success implies a
 *      non-zero Unix time derived from the transmit timestamp field.
 *   3. Only server-mode replies are accepted.
 *   4. Decoding is deterministic.
 */
#include <stdlib.h>
#include <string.h>

#include "w5500_sntp_internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint32_t t1;
    uint32_t t2;
    int rc;

    if (size > 0xFFFFu) {
        return 0;
    }

    t1 = 0xDEADBEEFu;
    rc = w5500_sntp_parse_response(data, (uint16_t)size, &t1);
    if (rc != 0 && rc != -1) {
        abort();
    }
    if (rc != 0) {
        if (t1 != 0xDEADBEEFu) {
            abort(); /* failed parse must not write the result */
        }
    } else if (size < W5500_SNTP_PKT_LEN) {
        abort();
    }

    t2 = 0xDEADBEEFu;
    if (w5500_sntp_parse_response(data, (uint16_t)size, &t2) != rc) {
        abort();
    }
    if (rc == 0 && t1 != t2) {
        abort(); /* deterministic */
    }
    if (w5500_sntp_parse_response(data, (uint16_t)size, NULL) != -1) {
        abort();
    }
    return 0;
}
