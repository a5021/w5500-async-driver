/**
 * @file fuzz_dhcp_state.c
 * @brief Fuzzes the DHCP client state machine, not just the packet codec.
 *
 * A real client instance runs on the emulator; the script injects replies
 * (with the transaction id patched in), advances the clock, stops and
 * restarts the client. Properties:
 *   1. No crash/OOB and no lock imbalance (ASan + fuzz_check_locks).
 *   2. A BOUND/RENEWED event implies a non-zero lease was applied to the
 *      chip (SIPR).
 *   3. FAILED implies the socket was closed.
 *   4. Polling a stopped client returns W5500_ERR_STATE.
 *   5. Callbacks only ever carry one of the three defined events.
 *
 * Input layout (> FUZZ_MAX_OPS cap, bounds checked):
 *   [0]   flags: bit0 = burst platform path
 *   [..]  script:
 *           0 INJECT  2 bytes little-endian len (<= 600), payload; xid patched in
 *           1 TIME    2 bytes little-endian ms delta
 *           2 STOP    (0 args)
 *           3 START   (0 args)
 */
#include <stdlib.h>
#include <string.h>

#include "fuzz_common.h"
#include "w5500_dhcp.h"

static unsigned g_events;
static w5500_dhcp_event_t g_last_event;
static w5500_handle_t g_chip;

static void dhcp_cb(w5500_dhcp_t *dhcp, w5500_dhcp_event_t event, void *arg)
{
    uint8_t ip[4];

    (void)arg;
    if (event != W5500_DHCP_EVENT_BOUND && event != W5500_DHCP_EVENT_RENEWED &&
        event != W5500_DHCP_EVENT_FAILED) {
        abort();
    }
    switch (event) {
    case W5500_DHCP_EVENT_BOUND:
    case W5500_DHCP_EVENT_RENEWED:
        if (w5500_dhcp_get_config(dhcp, ip, NULL, NULL, NULL) != W5500_OK ||
            (ip[0] | ip[1] | ip[2] | ip[3]) == 0u) {
            abort(); /* no lease applied */
        }
        if (w5500_get_ip(g_chip, ip) != W5500_OK ||
            (ip[0] | ip[1] | ip[2] | ip[3]) == 0u) {
            abort(); /* chip not reconfigured before the callback */
        }
        break;
    case W5500_DHCP_EVENT_FAILED:
        if (g_mock.sreg[0][W5500_Sn_SR] != W5500_SOCK_CLOSED) {
            abort(); /* FAILED must leave the socket closed */
        }
        break;
    default:
        abort();
    }
    g_events++;
    g_last_event = event;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t mem[4096];
    static uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    w5500_dhcp_t *d = NULL;
    fuzz_lock_snap_t snap;
    uint32_t now = 0u;
    size_t p = 1u;
    unsigned ops = 0u;

    if (size < 1u) {
        return 0;
    }

    g_chip = fuzz_open_handle(data[0] & 1u);
    g_events = 0u;
    g_last_event = W5500_DHCP_EVENT_FAILED;

    if (w5500_dhcp_init(&d, mem, sizeof(mem), g_chip, mac, dhcp_cb, NULL) !=
        W5500_OK) {
        abort();
    }
    if (w5500_dhcp_start(d, now) != W5500_OK) {
        abort();
    }
    snap = fuzz_lock_snap();

    while (p < size && ops < FUZZ_MAX_OPS) {
        uint8_t op = data[p++];

        ops++;
        switch (op & 0x03u) {
        case 0u: { /* INJECT */
            uint8_t pkt[600];
            uint16_t len;

            if (p + 2u > size) {
                goto done;
            }
            len = (uint16_t)((uint16_t)data[p] |
                             ((uint16_t)data[p + 1u] << 8));
            p += 2u;
            if (len > sizeof(pkt)) {
                len = (uint16_t)sizeof(pkt);
            }
            if ((size_t)len > size - p) {
                len = (uint16_t)(size - p);
            }
            if (len > 0u) {
                memcpy(pkt, data + p, len);
            }
            p += len;
            /* Patch the transaction id so replies can actually match. */
            if (len >= 8u && g_mock.last_sent_len[0] >= 8u) {
                memcpy(pkt + 4, g_mock.last_sent[0] + 4, 4u);
            }
            mock_w5500_inject_rx(0, pkt, len);
            break;
        }
        case 1u: { /* TIME */
            uint32_t delta;

            if (p + 2u > size) {
                goto done;
            }
            delta = (uint32_t)data[p] | ((uint32_t)data[p + 1u] << 8);
            p += 2u;
            now += delta;
            break;
        }
        case 2u: /* STOP */
            w5500_dhcp_stop(d);
            if (w5500_dhcp_poll(d, now) != W5500_ERR_STATE) {
                abort(); /* a stopped client reports ERR_STATE */
            }
            break;
        case 3u: /* START */
            (void)w5500_dhcp_start(d, now);
            break;
        default:
            abort();
        }

        /* Each step is followed by the normal service call. */
        {
            w5500_result_t rc = w5500_dhcp_poll(d, now);
            if (rc != W5500_OK && rc != W5500_ERR_STATE) {
                abort();
            }
        }
        (void)w5500_poll(g_chip); /* clear SEND_DONE for the next transmit */
    }

done:
    if (g_events > (FUZZ_MAX_OPS + 2u)) {
        abort(); /* a callback per step is the absolute ceiling */
    }
    (void)g_last_event;
    w5500_dhcp_stop(d);
    fuzz_check_locks(snap);
    w5500_deinit(g_chip);
    return 0;
}
