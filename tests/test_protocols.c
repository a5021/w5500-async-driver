/**
 * @file test_protocols.c
 * @brief Unit tests for the DHCP client and DNS resolver against the chip
 *        emulator. Build & run: make test
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_registers.h"
#include "w5500_dhcp.h"
#include "w5500_dns.h"
#include "../src/w5500_dhcp_internal.h"
#include "../src/w5500_dns_internal.h"
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

static w5500_handle_t fixture_open(void)
{
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;

    mock_w5500_reset();
    mock_w5500_plat(&plat);
    CHECK_EQ(w5500_init(&h, &plat), W5500_OK);
    CHECK(h != NULL);
    CHECK_EQ(w5500_set_timeout(h, 50u), W5500_OK);
    return h;
}

/* ------------------------------------------------------------------ */
/* DHCP packet builder + event capture                                 */
/* ------------------------------------------------------------------ */

static unsigned g_dhcp_events;
static w5500_dhcp_event_t g_dhcp_last;

static void dhcp_cb(w5500_dhcp_t *dhcp, w5500_dhcp_event_t event, void *arg)
{
    (void)dhcp;
    (void)arg;
    g_dhcp_events++;
    g_dhcp_last = event;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t build_reply(uint8_t *b, uint8_t type, const uint8_t xid[4],
                            const uint8_t yiaddr[4], const uint8_t mask[4],
                            const uint8_t gw[4], const uint8_t dns[4],
                            uint32_t lease_s, const uint8_t server[4])
{
    uint16_t off = 240u;

    memset(b, 0, 512);
    b[0] = 2u; /* BOOTREPLY */
    b[1] = 1u;
    b[2] = 6u;
    memcpy(b + 4, xid, 4u);
    if (yiaddr != NULL) {
        memcpy(b + 16, yiaddr, 4u);
    }
    b[236] = 0x63u;
    b[237] = 0x82u;
    b[238] = 0x53u;
    b[239] = 0x63u;

    b[off++] = 53u;
    b[off++] = 1u;
    b[off++] = type;
    if (server != NULL) {
        b[off++] = 54u;
        b[off++] = 4u;
        memcpy(b + off, server, 4u);
        off = (uint16_t)(off + 4u);
    }
    if (lease_s != 0u) {
        b[off++] = 51u;
        b[off++] = 4u;
        put32(b + off, lease_s);
        off = (uint16_t)(off + 4u);
    }
    if (mask != NULL) {
        b[off++] = 1u;
        b[off++] = 4u;
        memcpy(b + off, mask, 4u);
        off = (uint16_t)(off + 4u);
    }
    if (gw != NULL) {
        b[off++] = 3u;
        b[off++] = 4u;
        memcpy(b + off, gw, 4u);
        off = (uint16_t)(off + 4u);
    }
    if (dns != NULL) {
        b[off++] = 6u;
        b[off++] = 4u;
        memcpy(b + off, dns, 4u);
        off = (uint16_t)(off + 4u);
    }
    b[off++] = 255u;
    return off;
}

static void test_dhcp_parse(void)
{
    uint8_t pkt[512];
    uint8_t xid[4] = {1, 2, 3, 4};
    uint8_t ip[4] = {192, 168, 1, 50};
    uint8_t mask[4] = {255, 255, 255, 0};
    uint8_t gw[4] = {192, 168, 1, 1};
    uint8_t dns[4] = {8, 8, 8, 8};
    uint8_t srv[4] = {192, 168, 1, 1};
    w5500_dhcp_parse_t m;
    uint16_t len;

    len = build_reply(pkt, 5u, xid, ip, mask, gw, dns, 60u, srv);
    CHECK_EQ(w5500_dhcp_parse_packet(pkt, len, &m), 0);
    CHECK_EQ(m.msg_type, 5);
    CHECK_EQ(m.has_ip, 1);
    CHECK_EQ(m.ip[3], 50);
    CHECK_EQ(m.has_lease, 1);
    CHECK_EQ(m.lease_s, 60u);
    CHECK_EQ(m.has_mask, 1);
    CHECK_EQ(m.mask[2], 255);
    CHECK_EQ(m.has_gw, 1);
    CHECK_EQ(m.gw[3], 1);
    CHECK_EQ(m.has_dns, 1);
    CHECK_EQ(m.dns[0], 8);
    CHECK_EQ(m.has_server, 1);
    CHECK_EQ(m.server[2], 1);

    /* Truncated option payload, bad cookie, wrong op: all rejected. */
    CHECK_EQ(w5500_dhcp_parse_packet(pkt, 100u, &m), -1);
    CHECK_EQ(w5500_dhcp_parse_packet(pkt, (uint16_t)(len - 2u), &m), -1);
    pkt[236] = 0u;
    CHECK_EQ(w5500_dhcp_parse_packet(pkt, len, &m), -1);
    pkt[236] = 0x63u;
    pkt[0] = 1u;
    CHECK_EQ(w5500_dhcp_parse_packet(pkt, len, &m), -1);
    CHECK_EQ(w5500_dhcp_parse_packet(NULL, len, &m), -1);
}

static void test_dhcp_flow(void)
{
    static uint8_t mem[4096];
    w5500_handle_t h = fixture_open();
    w5500_dhcp_t *d = NULL;
    uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    static const uint8_t bcast_lease_ip[4] = {10, 0, 0, 77};
    static const uint8_t mask[4] = {255, 255, 255, 0};
    static const uint8_t gw[4] = {10, 0, 0, 1};
    static const uint8_t dns[4] = {10, 0, 0, 2};
    static const uint8_t srv[4] = {10, 0, 0, 1};
    uint8_t reply[512];
    uint8_t xid[4];
    uint8_t got[4];
    uint16_t len;
    uint32_t now = 0u;

    CHECK(w5500_dhcp_size() <= sizeof(mem));
    g_dhcp_events = 0u;
    CHECK_EQ(w5500_dhcp_init(&d, mem, sizeof(mem), h, mac, dhcp_cb, NULL),
             W5500_OK);
    CHECK(d != NULL);
    CHECK_EQ(w5500_dhcp_start(d, now), W5500_OK);

    /* DISCOVER left the socket and is captured by the emulator. */
    CHECK(g_mock.last_sent_len[0] >= 240u);
    CHECK_EQ(g_mock.last_sent[0][0], 1);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_SR], W5500_SOCK_UDP);
    memcpy(xid, g_mock.last_sent[0] + 4, 4u);

    /* Clear the SEND_DONE so the client may transmit again. */
    CHECK_EQ(w5500_poll(h), W5500_OK);

    /* OFFER -> client must answer with REQUEST (option 53 == 3). */
    len = build_reply(reply, 2u, xid, bcast_lease_ip, mask, gw, dns, 120u, srv);
    mock_w5500_inject_rx(0, reply, len);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    CHECK_EQ(g_mock.last_sent[0][240], 53);
    CHECK_EQ(g_mock.last_sent[0][242], 3);
    CHECK_EQ(g_dhcp_events, 0u);

    CHECK_EQ(w5500_poll(h), W5500_OK);

    /* ACK -> BOUND, chip reconfigured, lease readable. */
    len = build_reply(reply, 5u, xid, bcast_lease_ip, mask, gw, dns, 120u, srv);
    mock_w5500_inject_rx(0, reply, len);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    CHECK_EQ(g_dhcp_events, 1u);
    CHECK_EQ(g_dhcp_last, W5500_DHCP_EVENT_BOUND);

    CHECK_EQ(w5500_get_ip(h, got), W5500_OK);
    CHECK_EQ(memcmp(got, bcast_lease_ip, 4), 0);
    CHECK_EQ(w5500_get_subnet_mask(h, got), W5500_OK);
    CHECK_EQ(memcmp(got, mask, 4), 0);
    CHECK_EQ(w5500_get_gateway(h, got), W5500_OK);
    CHECK_EQ(memcmp(got, gw, 4), 0);
    CHECK_EQ(w5500_dhcp_get_config(d, NULL, NULL, NULL, got), W5500_OK);
    CHECK_EQ(memcmp(got, dns, 4), 0);

    /* At 50% of the lease the client re-requests; ACK -> RENEWED. */
    now = 60000u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    CHECK_EQ(g_mock.last_sent[0][240], 53);
    CHECK_EQ(g_mock.last_sent[0][242], 3);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    len = build_reply(reply, 5u, xid, bcast_lease_ip, mask, gw, dns, 120u, srv);
    mock_w5500_inject_rx(0, reply, len);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    CHECK_EQ(g_dhcp_events, 2u);
    CHECK_EQ(g_dhcp_last, W5500_DHCP_EVENT_RENEWED);

    w5500_dhcp_stop(d);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_SR], W5500_SOCK_CLOSED);
    w5500_deinit(h);

    CHECK_EQ(g_mock.lock_count, g_mock.unlock_count);
}

