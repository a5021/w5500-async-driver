/**
 * @file fuzz_mqtt.c
 * @brief Fuzzes the MQTT 3.1.1 packet codec with arbitrary bytes.
 *
 * Properties:
 *   1. No out-of-bounds access for any input (ASan).
 *   2. remaining_len returns 0/-1 only; a decoded header length is 2..5.
 *   3. parse returns 0/-1 only; on success consumed is within the input and
 *      decoded topic/payload pointers alias inside that input; decoding is
 *      deterministic.
 *   4. Builders reject buffers smaller than the encoded packet and their
 *      output round-trips through parse.
 */
#include <stdlib.h>
#include <string.h>
#include "w5500_mqtt_internal.h"

static void make_str(uint8_t *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t i;
    if (n > cap - 1u) {
        n = cap - 1u;
    }
    for (i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
    dst[n] = 0u;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t str[200];  /* long enough to cross the 128-byte length boundary */
    static uint8_t payload[200];
    static uint8_t out[W5500_MQTT_PKT_MAX];
    w5500_mqtt_packet_t pkt;
    w5500_mqtt_packet_t pkt2;
    uint16_t remain = 0u;
    uint16_t remain2 = 0u;
    uint16_t consumed = 0u;
    uint16_t consumed2 = 0u;
    uint8_t hdr = 0u;
    uint8_t hdr2 = 0u;
    uint16_t n;
    size_t slen;
    size_t plen;
    int rc;

    if (size > 0xFFFFu) {
        return 0;
    }

    /* Remaining-length decoder: deterministic, bounded header. */
    rc = w5500_mqtt_remaining_len(data, (uint16_t)size, &remain, &hdr);
    if (rc != 0 && rc != -1) {
        abort();
    }
    if (w5500_mqtt_remaining_len(data, (uint16_t)size, &remain2, &hdr2) != rc ||
        (rc == 0 && (remain != remain2 || hdr != hdr2))) {
        abort();
    }
    if (rc == 0 && (hdr < 2u || hdr > 5u)) {
        abort();
    }
    if (w5500_mqtt_remaining_len(data, (uint16_t)size, NULL, &hdr2) != -1 ||
        w5500_mqtt_remaining_len(data, (uint16_t)size, &remain2, NULL) != -1) {
        abort();
    }

    /* Full packet parser: deterministic, pointers stay inside the input. */
    memset(&pkt, 0xAA, sizeof(pkt));
    rc = w5500_mqtt_parse(data, (uint16_t)size, &pkt, &consumed);
    if (rc != 0 && rc != -1) {
        abort();
    }
    memset(&pkt2, 0xAA, sizeof(pkt2));
    if (w5500_mqtt_parse(data, (uint16_t)size, &pkt2, &consumed2) != rc ||
        (rc == 0 && (consumed != consumed2 ||
                     memcmp(&pkt, &pkt2, sizeof(pkt)) != 0))) {
        abort();
    }
    if (rc == 0) {
        if (consumed == 0u || (size_t)consumed > size) {
            abort();
        }
        if (pkt.type > 15u || pkt.qos > 3u) {
            abort();
        }
        if (pkt.topic_len > 0u &&
            (pkt.topic == NULL || pkt.topic < data || pkt.topic >= data + size)) {
            abort();
        }
        if (pkt.payload_len > 0u &&
            (pkt.payload == NULL || pkt.payload < data ||
             pkt.payload >= data + size)) {
            abort();
        }
    }
    if (w5500_mqtt_parse(data, (uint16_t)size, NULL, &consumed) != -1) {
        abort();
    }

    if (size == 0u) {
        return 0;
    }

    /* Builders: derived strings, capacity limits and round trips. */
    slen = size % (sizeof(str) - 1u);
    plen = size % sizeof(payload);
    if (slen > size) {
        slen = size;
    }
    if (plen > size) {
        plen = size;
    }
    make_str(str, sizeof(str), data, slen);
    memcpy(payload, data, plen);

    n = w5500_mqtt_build_connect(out, sizeof(out), (const char *)str,
                                 (uint16_t)size, (uint8_t)(data[0] & 1u),
                                 NULL, NULL);
    if (n > 0u) {
        if ((size_t)n > sizeof(out) ||
            w5500_mqtt_parse(out, n, &pkt, &consumed) != 0 ||
            consumed != n || pkt.type != W5500_MQTT_CONNECT) {
            abort();
        }
    }
    if (w5500_mqtt_build_connect(out, 1u, (const char *)str, 0u, 0u,
                                 NULL, NULL) != 0u) {
        abort();
    }

    n = w5500_mqtt_build_publish(out, sizeof(out), (const char *)str,
                                 payload, (uint16_t)plen,
                                 (uint8_t)(data[0] & 1u));
    if (n > 0u) {
        if ((size_t)n > sizeof(out) ||
            w5500_mqtt_parse(out, n, &pkt, &consumed) != 0 ||
            consumed != n || pkt.type != W5500_MQTT_PUBLISH) {
            abort();
        }
    }
    if (w5500_mqtt_build_publish(out, 2u, (const char *)str, payload,
                                 (uint16_t)plen, 0u) != 0u) {
        abort();
    }

    n = w5500_mqtt_build_subscribe(out, sizeof(out), (const char *)str,
                                   (uint16_t)size, (uint8_t)(size & 1u));
    if (n > 0u) {
        if ((size_t)n > sizeof(out) ||
            w5500_mqtt_parse(out, n, &pkt, &consumed) != 0 ||
            consumed != n || pkt.type != W5500_MQTT_SUBSCRIBE) {
            abort();
        }
    }
    if (w5500_mqtt_build_subscribe(out, 1u, (const char *)str, 1u, 0u) != 0u) {
        abort();
    }

    if (w5500_mqtt_build_simple(out, sizeof(out), W5500_MQTT_PINGREQ) != 2u ||
        w5500_mqtt_build_simple(out, 1u, W5500_MQTT_PINGREQ) != 0u ||
        w5500_mqtt_parse(out, 2u, &pkt, &consumed) != 0 || consumed != 2u) {
        abort();
    }
    return 0;
}
