/**
 * @file fuzz_reg_access.c
 * @brief Fuzzes w5500_read_regs()/w5500_write_regs() with arbitrary
 *        addr/bsb/len against the chip emulator.
 *
 * Input layout:
 *   [0]   addr high byte
 *   [1]   addr low byte
 *   [2]   bsb (block select byte, masked to 5 bits on the wire)
 *   [3]   len code   -> len = data[3] % 65   (0..64)
 *   [4]   flags: bit0 = burst platform path, bit1 = write before read,
 *                bit2 = cr_stuck mock knob
 *   [5..] payload (padded with 0xA5 when shorter than len)
 *
 * Properties:
 *   1. Result codes always belong to the documented set; len == 0 is
 *      always W5500_ERR_PARAM; a successful read of a present block
 *      always returns W5500_OK.
 *   2. Write/read roundtrip, with the emulator's known exceptions:
 *        - common block: plain registers echo, but MR (offset 0) is a
 *          side-effecting reset register and offsets crossing 0x40 wrap;
 *        - socket register block: Sn_CR executes a command, Sn_IR is
 *          write-1-to-clear, Sn_TX_FSR/Sn_RX_RSR are computed reads;
 *        - block-select values >= 16 and kind-0 blocks (bsb%4 == 0)
 *          have no backing store and read as all zeros.
 *   3. Lock balance is preserved.
 */
#include "fuzz_common.h"

/* Roundtrip expectation for one write/read pair at (addr, wire_bsb, len):
 *   1 => bytes must read back exactly as written
 *   0 => the region reads as all zeros regardless of what was written
 *  -1 => unspecified (skip the comparison)                                    */
static int roundtrip_expect(uint16_t addr, uint8_t wire_bsb, uint16_t len)
{
    if (wire_bsb == 0u) { /* common register block */
        uint8_t off = (uint8_t)(addr & 0x3Fu);
        if (off == 0u) {
            return -1; /* MR: writing MR.RST resets the whole chip model */
        }
        if ((uint16_t)(off + len) > 0x40u) {
            return -1; /* address wraps inside the 64-byte block */
        }
        return 1;
    }
    if (wire_bsb >= 16u) {
        return 0; /* no such block on the chip */
    }
    if ((wire_bsb & 3u) == 0u) {
        return 0; /* kind 0: no backing store in the emulator */
    }
    if ((wire_bsb & 3u) == 1u) { /* socket registers */
        uint16_t i;
        for (i = 0; i < len; i++) {
            uint8_t off = (uint8_t)((addr + i) & 0xFFu);
            if (off == 0x01u || off == 0x02u ||   /* Sn_CR / Sn_IR */
                off == 0x20u || off == 0x21u ||   /* Sn_TX_FSR (computed) */
                off == 0x26u || off == 0x27u) {   /* Sn_RX_RSR (computed) */
                return -1;
            }
        }
        return 1;
    }
    return 1; /* TX/RX buffer blocks: the address mask is deterministic */
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t wbuf[64];
    uint8_t rbuf[64];
    uint16_t addr;
    uint16_t len;
    uint8_t bsb;
    uint8_t wire_bsb;
    int burst;
    int do_write;
    int expect;
    w5500_handle_t h;
    fuzz_lock_snap_t snap;
    w5500_result_t rc;

    if (size < 5u) {
        return 0;
    }

    addr = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    bsb = data[2];
    len = (uint16_t)(data[3] % 65u);
    burst = (data[4] & 0x01) != 0;
    do_write = (data[4] & 0x02) != 0;

    h = fuzz_open_handle(burst);
    g_mock.cr_stuck = (uint8_t)((data[4] >> 2) & 0x01u);
    snap = fuzz_lock_snap();

    /* Build the write payload: input bytes first, then a fixed pad so the
       buffer always covers the full len without reading past `data`. */
    memset(wbuf, 0xA5, sizeof(wbuf));
    if (size > 5u) {
        size_t n = size - 5u;
        if (n > len) {
            n = len;
        }
        if (n > sizeof(wbuf)) {
            n = sizeof(wbuf);
        }
        memcpy(wbuf, data + 5, n);
    }
    memset(rbuf, 0xCD, sizeof(rbuf));

    if (do_write != 0) {
        rc = w5500_write_regs(h, addr, bsb, wbuf, len);
        fuzz_check_rc(rc);
        if (len == 0u) {
            if (rc != W5500_ERR_PARAM) {
                abort();
            }
        }
    }

    rc = w5500_read_regs(h, addr, bsb, rbuf, len);
    fuzz_check_rc(rc);
    if (len == 0u) {
        if (rc != W5500_ERR_PARAM) {
            abort();
        }
    } else if (rc != W5500_OK) {
        abort(); /* a read on a valid handle can only fail on len == 0 */
    }

    if (len > 0u) {
        wire_bsb = (uint8_t)(bsb & 0x1Fu);
        expect = roundtrip_expect(addr, wire_bsb, len);
        if (do_write == 0 && expect == 1) {
            /* Without a prior write only the constant-zero regions are
               predictable; plain memory holds whatever was there. */
            expect = -1;
        }
        if (expect == 1) {
            if (memcmp(wbuf, rbuf, len) != 0) {
                abort();
            }
        } else if (expect == 0) {
            uint16_t i;
            for (i = 0; i < len; i++) {
                if (rbuf[i] != 0u) {
                    abort();
                }
            }
        }
    }

    fuzz_check_locks(snap);
    w5500_deinit(h);
    return 0;
}
