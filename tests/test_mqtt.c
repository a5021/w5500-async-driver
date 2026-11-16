/**
 * @file test_mqtt.c
 * @brief Unit tests for the MQTT codec and client state machine.
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_mqtt.h"
#include "w5500_mqtt_internal.h"
#include "mock_w5500.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long _a = (long)(a);                                                 \
        long _b = (long)(b);                                                 \
        if (_a != _b) {                                                      \
            printf("FAIL %s:%d: %s == %s (%ld != %ld)\n", __FILE__,          \
                   __LINE__, #a, #b, _a, _b);                                \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Codec                                                               */
/* ------------------------------------------------------------------ */

static void test_codec(void)
{
    uint8_t buf[W5500_MQTT_PKT_MAX];
    uint16_t n;
    w5500_mqtt_packet_t pkt;
    uint16_t consumed;

    /* CONNECT packet layout. */
    n = w5500_mqtt_build_connect(buf, sizeof(buf), "dev1", 60u, 1u,
                                 "user", "pass");
    CHECK(n != 0u);
    CHECK_EQ(buf[0], 0x10);
    CHECK_EQ(buf[1], n - 2u);
    CHECK_EQ(memcmp(buf + 2, "MQTT", 4), 0);
    CHECK_EQ(buf[6], 4);
    CHECK_EQ(buf[7], (1u << 7) | (1u << 6) | 0x02u);
    CHECK_EQ(buf[8], 0);
    CHECK_EQ(buf[9], 60);

    /* PUBLISH packet with topic+payload. */
    n = w5500_mqtt_build_publish(buf, sizeof(buf), "a/b",
                                 (const uint8_t *)"hi", 2u, 1u);
    CHECK_EQ(n, 9);
    CHECK_EQ(buf[0], 0x31);
    CHECK_EQ(buf[1], 7);
    CHECK_EQ(buf[4], 'a');
    CHECK_EQ(memcmp(buf + 7, "hi", 2), 0);

    /* Decode a broker PUBLISH (remaining length = 6). */
    {
        static const uint8_t pub[] = {0x30, 6, 0, 2, 'a', 'b', 'O', 'K'};
        CHECK_EQ(w5500_mqtt_parse(pub, sizeof(pub), &pkt, &consumed), 0);
        CHECK_EQ(pkt.type, W5500_MQTT_PUBLISH);
        CHECK_EQ(pkt.qos, 0);
        CHECK_EQ(pkt.topic_len, 2);
        CHECK_EQ(pkt.payload_len, 2);
        CHECK_EQ(memcmp(pkt.payload, "OK", 2), 0);
        CHECK_EQ(consumed, sizeof(pub));
    }
    /* CONNACK success and failure. */
    {
        static const uint8_t ack[] = {0x20, 2, 0, 0};
        CHECK_EQ(w5500_mqtt_parse(ack, sizeof(ack), &pkt, &consumed), 0);
        CHECK_EQ(pkt.flags, 0);
    }
    {
        static const uint8_t bad[] = {0x20, 2, 0, 5};
        CHECK_EQ(w5500_mqtt_parse(bad, sizeof(bad), &pkt, &consumed), 0);
        CHECK_EQ(pkt.flags, 5);
    }
    /* Truncated and malformed frames. */
    {
        static const uint8_t trunc[] = {0x30, 10, 0, 2, 'a'};
        CHECK_EQ(w5500_mqtt_parse(trunc, sizeof(trunc), &pkt, &consumed), -1);
        static const uint8_t badlen[] = {0x30, 0x80, 0x80, 0x80, 0x80};
        CHECK_EQ(w5500_mqtt_parse(badlen, sizeof(badlen), &pkt, &consumed), -1);
        static const uint8_t short_cont[] = {0x30, 0x80};
        CHECK_EQ(w5500_mqtt_parse(short_cont, sizeof(short_cont), &pkt,
                                  &consumed), -1);
        CHECK_EQ(w5500_mqtt_parse(NULL, 0u, &pkt, &consumed), -1);
    }
    /* SUBSCRIBE and PINGREQ. */
    n = w5500_mqtt_build_subscribe(buf, sizeof(buf), "a/b", 7u, 1u);
    CHECK_EQ(n, 10);
    CHECK_EQ(buf[0], 0x82);
    CHECK_EQ(buf[2], 0);
    CHECK_EQ(buf[3], 7);
    CHECK_EQ(buf[9], 1);
    n = w5500_mqtt_build_simple(buf, sizeof(buf), W5500_MQTT_PINGREQ);
    CHECK_EQ(n, 2);
    CHECK_EQ(buf[0], 0xC0);
    CHECK_EQ(buf[1], 0);
}

/* ------------------------------------------------------------------ */
/* Client                                                              */
/* ------------------------------------------------------------------ */

static unsigned g_events;
static w5500_mqtt_event_t g_last_event;
static uint8_t g_topic[16];
static uint8_t g_payload[16];
static uint16_t g_topic_len;
static uint16_t g_payload_len;

