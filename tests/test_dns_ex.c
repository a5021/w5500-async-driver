/**
 * @file test_dns_ex.c
 * @brief Tests for CNAME following, multiple A records and the answer cache.
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_dns.h"
#include "../src/w5500_dns_internal.h"
#include "mock_w5500.h"

static int g_failures;

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

static const uint8_t IP1[4] = {93, 184, 216, 34};
static const uint8_t IP2[4] = {93, 184, 216, 35};

static unsigned g_calls;
static uint8_t g_count;
static uint32_t g_ttl;
static w5500_result_t g_status;
static uint8_t g_ips[W5500_DNS_MAX_ADDRS][4];

static void multi_cb(w5500_dns_t *dns, const w5500_dns_addr_t *addrs,
                     uint8_t count, w5500_result_t status, void *arg)
{
    uint8_t i;

    (void)dns;
    (void)arg;
    g_calls++;
    g_count = count;
    g_status = status;
    g_ttl = (count > 0u) ? addrs[0].ttl_s : 0u;
    for (i = 0u; i < count && i < W5500_DNS_MAX_ADDRS; i++) {
        memcpy(g_ips[i], addrs[i].ip, 4u);
    }
}

static uint16_t put_name(uint8_t *b, uint16_t off, const char *name)
{
    while (*name != '\0') {
        const char *dot = strchr(name, '.');
        size_t n = (dot != NULL) ? (size_t)(dot - name) : strlen(name);
        b[off++] = (uint8_t)n;
        memcpy(b + off, name, n);
        off = (uint16_t)(off + n);
        name += n;
        if (*name == '.') {
            name++;
        }
    }
    b[off++] = 0u;
    return off;
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

static uint16_t build_cname_reply(uint8_t *b, uint16_t qid, uint32_t ttl)
{
    uint8_t target[64];
    uint16_t tlen = put_name(target, 0u, "www.example.com");
    uint16_t off;

    memset(b, 0, 512u);
    b[0] = (uint8_t)(qid >> 8);
    b[1] = (uint8_t)qid;
    b[2] = 0x81u;
    b[3] = 0x80u;
    b[5] = 1u; /* QDCOUNT */
    b[7] = 3u; /* ANCOUNT */
    off = put_name(b, 12u, "alias.example.com");
    b[off++] = 0u; b[off++] = 1u; /* QTYPE A */
    b[off++] = 0u; b[off++] = 1u; /* QCLASS IN */

    off = put_name(b, off, "alias.example.com");
    b[off++] = 0u; b[off++] = 5u; /* CNAME */
    b[off++] = 0u; b[off++] = 1u;
    put32(b + off, ttl); off = (uint16_t)(off + 4u);
    b[off++] = (uint8_t)(tlen >> 8);
    b[off++] = (uint8_t)tlen;
    memcpy(b + off, target, tlen);
    off = (uint16_t)(off + tlen);

    off = put_name(b, off, "www.example.com");
    b[off++] = 0u; b[off++] = 1u; /* A */
    b[off++] = 0u; b[off++] = 1u;
    put32(b + off, ttl); off = (uint16_t)(off + 4u);
    b[off++] = 0u; b[off++] = 4u;
    memcpy(b + off, IP1, 4u); off = (uint16_t)(off + 4u);

    off = put_name(b, off, "www.example.com");
    b[off++] = 0u; b[off++] = 1u;
    b[off++] = 0u; b[off++] = 1u;
    put32(b + off, ttl); off = (uint16_t)(off + 4u);
    b[off++] = 0u; b[off++] = 4u;
    memcpy(b + off, IP2, 4u); off = (uint16_t)(off + 4u);
    return off;
}

static void test_codec(void)
{
    uint8_t pkt[512];
    uint8_t ip[4];
    w5500_dns_addr_t addrs[W5500_DNS_MAX_ADDRS];
    uint8_t count = 0xFFu;
    uint16_t len = build_cname_reply(pkt, 0x1234u, 60u);

    memset(addrs, 0, sizeof(addrs));
    CHECK_EQ(w5500_dns_parse_response_ex(pkt, len, 0x1234u,
                                         "alias.example.com", addrs,
                                         W5500_DNS_MAX_ADDRS, &count), 0);
    CHECK_EQ(count, 2u);
    CHECK_EQ(memcmp(addrs[0].ip, IP1, 4), 0);
    CHECK_EQ(memcmp(addrs[1].ip, IP2, 4), 0);
    CHECK_EQ(addrs[0].ttl_s, 60u);

    /* An unrelated name gets no records. */
    count = 0xFFu;
    CHECK_EQ(w5500_dns_parse_response_ex(pkt, len, 0x1234u, "other.example",
                                         addrs, W5500_DNS_MAX_ADDRS, &count), 0);
    CHECK_EQ(count, 0u);

    /* The legacy wrapper still returns the first A record. */
    memset(ip, 0, sizeof(ip));
    CHECK_EQ(w5500_dns_parse_response(pkt, len, 0x1234u, ip), 0);
    CHECK_EQ(memcmp(ip, IP1, 4), 0);

    /* Capacity one yields a single address. */
    count = 0xFFu;
    CHECK_EQ(w5500_dns_parse_response_ex(pkt, len, 0x1234u,
                                         "alias.example.com", addrs, 1u,
                                         &count), 0);
    CHECK_EQ(count, 1u);

    /* Malformed and parameter checks. */
    CHECK_EQ(w5500_dns_parse_response_ex(pkt, len, 0x4321u, NULL, addrs,
                                         W5500_DNS_MAX_ADDRS, &count), -1);
    CHECK_EQ(w5500_dns_parse_response_ex(pkt, len, 0x1234u, NULL, NULL, 1u,
                                         &count), -1);
    CHECK_EQ(w5500_dns_parse_response_ex(pkt, len, 0x1234u, NULL, addrs, 0u,
                                         &count), -1);
    CHECK_EQ(w5500_dns_parse_response_ex(pkt, len, 0x1234u, NULL, addrs, 1u,
                                         NULL), -1);
    CHECK_EQ(w5500_dns_parse_response_ex(pkt, 11u, 0x1234u, NULL, addrs, 1u,
                                         &count), -1);
}

