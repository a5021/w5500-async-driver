/**
 * @file fuzz_spi_frame.c
 * @brief Fuzzes the SPI wire format: the w5500_ctrl_byte algebra, the mock's
 *        frame decoder state machine, and full write/read frame round-trips
 *        (manual byte driving plus driver-level ll_write/ll_read, byte-loop
 *        and burst paths).
 *
 * Input layout (fixed header, size >= 7):
 *   [0]    flags: bit0 = burst path for the handle under test
 *   [1..2] fuzzed frame address (big endian)
 *   [3]    fuzzed raw BSB byte (driver must mask to 5 bits)
 *   [4]    fuzzed raw RWB byte (driver must mask to 1 bit)
 *   [5]    common-block offset selector (offset = 1 + %63, i.e. 1..63;
 *          MR at 0 is excluded so the write cannot trigger a chip reset)
 *   [6]    length selector (n = 1 + %15, clamped to fit) and part-B data byte
 *   [7..]  payload bytes (padded with 0x5A when short)
 *
 * Properties:
 *   1. ctrl = (bsb & 0x1F) << 3 | (rwb & 1) << 2 | OM(VDM=0):
 *      exact bit layout, address-independent, OM bits always zero.
 *   2. Frame decoder: select resets phase; addr bytes land in g_mock.addr;
 *      the control byte lands in bsb/rwb; each data byte advances addr by
 *      one; deselect resets the machine; idle-bus bytes read 0xFF.
 *   3. Round-trips: a manual write frame deposits exactly the payload into
 *      the raw register image, a manual read frame echoes it back, and
 *      w5500_ll_write()/w5500_ll_read() reproduce the same bytes.
 */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    w5500_handle_t h;
    w5500_plat_if_t plat;
    fuzz_lock_snap_t snap;
    uint16_t addr;
    uint8_t bsb_raw;
    uint8_t rwb_raw;
    uint8_t ctrl;
    unsigned off;
    unsigned n;
    unsigned i;
    size_t avail;
    uint8_t payload[15];
    uint8_t got[15];
    uint8_t r;

    if (size < 7u) {
        return 0;
    }

    h = fuzz_open_handle(data[0] & 1u);
    mock_w5500_plat(&plat); /* byte hooks for manual frame driving */
    snap = fuzz_lock_snap();

    addr = (uint16_t)(((uint16_t)data[1] << 8) | data[2]);
    bsb_raw = data[3];
    rwb_raw = data[4];

    /* --- (a) ctrl-byte algebra ------------------------------------- */
    ctrl = w5500_ctrl_byte(addr, bsb_raw, rwb_raw);
    if (ctrl !=
        (uint8_t)(((bsb_raw & 0x1Fu) << 3) | ((rwb_raw & 1u) << 2))) {
        abort(); /* exact layout: masked BSB, masked RWB, OM = 0 */
    }
    if ((ctrl & 0x03u) != 0u) {
        abort(); /* variable-length data mode is 0b00 */
    }
    if (w5500_ctrl_byte((uint16_t)(addr ^ 0xFFFFu), bsb_raw, rwb_raw) !=
        ctrl) {
        abort(); /* control byte must not depend on the address */
    }
    if (((ctrl >> 3) & 0x1Fu) != (bsb_raw & 0x1Fu) ||
        ((ctrl >> 2) & 0x01u) != (rwb_raw & 0x01u)) {
        abort();
    }

    /* --- (b) frame decoder state machine --------------------------- */
    if (plat.spi_transfer_byte(0xAAu) != 0xFFu) {
        abort(); /* idle bus (CS high) reads 0xFF */
    }
    plat.spi_select();
    if (g_mock.phase != 0u || g_mock.in_cs != 1u) {
        abort(); /* select arms the address phase */
    }
    r = plat.spi_transfer_byte((uint8_t)(addr >> 8));
    if (r != 0u || g_mock.phase != 1u) {
        abort();
    }
    r = plat.spi_transfer_byte((uint8_t)(addr & 0xFFu));
    if (r != 0u || g_mock.phase != 2u || g_mock.addr != addr) {
        abort();
    }
    {
        int is_write = ((rwb_raw & 1u) == 0u);
        uint8_t bsb_eff = bsb_raw;
        uint16_t start = addr;

        /* The mock maps common addresses with & 0x3F, so a write at any
           addr with addr & 0x3F == 0 and MR.RST set would reset the chip
           model mid-frame and wipe the decoder state we assert on. */
        if (is_write && (bsb_eff & 0x1Fu) == 0u &&
            (start & 0x3Fu) == 0u && (data[6] & W5500_MR_RST) != 0u) {
            is_write = 0;
        }
        ctrl = w5500_ctrl_byte(start, bsb_eff, is_write ? 0u : 1u);
        r = plat.spi_transfer_byte(ctrl);
        if (r != 0u || g_mock.phase != 3u) {
            abort();
        }
        if (g_mock.bsb != (uint8_t)((ctrl >> 3) & 0x1Fu) ||
            g_mock.rwb != (uint8_t)((ctrl >> 2) & 0x01u)) {
            abort(); /* decoder extracts BSB/RWB exactly as encoded */
        }
        r = plat.spi_transfer_byte(data[6]);
        if (is_write && r != 0u) {
            abort(); /* write data phase returns 0 */
        }
        if (g_mock.addr != (uint16_t)(start + 1u)) {
            abort(); /* every data byte advances the address */
        }
    }
    plat.spi_deselect();
    if (g_mock.phase != 0u || g_mock.in_cs != 0u) {
        abort(); /* deselect resets the machine */
    }

    /* --- (c) full common-block frames ------------------------------ */
    off = 1u + (unsigned)(data[5] % 63u); /* 1..63: never MR (offset 0) */
    n = 1u + (unsigned)(data[6] % 15u);
    if (n > 0x40u - off) {
        n = 0x40u - off; /* stay inside the 64-byte common block */
    }
    avail = (size > 7u) ? (size - 7u) : 0u;
    for (i = 0; i < n; i++) {
        payload[i] = (i < avail) ? data[7u + i] : 0x5Au;
    }

    plat.spi_select();
    (void)plat.spi_transfer_byte((uint8_t)(off >> 8));
    (void)plat.spi_transfer_byte((uint8_t)(off & 0xFFu));
    (void)plat.spi_transfer_byte(
        w5500_ctrl_byte((uint16_t)off, W5500_BSB_COMMON,
                        W5500_SPI_RWB_WRITE));
    for (i = 0; i < n; i++) {
        if (plat.spi_transfer_byte(payload[i]) != 0u) {
            abort();
        }
    }
    plat.spi_deselect();
    if (memcmp(&g_mock.common[off], payload, n) != 0) {
        abort(); /* write frame landed byte-for-byte */
    }

    plat.spi_select();
    (void)plat.spi_transfer_byte((uint8_t)(off >> 8));
    (void)plat.spi_transfer_byte((uint8_t)(off & 0xFFu));
    (void)plat.spi_transfer_byte(
        w5500_ctrl_byte((uint16_t)off, W5500_BSB_COMMON,
                        W5500_SPI_RWB_READ));
    for (i = 0; i < n; i++) {
        got[i] = plat.spi_transfer_byte(0x00u);
    }
    plat.spi_deselect();
    if (memcmp(got, payload, n) != 0) {
        abort(); /* read frame echoes the written bytes */
    }

    /* Driver-level round trip (byte loop or burst, depending on flags). */
    for (i = 0; i < n; i++) {
        payload[i] = (uint8_t)(payload[i] ^ 0xFFu);
    }
    if (w5500_ll_write(h, (uint16_t)off, W5500_BSB_COMMON, payload,
                       (uint16_t)n) != W5500_OK ||
        memcmp(&g_mock.common[off], payload, n) != 0) {
        abort();
    }
    memset(got, 0, sizeof(got));
    if (w5500_ll_read(h, (uint16_t)off, W5500_BSB_COMMON, got,
                      (uint16_t)n) != W5500_OK ||
        memcmp(got, payload, n) != 0) {
        abort();
    }

    fuzz_check_locks(snap);
    w5500_deinit(h);
    return 0;
}