static void test_dhcp_nak_and_timeout(void)
{
    static uint8_t mem[4096];
    w5500_handle_t h = fixture_open();
    w5500_dhcp_t *d = NULL;
    uint8_t mac[6] = {0x02, 1, 2, 3, 4, 5};
    static const uint8_t ip[4] = {10, 0, 0, 90};
    static const uint8_t srv[4] = {10, 0, 0, 1};
    uint8_t reply[512];
    uint8_t xid[4];
    uint16_t len;
    uint32_t now = 0u;

    g_dhcp_events = 0u;
    CHECK_EQ(w5500_dhcp_init(&d, mem, sizeof(mem), h, mac, dhcp_cb, NULL),
             W5500_OK);
    CHECK_EQ(w5500_dhcp_start(d, now), W5500_OK);
    memcpy(xid, g_mock.last_sent[0] + 4, 4u);
    CHECK_EQ(w5500_poll(h), W5500_OK);

    /* OFFER, then a NAK while REQUESTING -> back to DISCOVER. */
    len = build_reply(reply, 2u, xid, ip, NULL, NULL, NULL, 0u, srv);
    mock_w5500_inject_rx(0, reply, len);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);

    len = build_reply(reply, 6u, xid, NULL, NULL, NULL, NULL, 0u, srv);
    mock_w5500_inject_rx(0, reply, len);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    CHECK_EQ(g_mock.last_sent[0][242], 1); /* DISCOVER again */
    memcpy(xid, g_mock.last_sent[0] + 4, 4u);

    /* No answers: 4 attempts, 4 s apart, then FAILED and socket closed. */
    now = 4000u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    CHECK_EQ(g_dhcp_events, 0u);
    now = 8000u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    now = 12000u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    CHECK_EQ(g_dhcp_events, 0u);
    now = 16000u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_dhcp_poll(d, now), W5500_OK);
    CHECK_EQ(g_dhcp_events, 1u);
    CHECK_EQ(g_dhcp_last, W5500_DHCP_EVENT_FAILED);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_SR], W5500_SOCK_CLOSED);
    w5500_deinit(h);
}

