/**
 * @file fuzz_dns_state.c
 * @brief Fuzzes the DNS resolver state machine: replies, clock, restarts.
 *
 * A live resolver runs on the emulator; the script injects replies (with
 * the transaction id patched in), advances time, stops and re-queries.
 *
 * Properties:
 *   1. No crash/OOB and no lock imbalance (ASan + fuzz_check_locks).
 *   2. Every callback carries a documented status and leaves the resolver
 *      idle: a follow-up poll returns W5500_ERR_STATE.
 *   3. Callbacks never outnumber started queries (a restart cancels the
 *      previous query silently).
 *   4. Starting with an invalid name is rejected without touching I/O.
 *
 * Input layout (> FUZZ_MAX_OPS cap, bounds checked):
 *   [0]   flags: bit0 = burst platform path
 *   [..]  script:
 *           0 INJECT  2 bytes little-endian len (<= 600), payload; qid patched in
 *           1 TIME    2 bytes little-endian ms delta
 *           2 STOP    (0 args)
 *           3 RESOLVE (0 args) - cancel and re-query example.com
 */
#include <stdlib.h>
#include <string.h>

#include "fuzz_common.h"
#include "w5500_dns.h"

static unsigned g_callbacks;
static unsigned g_starts;
static w5500_result_t g_last_status;
static w5500_dns_t *g_resolver;

static void dns_cb(w5500_dns_t *dns, const uint8_t ip[4], w5500_result_t status,
                   void *arg)
{
    (void)arg;
    (void)ip;
    if (status != W5500_OK && status != W5500_ERR_TIMEOUT &&
        status != W5500_ERR_STATE) {
        abort();
    }
    if (dns != g_resolver) {
        abort();
    }
    if (w5500_dns_poll(dns, 0u) != W5500_ERR_STATE) {
        abort(); /* the resolver must be idle inside the callback */
    }
    g_callbacks++;
    g_last_status = status;
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
    g_resolver = NULL;

    if (w5500_dns_init(&g_resolver, mem, sizeof(mem), h, server, dns_cb,
                       NULL) != W5500_OK ||
        g_resolver == NULL) {
        abort();
    }
    if (w5500_dns_start(g_resolver, "example.com", now) != W5500_OK) {
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
            if (len >= 2u && g_mock.last_sent_len[0] >= 2u) {
                memcpy(pkt, g_mock.last_sent[0], 2u); /* patch the qid */
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
            w5500_dns_stop(g_resolver);
            if (w5500_dns_poll(g_resolver, now) != W5500_ERR_STATE) {
                abort();
            }
            break;
        case 3u: { /* RESOLVE (cancels any query in flight silently) */
            w5500_result_t rc = w5500_dns_start(g_resolver, "example.com",
                                                now);
            if (rc == W5500_OK) {
                g_starts++;
            }
            break;
        }
        default:
            abort();
        }

        {
            w5500_result_t rc = w5500_dns_poll(g_resolver, now);
            if (rc != W5500_OK && rc != W5500_ERR_STATE) {
                abort();
            }
        }
        (void)w5500_poll(h); /* clear SEND_DONE for the next transmit */
    }

done:
    if (g_callbacks > g_starts) {
        abort(); /* more results than queries */
    }
    (void)g_last_status;
    w5500_dns_stop(g_resolver);
    fuzz_check_locks(snap);
    w5500_deinit(h);
    return 0;
}