static w5500_handle_t fixture_open(void)
{
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;

    mock_w5500_reset();
    mock_w5500_plat(&plat);
    if (w5500_init(&h, &plat) != W5500_OK) {
        return NULL;
    }
    return h;
}

static uint16_t sent_qid(void)
{
    return (uint16_t)(((uint16_t)g_mock.last_sent[0][0] << 8) |
                      g_mock.last_sent[0][1]);
}

static void test_cache_and_multi(void)
{
    static uint8_t mem[4096];
    w5500_handle_t h = fixture_open();
    w5500_dns_t *d = NULL;
    static const uint8_t server[4] = {8, 8, 8, 8};
    uint8_t pkt[512];
    uint16_t qid;
    uint16_t len;

    CHECK_EQ(w5500_dns_init_multi(&d, mem, sizeof(mem), h, server, multi_cb,
                                  NULL), W5500_OK);
    w5500_dns_set_cache(d, 1u);
    g_calls = 0u;

    CHECK_EQ(w5500_dns_start(d, "alias.example.com", 0u), W5500_OK);
    qid = sent_qid();
    CHECK_EQ(w5500_poll(h), W5500_OK);
    len = build_cname_reply(pkt, qid, 60u);
    mock_w5500_inject_rx(0, pkt, len);
    CHECK_EQ(w5500_dns_poll(d, 0u), W5500_OK);
    CHECK_EQ(g_calls, 1u);
    CHECK_EQ(g_status, W5500_OK);
    CHECK_EQ(g_count, 2u);
    CHECK_EQ(g_ttl, 60u);
    CHECK_EQ(memcmp(g_ips[0], IP1, 4), 0);
    CHECK_EQ(memcmp(g_ips[1], IP2, 4), 0);

    /* Second query is served from cache on the next poll, no new packet. */
    CHECK_EQ(w5500_dns_start(d, "ALIAS.EXAMPLE.COM", 1000u), W5500_OK);
    CHECK_EQ(w5500_dns_poll(d, 1000u), W5500_OK);
    CHECK_EQ(g_calls, 2u);
    CHECK_EQ(g_count, 1u);
    CHECK_EQ(memcmp(g_ips[0], IP1, 4), 0);
    CHECK_EQ(sent_qid(), qid);

    /* After expiry, a fresh query goes on the wire. */
    CHECK_EQ(w5500_poll(h), W5500_OK); /* clear SEND_DONE */
    CHECK_EQ(w5500_dns_start(d, "alias.example.com", 62000u), W5500_OK);
    CHECK(sent_qid() != qid);
    CHECK_EQ(w5500_dns_poll(d, 62000u), W5500_OK);
    CHECK_EQ(g_calls, 2u); /* no callback until the new query resolves */

    /* Explicit clear also forces a fresh query. */
    qid = sent_qid();
    CHECK_EQ(w5500_poll(h), W5500_OK);
    w5500_dns_cache_clear(d);
    CHECK_EQ(w5500_dns_start(d, "alias.example.com", 62000u), W5500_OK);
    CHECK(sent_qid() != qid);

    w5500_dns_stop(d);
    w5500_deinit(h);
    CHECK_EQ(g_mock.lock_count, g_mock.unlock_count);
}

static void test_init_validation(void)
{
    static uint8_t mem[4096];
    w5500_handle_t h = fixture_open();
    w5500_dns_t *d = NULL;
    static const uint8_t server[4] = {8, 8, 8, 8};

    CHECK_EQ(w5500_dns_init_multi(&d, mem, 4u, h, server, multi_cb, NULL),
             W5500_ERR_NOMEM);
    CHECK_EQ(w5500_dns_init_multi(&d, mem, sizeof(mem), h, server, NULL,
                                  NULL), W5500_ERR_PARAM);
    w5500_dns_set_cache(NULL, 1u);
    w5500_dns_cache_clear(NULL);
    w5500_deinit(h);
}

int main(void)
{
    test_codec();
    test_cache_and_multi();
    test_init_validation();
    if (g_failures == 0) {
        printf("OK: all dns_ex tests passed\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