/* ------------------------------------------------------------------ */
/* DNS                                                                 */
/* ------------------------------------------------------------------ */

static unsigned g_dns_results;
static w5500_result_t g_dns_status;
static uint8_t g_dns_ip[4];

static void dns_cb(w5500_dns_t *dns, const uint8_t ip[4],
                   w5500_result_t status, void *arg)
{
    (void)dns;
    (void)arg;
    g_dns_results++;
    g_dns_status = status;
    memcpy(g_dns_ip, ip, 4u);
}

static uint16_t build_dns_answer(uint8_t *b, uint16_t qid, uint8_t rcode,
                                 uint8_t ancount, const uint8_t ip[4])
{
    /* Header + question for "example.com" + one A answer. */
    static const uint8_t qname[] = {7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
                                    3, 'c', 'o', 'm', 0};
    uint16_t off = 12u;

    memset(b, 0, 512);
    b[0] = (uint8_t)(qid >> 8);
    b[1] = (uint8_t)qid;
    b[2] = 0x81u;
    b[3] = (uint8_t)(0x80u | rcode);
    b[5] = 1u; /* QDCOUNT */
    b[7] = ancount;

    memcpy(b + off, qname, sizeof(qname));
    off = (uint16_t)(off + sizeof(qname));
    b[off++] = 0u;
    b[off++] = 1u; /* A */
    b[off++] = 0u;
    b[off++] = 1u; /* IN */

    if (ancount != 0u) {
        b[off++] = 0xC0u; /* compression pointer to the question name */
        b[off++] = 0x0Cu;
        b[off++] = 0u;
        b[off++] = 1u; /* A */
        b[off++] = 0u;
        b[off++] = 1u; /* IN */
        put32(b + off, 60u);
        off = (uint16_t)(off + 4u);
        b[off++] = 0u;
        b[off++] = 4u; /* RDLENGTH */
        memcpy(b + off, ip, 4u);
        off = (uint16_t)(off + 4u);
    }
    return off;
}

static void test_dns_parse(void)
{
    uint8_t pkt[512];
    uint8_t ip[4];
    uint16_t len;

    ip[0] = 93;
    ip[1] = 184;
    ip[2] = 216;
    ip[3] = 34;
    len = build_dns_answer(pkt, 0x1234u, 0u, 1u, ip);
    memset(ip, 0, sizeof(ip));
    CHECK_EQ(w5500_dns_parse_response(pkt, len, 0x1234u, ip), 0);
    CHECK_EQ(ip[2], 216);

    /* Wrong id, NXDOMAIN, no answers, truncations: rejected. */
    CHECK_EQ(w5500_dns_parse_response(pkt, len, 0x4321u, ip), -1);
    len = build_dns_answer(pkt, 0x1234u, 3u, 1u, ip);
    CHECK_EQ(w5500_dns_parse_response(pkt, len, 0x1234u, ip), -1);
    len = build_dns_answer(pkt, 0x1234u, 0u, 0u, ip);
    CHECK_EQ(w5500_dns_parse_response(pkt, len, 0x1234u, ip), -1);
    len = build_dns_answer(pkt, 0x1234u, 0u, 1u, ip);
    CHECK_EQ(w5500_dns_parse_response(pkt, 11u, 0x1234u, ip), -1);
    CHECK_EQ(w5500_dns_parse_response(pkt, (uint16_t)(len - 2u), 0x1234u, ip),
             -1);
    CHECK_EQ(w5500_dns_parse_response(NULL, len, 0x1234u, ip), -1);
}

