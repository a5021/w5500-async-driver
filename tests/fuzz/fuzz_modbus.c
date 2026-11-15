/**
 * @file fuzz_modbus.c
 * @brief Fuzzes the Modbus TCP ADU codec and the reply builders.
 *
 * Properties:
 *   1. No out-of-bounds access for any input (ASan).
 *   2. A successful parse consumed no more bytes than were supplied and the
 *      declared length fields agree with the frame layout.
 *   3. Function-code specific bounds hold (count ranges, byte-count ==
 *      2 * register count for FC16).
 *   4. Parsing is deterministic, and the builders produce self-consistent
 *      MBAP headers (protocol id 0, length field matching the payload).
 */
#include <stdlib.h>
#include <string.h>

#include "w5500_modbus_internal.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    w5500_mb_adu_t a;
    w5500_mb_adu_t b;
    int rc;

    if (size > 0xFFFFu) {
        return 0;
    }

    memset(&a, 0x5A, sizeof(a));
    rc = w5500_mb_parse_adu(data, (uint16_t)size, &a);
    if (rc != 0 && rc != -1) {
        abort();
    }
    if (rc == 0) {
        uint16_t mbap_len;

        if (a.frame_len < 8u || a.frame_len > (uint16_t)size) {
            abort();
        }
        mbap_len = rd16(data + 4);
        if (a.frame_len != (uint16_t)(6u + mbap_len)) {
            abort();
        }
        if (rd16(data + 2) != 0u) {
            abort(); /* protocol id must be 0 */
        }
        switch (a.fc) {
        case W5500_MB_FC_READ_HOLDING:
        case W5500_MB_FC_READ_INPUT:
            if (a.count < 1u || a.count > 125u || a.frame_len != 12u) {
                abort();
            }
            break;
        case W5500_MB_FC_WRITE_SINGLE:
            if (a.frame_len != 12u) {
                abort();
            }
            break;
        case W5500_MB_FC_WRITE_MULTIPLE:
            if (a.count < 1u || a.count > 123u ||
                a.data_len != (uint16_t)(2u * a.count) ||
                a.frame_len != (uint16_t)(13u + 2u * a.count) ||
                a.data == NULL) {
                abort();
            }
            break;
        default:
            break;
        }
    }

    /* Deterministic decode. */
    memset(&b, 0, sizeof(b));
    if (w5500_mb_parse_adu(data, (uint16_t)size, &b) != rc) {
        abort();
    }
    if (rc == 0 && memcmp(&a, &b, sizeof(a)) != 0) {
        abort();
    }
    if (w5500_mb_parse_adu(data, (uint16_t)size, NULL) != -1) {
        abort();
    }

    /* Response parser: same buffer, txid taken from the input. */
    {
        uint16_t txid = (size >= 2u) ? rd16(data) : 0x1234u;
        w5500_mb_adu_t r1;
        w5500_mb_adu_t r2;
        int rrc;

        memset(&r1, 0x5A, sizeof(r1));
        rrc = w5500_mb_parse_response(data, (uint16_t)size, txid, &r1);
        if (rrc != 0 && rrc != -1) {
            abort();
        }
        memset(&r2, 0, sizeof(r2));
        if (w5500_mb_parse_response(data, (uint16_t)size, txid, &r2) != rrc) {
            abort();
        }
        if (rrc == 0) {
            if (r1.frame_len > (uint16_t)size || r1.txid != txid) {
                abort();
            }
            if ((r1.fc & 0x80u) == 0u &&
                (r1.fc == W5500_MB_FC_READ_HOLDING ||
                 r1.fc == W5500_MB_FC_READ_INPUT)) {
                if (r1.data == NULL ||
                    r1.data_len != (uint16_t)(2u * r1.count)) {
                    abort();
                }
            }
            if (w5500_mb_parse_response(data, (uint16_t)size,
                                        (uint16_t)(txid ^ 0xFFFFu), &r2) ==
                0) {
                abort(); /* a mismatched transaction id is rejected */
            }
        }
        if (w5500_mb_parse_response(data, (uint16_t)size, txid, NULL) != -1) {
            abort();
        }
    }

    /* Builders: self-consistent MBAP headers. */
    {
        uint8_t out[W5500_MB_FRAME_MAX];
        static const uint16_t regs[3] = {1u, 2u, 0xABCDu};
        uint16_t n;

        n = w5500_mb_build_exception(out, sizeof(out), 0x1234u, 7u, 3u, 2u);
        if (n != 9u || rd16(out) != 0x1234u || rd16(out + 2) != 0u ||
            rd16(out + 4) != 3u || out[6] != 7u || out[7] != 0x83u ||
            out[8] != 2u) {
            abort();
        }
        if (w5500_mb_build_exception(out, 8u, 1u, 1u, 1u, 1u) != 0u) {
            abort();
        }

        n = w5500_mb_build_read_response(out, sizeof(out), 1u, 1u,
                                         W5500_MB_FC_READ_HOLDING, regs, 3u);
        if (n != 15u || rd16(out + 4) != 9u || out[8] != 6u ||
            rd16(out + 9) != 1u || rd16(out + 13) != 0xABCDu) {
            abort();
        }
        if (w5500_mb_build_read_response(out, 10u, 1u, 1u, 3u, regs, 3u) !=
            0u) {
            abort();
        }

        n = w5500_mb_build_write_response(out, sizeof(out), 2u, 1u,
                                          W5500_MB_FC_WRITE_SINGLE, 0x10u,
                                          0xBEEFu);
        if (n != 12u || rd16(out + 4) != 6u || rd16(out + 8) != 0x10u ||
            rd16(out + 10) != 0xBEEFu) {
            abort();
        }
    }
    return 0;
}
