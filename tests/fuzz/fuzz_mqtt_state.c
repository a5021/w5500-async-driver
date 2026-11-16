/**
 * @file fuzz_mqtt_state.c
 * @brief Fuzzes the MQTT client state machine: replies, traffic, restarts.
 *
 * A live client runs on the emulator; the script injects broker packets,
 * advances time, queues publishes/subscribes, stops and reconnects.
 *
 * Properties:
 *   1. No crash/OOB and no lock imbalance (ASan + fuzz_check_locks).
 *   2. Every poll returns a documented status; every callback event carries
 *      topic/payload only for PUBLISH.
 *   3. Callbacks never arrive for a client that was never started.
 *
 * Input layout (> FUZZ_MAX_OPS cap, bounds checked):
 *   [0]   flags: bit0 = burst platform path
 *   [..]  script:
 *           0 INJECT     2 bytes LE len (<= 600), payload
 *           1 TIME       2 bytes LE ms delta
 *           2 STOP       (0 args)
 *           3 CONNECT    (0 args) - reconnect after stop
 *           4 PUBLISH    up to 4 bytes of payload, then queued
 *           5 SUBSCRIBE  (0 args)
 */
#include <stdlib.h>
#include <string.h>
#include "fuzz_common.h"
#include "w5500_mqtt.h"

static unsigned g_starts;
static w5500_mqtt_t *g_client;

static void mqtt_cb(w5500_mqtt_t *cli, w5500_mqtt_event_t ev,
                    const uint8_t *topic, uint16_t tlen,
                    const uint8_t *payload, uint16_t plen, void *arg)
{
    (void)arg;
    if (cli != g_client || g_starts == 0u) {
        abort();
    }
    switch (ev) {
    case W5500_MQTT_EVENT_CONNECTED:
    case W5500_MQTT_EVENT_SUBACK:
    case W5500_MQTT_EVENT_DISCONNECTED:
    case W5500_MQTT_EVENT_ERROR:
        if (topic != NULL || payload != NULL || tlen != 0u || plen != 0u) {
            abort();
        }
        break;
    case W5500_MQTT_EVENT_PUBLISH:
        if ((tlen > 0u && topic == NULL) || (plen > 0u && payload == NULL)) {
            abort();
        }
        break;
    default:
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t mem[4096];
    static const uint8_t broker[4] = {10, 0, 0, 5};
    w5500_handle_t h;
    fuzz_lock_snap_t snap;
    uint32_t now = 0u;
    size_t p = 1u;
    unsigned ops = 0u;
    w5500_result_t rc;

    if (size < 1u) {
        return 0;
    }
    h = fuzz_open_handle(data[0] & 1u);
    g_starts = 0u;
    g_client = NULL;
    if (w5500_mqtt_init(&g_client, mem, sizeof(mem), h, broker, 1883u,
                        "fuzz", mqtt_cb, NULL) != W5500_OK ||
        g_client == NULL) {
        abort();
    }
    rc = w5500_mqtt_connect(g_client, now, 60u, 1u, NULL, NULL);
    if (rc != W5500_OK) {
        abort();
    }
    g_starts++;
    snap = fuzz_lock_snap();

    while (p < size && ops < FUZZ_MAX_OPS) {
        uint8_t op = data[p++];
        ops++;
        switch (op & 0x07u) {
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
            w5500_mqtt_stop(g_client);
            break;
        case 3u: /* CONNECT */
            rc = w5500_mqtt_connect(g_client, now, 60u, 1u, NULL, NULL);
            fuzz_check_rc(rc);
            if (rc == W5500_OK) {
                g_starts++;
            }
            break;
        case 4u: { /* PUBLISH */
            uint8_t pl[4];
            uint16_t plen = 0u;
            while (plen < sizeof(pl) && p < size) {
                pl[plen++] = data[p++];
            }
            rc = w5500_mqtt_publish(g_client, now, "fuzz/t", pl, plen,
                                    (uint8_t)(op & 1u));
            fuzz_check_rc(rc);
            break;
        }
        case 5u: /* SUBSCRIBE */
            rc = w5500_mqtt_subscribe(g_client, now, "fuzz/#",
                                      (uint8_t)(op & 1u));
            fuzz_check_rc(rc);
            break;
        default: /* reserved ops are no-ops */
            break;
        }
        rc = w5500_mqtt_poll(g_client, now);
        if (rc != W5500_OK && rc != W5500_ERR_STATE) {
            abort();
        }
        (void)w5500_poll(h); /* clear SEND_DONE for the next transmit */
    }
done:
    w5500_mqtt_stop(g_client);
    fuzz_check_locks(snap);
    w5500_deinit(h);
    return 0;
}
