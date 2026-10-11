/**
 * @file fuzz_net_config.c
 * @brief Fuzzes the network-configuration surface with cross-properties:
 *        raw register image vs high-level API, setter/getter round-trips,
 *        the SETBUF validity matrix, timeout/link/reset semantics and the
 *        full NULL/zero argument matrix.
 *
 * Input layout:
 *   [0]    flags: bit0 = burst path
 *   [..]   opcode script (<= FUZZ_MAX_OPS, bounds checked):
 *            0 SET_MAC     6 bytes
 *            1 SET_IP      4 bytes
 *            2 SET_MASK    4 bytes
 *            3 SET_GW      4 bytes
 *            4 SETBUF      1 byte (candidate KB size)
 *            5 SET_TIMEOUT 4 bytes (little-endian ms)
 *            6 LINK        1 byte (raw PHYCFGR poke)
 *            7 CHECK       (0 args)
 *            8 SW_RESET    (0 args)
 *            9 NEGATIVE probes: NULL/zero-length arguments (0 args)
 *
 * Properties:
 *   1. set_mac/ip/mask/gw: rc == OK, raw common[off..off+n) equals the
 *      input byte-for-byte, every other common byte is untouched, and the
 *      matching getter echoes the same bytes.
 *   2. SETBUF: rc == OK  <=>  kb in {1,2,4} (8/16 exceed the 16 KB sum,
 *      everything else is not a chip buffer size). On OK every socket's
 *      TX/RXBUF_SIZE registers equal kb, only those 8 bytes of sreg may
 *      change, and all driver socket slots are invalidated. On failure
 *      nothing moves at all.
 *   3. SET_TIMEOUT: 0 -> ERR_PARAM and the previous value is preserved,
 *      anything else -> OK and timeout_ms matches.
 *   4. LINK: a raw PHYCFGR write is observed by get_link_status with
 *      exactly bit 0 semantics.
 *   5. CHECK reads VERSIONR == 4; SW_RESET clears the network registers
 *      back to post-reset defaults.
 *   6. Every NULL/zero-arity probe returns ERR_PARAM without touching
 *      the register image.
 */
#include "fuzz_common.h"

#define NEED(n)                                                              \
    do {                                                                     \
        if (p + (size_t)(n) > size) {                                        \
            goto done;                                                       \
        }                                                                    \
    } while (0)

static void noop_cb(w5500_socket_t sock, w5500_event_t event,
                    w5500_result_t status, void *user_arg)
{
    (void)sock;
    (void)event;
    (void)status;
    (void)user_arg;
}

/** Cross-check a successful net-setter: raw image + non-interference + readback. */
static void check_set(w5500_handle_t h, w5500_result_t rc, uint16_t off,
                      uint16_t len, const uint8_t *val,
                      w5500_result_t (*getter)(w5500_handle_t, uint8_t *),
                      const uint8_t snap[0x40])
{
    uint8_t back[6];
    unsigned k;

    fuzz_check_rc(rc);
    if (rc != W5500_OK) {
        abort(); /* mock register writes cannot fail */
    }
    for (k = 0; k < 0x40u; k++) {
        if (k >= (unsigned)off && k < (unsigned)off + len) {
            if (g_mock.common[k] != val[k - off]) {
                abort(); /* written bytes must match the input exactly */
            }
        } else if (g_mock.common[k] != snap[k]) {
            abort(); /* non-interference: only [off, off+len) moved */
        }
    }
    memset(back, 0, sizeof(back));
    if (getter(h, back) != W5500_OK || memcmp(back, val, len) != 0) {
        abort(); /* getter round-trip echoes what was written */
    }
}

typedef struct {
    uint8_t            in_use;
    w5500_sock_state_t state;
    w5500_event_cb_t   cb;
    void              *user_arg;
} slot_snap_t;