static void on_mqtt(w5500_mqtt_t *cli, w5500_mqtt_event_t ev,
                    const uint8_t *topic, uint16_t tlen,
                    const uint8_t *payload, uint16_t plen, void *arg)
{
    (void)cli;
    (void)arg;
    g_events++;
    g_last_event = ev;
    g_topic_len = tlen;
    g_payload_len = plen;
    if (tlen > 0u && topic != NULL) {
        memcpy(g_topic, topic, (size_t)tlen);
    }
    if (plen > 0u && payload != NULL) {
        memcpy(g_payload, payload, (size_t)plen);
    }
}

static w5500_handle_t fixture_open(void)
{
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;

    mock_w5500_reset();
    mock_w5500_plat(&plat);
    CHECK_EQ(w5500_init(&h, &plat), W5500_OK);
    CHECK_EQ(w5500_set_timeout(h, 50u), W5500_OK);
    return h;
}

static void pump(w5500_handle_t h, w5500_mqtt_t *c, uint32_t now, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        (void)w5500_poll(h);
        (void)w5500_mqtt_poll(c, now);
    }
}

static void test_client_flow(void)
{
    static uint8_t mem[4096];
    w5500_handle_t h = fixture_open();
    w5500_mqtt_t *c = NULL;
    static const uint8_t broker[4] = {10, 0, 0, 5};
    static const uint8_t connack[] = {0x20, 2, 0, 0};
    static const uint8_t publish[] =
        {0x30, 6, 0, 2, 'a', 'b', 'O', 'K'};

    g_events = 0u;
    CHECK_EQ(w5500_mqtt_init(&c, mem, sizeof(mem), h, broker, 1883u,
                             "dev1", on_mqtt, NULL), W5500_OK);
    CHECK_EQ(w5500_mqtt_connect(c, 0u, 60u, 1u, NULL, NULL), W5500_OK);
    pump(h, c, 0u, 3u);
    CHECK_EQ(g_mock.last_sent_len[0], 16u); /* CONNECT: 2 + 10 + 4 */

    /* Inject CONNACK and a PUBLISH. */
    mock_w5500_inject_rx(0, connack, sizeof(connack));
    mock_w5500_inject_rx(0, publish, sizeof(publish));
    pump(h, c, 0u, 3u);
    CHECK_EQ(g_events, 2u);
    CHECK_EQ(g_last_event, W5500_MQTT_EVENT_PUBLISH);
    CHECK_EQ(g_topic_len, 2);
    CHECK_EQ(memcmp(g_topic, "ab", 2), 0);
    CHECK_EQ(g_payload_len, 2);
    CHECK_EQ(memcmp(g_payload, "OK", 2), 0);

    /* Now ready: publish + subscribe queued. */
    CHECK_EQ(w5500_mqtt_publish(c, 0u, "x", (const uint8_t *)"v", 1u, 0u),
             W5500_OK);
    pump(h, c, 0u, 2u);
    CHECK_EQ(g_mock.last_sent[0][0], 0x30);
    CHECK_EQ(w5500_mqtt_subscribe(c, 0u, "x", 0u), W5500_OK);
    pump(h, c, 0u, 2u);
    CHECK_EQ(g_mock.last_sent[0][0], 0x82);

    w5500_mqtt_stop(c);
    w5500_deinit(h);
}

static void test_client_errors(void)
{
    static uint8_t mem[4096];
    w5500_handle_t h = fixture_open();
    w5500_mqtt_t *c = NULL;
    static const uint8_t broker[4] = {10, 0, 0, 5};
    static const uint8_t refused[] = {0x20, 2, 0, 5};

    g_events = 0u;
    CHECK_EQ(w5500_mqtt_init(&c, mem, sizeof(mem), h, broker, 1883u,
                             "dev1", on_mqtt, NULL), W5500_OK);
    CHECK_EQ(w5500_mqtt_publish(c, 0u, "x", NULL, 0u, 0u), W5500_ERR_STATE);
    CHECK_EQ(w5500_mqtt_subscribe(c, 0u, "x", 0u), W5500_ERR_STATE);

    CHECK_EQ(w5500_mqtt_connect(c, 0u, 0u, 1u, NULL, NULL), W5500_OK);
    pump(h, c, 0u, 2u);
    mock_w5500_inject_rx(0, refused, sizeof(refused));
    pump(h, c, 0u, 3u);
    CHECK_EQ(g_events, 1u);
    CHECK_EQ(g_last_event, W5500_MQTT_EVENT_ERROR);

    w5500_mqtt_stop(c);
    w5500_deinit(h);
}

static void test_init_validation(void)
{
    static uint8_t mem[4096];
    w5500_handle_t h = fixture_open();
    w5500_mqtt_t *c = NULL;
    static const uint8_t broker[4] = {10, 0, 0, 5};

    CHECK_EQ(w5500_mqtt_init(&c, mem, 4u, h, broker, 1883u, "x", on_mqtt,
                             NULL), W5500_ERR_NOMEM);
    CHECK_EQ(w5500_mqtt_init(&c, mem, sizeof(mem), h, broker, 0u, "x",
                             on_mqtt, NULL), W5500_ERR_PARAM);
    CHECK_EQ(w5500_mqtt_init(&c, mem, sizeof(mem), h, broker, 1883u, "x",
                             NULL, NULL), W5500_ERR_PARAM);
    w5500_deinit(h);
}

int main(void)
{
    test_codec();
    test_client_flow();
    test_client_errors();
    test_init_validation();

    if (g_failures == 0) {
        printf("OK: all mqtt tests passed\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
