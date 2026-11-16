/**
 * @file fuzz_sntp_state.c
 * @brief Fuzzes the SNTP client state machine: replies, clock, restarts.
 *
 * A live client runs on the emulator; the script injects replies,
 * advances time, stops and re-requests.
 *
 * Properties:
 *   1. No crash/OOB and no lock imbalance (ASan + fuzz_check_locks).
 *   2. Every callback carries a documented status and leaves the client
 *      idle: a follow-up poll returns W5500_ERR_STATE.
 *   3. Callbacks never outnumber started requests.
 *
 * Input layout (> FUZZ_MAX_OPS cap, bounds checked):
 *   [0]   flags: bit0 = burst platform path
 *   [..]  script:
 *           0 INJECT  2 bytes little-endian len (<= 600), payload
 *           1 TIME    2 bytes little-endian ms delta
 *           2 STOP    (0 args)
 *           3 REQUEST (0 args) - cancel and re-request time
 */
#include <stdlib.h>
#include <string.h>

#include "fuzz_common.h"
#include "w5500_sntp.h"

static unsigned g_callbacks;
static unsigned g_starts;
static w5500_result_t g_last_status;
static w5500_sntp_t *g_client;
static uint32_t g_last_time;

static void sntp_cb(w5500_sntp_t *sntp, uint32_t unix_time,
                    w5500_result_t status, void *arg)
{
    (void)arg;
    if (status != W5500_OK && status != W5500_ERR_TIMEOUT &&
        status != W5500_ERR_STATE) {
        abort();
    }
    if (sntp != g_client) {
        abort();
    }
    if (w5500_sntp_poll(sntp, 0u) != W5500_ERR_STATE) {
        abort(); /* client must be idle inside the callback */
    }
    if ((status == W5500_OK && unix_time == 0u) ||
        (status != W5500_OK && unix_time != 0u)) {
        abort(); /* time only valid on success */
    }
    g_callbacks++;
    g_last_status = status;
    g_last_time = unix_time;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t mem[4096];
    static const uint8_t server[4] = {1, 1, 1, 1};
    w5500_handle_t h;
    fuzz_lock_snap_t snap;
    uint32_t now = 0u;
    size_t p = 1u;
    unsigned ops = 0u;

    if (size < 1u) {
        return 0;
    }

    h = fuzz_open_handle(data[0] & 1u);
    g_callbacks = 0u;
    g_starts = 0u;
    g_last_status = W5500_ERR_TIMEOUT;
    g_last_time = 0u;
    g_client = NULL;

    if (w5500_sntp_init(&g_client, mem, sizeof(mem), h, server, sntp_cb,
                        NULL) != W5500_OK ||
        g_client == NULL) {
        abort();
    }
    if (w5500_sntp_start(g_client, now) != W5500_OK) {
        abort();
    }
    g_starts++;
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
            /* The W5500 prepends an 8-byte UDP header in RX memory; the
               emulator's inject_rx models it, so we inject raw payload. */
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
            w5500_sntp_stop(g_client);
            if (w5500_sntp_poll(g_client, now) != W5500_ERR_STATE) {
                abort();
            }
            break;
        case 3u: { /* REQUEST */
            w5500_result_t rc = w5500_sntp_start(g_client, now);
            if (rc == W5500_OK) {
                g_starts++;
            }
            break;
        }
        default:
            abort();
        }
        {
            w5500_result_t rc = w5500_sntp_poll(g_client, now);
            if (rc != W5500_OK && rc != W5500_ERR_STATE) {
                abort();
            }
        }
        (void)w5500_poll(h); /* clear SEND_DONE for the next transmit */
    }

done:
    if (g_callbacks > g_starts) {
        abort();
    }
    (void)g_last_status;
    (void)g_last_time;
    w5500_sntp_stop(g_client);
    fuzz_check_locks(snap);
    w5500_deinit(h);
    return 0;
}