static void take_slot_snap(w5500_handle_t h, slot_snap_t out[4])
{
    unsigned i;
    for (i = 0; i < 4u; i++) {
        out[i].in_use = h->sockets[i].in_use;
        out[i].state = h->sockets[i].state;
        out[i].cb = h->sockets[i].cb;
        out[i].user_arg = h->sockets[i].user_arg;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    w5500_handle_t h;
    w5500_socket_t fixture = NULL;
    fuzz_lock_snap_t snap;
    size_t p = 1u;
    unsigned ops = 0u;
    uint8_t snap_common[0x40];
    uint8_t snap_sreg[4][0x30];
    slot_snap_t slot_snapshot[4];

    if (size < 1u) {
        return 0;
    }

    h = fuzz_open_handle(data[0] & 1u);

    if (w5500_socket_open(h, W5500_PROTO_TCP, 0x1F90u, noop_cb, NULL,
                          &fixture) != W5500_OK ||
        fixture == NULL || fixture->id != 0u) {
        abort(); /* clean fixture: first slot must open */
    }
    snap = fuzz_lock_snap();

    while (p < size && ops < FUZZ_MAX_OPS) {
        uint8_t op = data[p++];
        ops++;

        switch (op % 10u) {
        case 0u: { /* SET_MAC */
            w5500_result_t rc;
            NEED(6);
            memcpy(snap_common, g_mock.common, 0x40);
            rc = w5500_set_mac(h, data + p);
            check_set(h, rc, W5500_REG_SHAR, 6u, data + p, w5500_get_mac,
                      snap_common);
            p += 6u;
            break;
        }
        case 1u: { /* SET_IP */
            w5500_result_t rc;
            NEED(4);
            memcpy(snap_common, g_mock.common, 0x40);
            rc = w5500_set_ip(h, data + p);
            check_set(h, rc, W5500_REG_SIPR, 4u, data + p, w5500_get_ip,
                      snap_common);
            p += 4u;
            break;
        }
        case 2u: { /* SET_MASK */
            w5500_result_t rc;
            NEED(4);
            memcpy(snap_common, g_mock.common, 0x40);
            rc = w5500_set_subnet_mask(h, data + p);
            check_set(h, rc, W5500_REG_SUBR, 4u, data + p,
                      w5500_get_subnet_mask, snap_common);
            p += 4u;
            break;
        }
        case 3u: { /* SET_GW */
            w5500_result_t rc;
            NEED(4);
            memcpy(snap_common, g_mock.common, 0x40);
            rc = w5500_set_gateway(h, data + p);
            check_set(h, rc, W5500_REG_GAR, 4u, data + p, w5500_get_gateway,
                      snap_common);
            p += 4u;
            break;
        }
        case 4u: { /* SETBUF validity matrix */
            uint8_t kb;
            int expect_ok;
            w5500_result_t rc;
            unsigned i;
            unsigned k;

            NEED(1);
            kb = data[p++];
            expect_ok = (kb == 1u || kb == 2u || kb == 4u);
            memcpy(snap_sreg, g_mock.sreg, sizeof(snap_sreg));
            take_slot_snap(h, slot_snapshot);

            rc = w5500_set_socket_buffer_size(h, kb);
            fuzz_check_rc(rc);
            if (((rc == W5500_OK) ? 1 : 0) != expect_ok) {
                abort(); /* matrix: OK <=> kb in {1,2,4} */
            }

            if (rc == W5500_OK) {
                for (i = 0; i < 4u; i++) {
                    if (g_mock.sreg[i][W5500_Sn_TXBUF_SIZE] != kb ||
                        g_mock.sreg[i][W5500_Sn_RXBUF_SIZE] != kb) {
                        abort();
                    }
                    if (h->sockets[i].in_use != 0u ||
                        h->sockets[i].state != W5500_SSR_FREE ||
                        h->sockets[i].cb != NULL ||
                        h->sockets[i].user_arg != NULL) {
                        abort(); /* buffer change invalidates open handles */
                    }
                }
                for (i = 0; i < 4u; i++) {
                    for (k = 0; k < 0x30u; k++) {
                        int is_buf_size = (k == W5500_Sn_TXBUF_SIZE ||
                                           k == W5500_Sn_RXBUF_SIZE);
                        if (!is_buf_size &&
                            g_mock.sreg[i][k] != snap_sreg[i][k]) {
                            abort(); /* only the two size bytes may move */
                        }
                    }
                }
            } else {
                if (memcmp(g_mock.sreg, snap_sreg, sizeof(snap_sreg)) != 0) {
                    abort(); /* rejected request must not write anything */
                }
                for (i = 0; i < 4u; i++) {
                    if (h->sockets[i].in_use != slot_snapshot[i].in_use ||
                        h->sockets[i].state != slot_snapshot[i].state ||
                        h->sockets[i].cb != slot_snapshot[i].cb ||
                        h->sockets[i].user_arg !=
                            slot_snapshot[i].user_arg) {
                        abort();
                    }
                }
            }
            break;
        }
        case 5u: { /* SET_TIMEOUT */
            uint32_t v;
            uint32_t prev;
            w5500_result_t rc;

            NEED(4);
            v = (uint32_t)data[p] | ((uint32_t)data[p + 1u] << 8) |
                ((uint32_t)data[p + 2u] << 16) | ((uint32_t)data[p + 3u] << 24);
            p += 4u;
            prev = h->timeout_ms;
            rc = w5500_set_timeout(h, v);
            fuzz_check_rc(rc);
            if (v == 0u) {
                if (rc != W5500_ERR_PARAM || h->timeout_ms != prev) {
                    abort(); /* 0 is rejected and keeps the previous value */
                }
            } else if (rc != W5500_OK || h->timeout_ms != v) {
                abort();
            }
            break;
        }
        case 6u: { /* LINK: raw PHYCFGR poke vs get_link_status */
            uint8_t b;
            bool link = false;
            w5500_result_t rc;
            unsigned k;

            NEED(1);
            b = data[p++];
            memcpy(snap_common, g_mock.common, 0x40);
            rc = w5500_write_regs(h, W5500_REG_PHYCFGR, W5500_BSB_COMMON, &b,
                                  1u);
            fuzz_check_rc(rc);
            if (rc != W5500_OK) {
                abort();
            }
            for (k = 0; k < 0x40u; k++) {
                uint8_t want = (k == (W5500_REG_PHYCFGR & 0x3Fu))
                                   ? b
                                   : snap_common[k];
                if (g_mock.common[k] != want) {
                    abort();
                }
            }
            rc = w5500_get_link_status(h, &link);
            fuzz_check_rc(rc);
            if (rc != W5500_OK || link != ((b & W5500_PHYCFGR_LNK_ON) != 0u)) {
                abort(); /* link status must mirror PHYCFGR bit 0 */
            }
            break;
        }
        case 7u: { /* CHECK */
            uint8_t ver = 0u;
            w5500_result_t rc = w5500_check(h);
            fuzz_check_rc(rc);
            if (rc != W5500_OK) {
                abort(); /* mock VERSIONR is a healthy 0x04 */
            }
            rc = w5500_get_hw_version(h, &ver);
            if (rc != W5500_OK || ver != W5500_HW_VERSION) {
                abort();
            }
            break;
        }
        case 8u: { /* SW_RESET restores post-reset defaults */
            w5500_result_t rc;
            unsigned k;

            rc = w5500_sw_reset(h);
            fuzz_check_rc(rc);
            if (rc != W5500_OK) {
                abort();
            }
            for (k = 0; k < 6u; k++) {
                if (g_mock.common[(W5500_REG_SHAR & 0x3Fu) + k] != 0u) {
                    abort(); /* SHAR cleared by the chip reset */
                }
            }
            if (g_mock.common[W5500_REG_MR & 0x3Fu] != 0u ||
                g_mock.common[W5500_REG_SIPR & 0x3Fu] != 0u ||
                g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] != 0x01u ||
                g_mock.common[W5500_REG_VERSIONR & 0x3Fu] != 0x04u) {
                abort(); /* defaults restored (link up, VERSIONR = 4) */
            }
            break;
        }
        case 9u: { /* NEGATIVE probes: every call must return ERR_PARAM */
            uint8_t mac[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
            uint8_t ip[4] = { 0u, 0u, 0u, 0u };
            uint8_t ver = 0u;
            uint8_t buf[4] = { 0u, 0u, 0u, 0u };

            memcpy(snap_common, g_mock.common, 0x40);
            if (w5500_set_mac(h, NULL) != W5500_ERR_PARAM ||
                w5500_set_mac(NULL, mac) != W5500_ERR_PARAM ||
                w5500_set_ip(h, NULL) != W5500_ERR_PARAM ||
                w5500_set_ip(NULL, ip) != W5500_ERR_PARAM ||
                w5500_set_subnet_mask(h, NULL) != W5500_ERR_PARAM ||
                w5500_set_subnet_mask(NULL, ip) != W5500_ERR_PARAM ||
                w5500_set_gateway(h, NULL) != W5500_ERR_PARAM ||
                w5500_set_gateway(NULL, ip) != W5500_ERR_PARAM ||
                w5500_get_mac(h, NULL) != W5500_ERR_PARAM ||
                w5500_get_mac(NULL, mac) != W5500_ERR_PARAM ||
                w5500_get_ip(h, NULL) != W5500_ERR_PARAM ||
                w5500_get_ip(NULL, ip) != W5500_ERR_PARAM ||
                w5500_get_subnet_mask(h, NULL) != W5500_ERR_PARAM ||
                w5500_get_gateway(h, NULL) != W5500_ERR_PARAM) {
                abort();
            }
            if (w5500_set_socket_buffer_size(h, 0u) != W5500_ERR_PARAM ||
                w5500_set_socket_buffer_size(h, 3u) != W5500_ERR_PARAM ||
                w5500_set_socket_buffer_size(h, 5u) != W5500_ERR_PARAM ||
                w5500_set_socket_buffer_size(h, 8u) != W5500_ERR_PARAM ||
                w5500_set_socket_buffer_size(h, 16u) != W5500_ERR_PARAM ||
                w5500_set_socket_buffer_size(NULL, 2u) != W5500_ERR_PARAM) {
                abort(); /* matrix negatives: 0, non-pow2, sum overflow */
            }
            if (w5500_set_timeout(h, 0u) != W5500_ERR_PARAM ||
                w5500_set_timeout(NULL, 1u) != W5500_ERR_PARAM ||
                w5500_get_link_status(h, NULL) != W5500_ERR_PARAM ||
                w5500_get_link_status(NULL, NULL) != W5500_ERR_PARAM ||
                w5500_get_hw_version(h, NULL) != W5500_ERR_PARAM ||
                w5500_get_hw_version(NULL, &ver) != W5500_ERR_PARAM ||
                w5500_check(NULL) != W5500_ERR_PARAM ||
                w5500_sw_reset(NULL) != W5500_ERR_PARAM) {
                abort();
            }
            if (w5500_write_regs(h, 0u, W5500_BSB_COMMON, NULL, 1u) !=
                    W5500_ERR_PARAM ||
                w5500_write_regs(h, 0u, W5500_BSB_COMMON, buf, 0u) !=
                    W5500_ERR_PARAM ||
                w5500_read_regs(h, 0u, W5500_BSB_COMMON, NULL, 1u) !=
                    W5500_ERR_PARAM ||
                w5500_read_regs(h, 0u, W5500_BSB_COMMON, buf, 0u) !=
                    W5500_ERR_PARAM ||
                w5500_read_regs(NULL, 0u, W5500_BSB_COMMON, buf, 1u) !=
                    W5500_ERR_PARAM) {
                abort();
            }
            if (memcmp(g_mock.common, snap_common, 0x40) != 0) {
                abort(); /* rejected calls must not touch the register image */
            }
            break;
        }
        default:
            abort();
        }
    }

done:
    fuzz_check_locks(snap);
    w5500_deinit(h);
    return 0;
}