static void test_dns_query_and_answer(void)
{
    static uint8_t mem[4096];
    w5500_handle_t h = fixture_open();
    w5500_dns_t *d = NULL;
    static const uint8_t server[4] = {8, 8, 8, 8};
    static const uint8_t answer[4] = {93, 184, 216, 34};
    uint8_t pkt[512];
    uint16_t qid;
    uint16_t len;

    CHECK(w5500_dns_size() <= sizeof(mem));
    g_dns_results = 0u;
    CHECK_EQ(w5500_dns_init(&d, mem, sizeof(mem), h, server, dns_cb, NULL),
             W5500_OK);

    /* Bad names are rejected before any I/O. */
    CHECK_EQ(w5500_dns_start(d, "", 0u), W5500_ERR_PARAM);
    CHECK_EQ(w5500_dns_start(d, "a..b", 0u), W5500_ERR_PARAM);
    CHECK_EQ(w5500_dns_start(d, NULL, 0u), W5500_ERR_PARAM);

    CHECK_EQ(w5500_dns_start(d, "example.com", 0u), W5500_OK);
    CHECK_EQ(g_mock.last_sent_len[0], 12u + 13u + 4u); /* header+qname+qtype */
    qid = (uint16_t)(((uint16_t)g_mock.last_sent[0][0] << 8) |
                     g_mock.last_sent[0][1]);
    CHECK_EQ(g_mock.last_sent[0][12], 7); /* first label length */

    CHECK_EQ(w5500_poll(h), W5500_OK); /* clear SEND_DONE */

    len = build_dns_answer(pkt, qid, 0u, 1u, answer);
    mock_w5500_inject_rx(0, pkt, len);
    CHECK_EQ(w5500_dns_poll(d, 0u), W5500_OK);
    CHECK_EQ(g_dns_results, 1u);
    CHECK_EQ(g_dns_status, W5500_OK);
    CHECK_EQ(memcmp(g_dns_ip, answer, 4), 0);

    /* NXDOMAIN completes immediately with ERR_STATE. */
    CHECK_EQ(w5500_dns_start(d, "example.com", 0u), W5500_OK);
    qid = (uint16_t)(((uint16_t)g_mock.last_sent[0][0] << 8) |
                     g_mock.last_sent[0][1]);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    len = build_dns_answer(pkt, qid, 3u, 0u, answer);
    mock_w5500_inject_rx(0, pkt, len);
    CHECK_EQ(w5500_dns_poll(d, 0u), W5500_OK);
    CHECK_EQ(g_dns_results, 2u);
    CHECK_EQ(g_dns_status, W5500_ERR_STATE);

    w5500_dns_stop(d);
    w5500_deinit(h);
    CHECK_EQ(g_mock.lock_count, g_mock.unlock_count);
}

static void test_dns_timeout(void)
{
    static uint8_t mem[4096];
    w5500_handle_t h = fixture_open();
    w5500_dns_t *d = NULL;
    static const uint8_t server[4] = {1, 1, 1, 1};

    g_dns_results = 0u;
    CHECK_EQ(w5500_dns_init(&d, mem, sizeof(mem), h, server, dns_cb, NULL),
             W5500_OK);
    CHECK_EQ(w5500_dns_start(d, "example.com", 0u), W5500_OK);
    CHECK_EQ(w5500_dns_poll(d, 1999u), W5500_OK);
    CHECK_EQ(g_dns_results, 0u);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_dns_poll(d, 2000u), W5500_OK); /* attempt 2 */
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_dns_poll(d, 4000u), W5500_OK); /* attempt 3 */
    CHECK_EQ(g_dns_results, 0u);
    CHECK_EQ(w5500_dns_poll(d, 6000u), W5500_OK); /* retries exhausted */
    CHECK_EQ(g_dns_results, 1u);
    CHECK_EQ(g_dns_status, W5500_ERR_TIMEOUT);
    w5500_dns_stop(d);
    w5500_deinit(h);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_dhcp_parse();
    test_dhcp_flow();
    test_dhcp_nak_and_timeout();
    test_dns_parse();
    test_dns_query_and_answer();
    test_dns_timeout();

    if (g_failures == 0) {
        printf("OK: all protocol tests passed\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
