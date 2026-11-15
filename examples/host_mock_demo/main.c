/**
 * @file main.c
 * @brief Host-runnable demo: the whole stack against the W5500 emulator.
 *
 * Exercises the public API end to end without hardware: DHCP
 * (DISCOVER/OFFER/REQUEST/ACK), DNS (A query), then a TCP request/reply.
 * Exit code 0 means every assertion held; CI runs this through
 * `make examples`.
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_dhcp.h"
#include "w5500_dns.h"
#include "mock_w5500.h"

static int g_failures;
static unsigned g_events;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ------------------------------------------------------------------ */
/* DHCP                                                                */
/* ------------------------------------------------------------------ */

static unsigned g_dhcp_events;
static w5500_dhcp_event_t g_dhcp_event;

static void on_dhcp(w5500_dhcp_t *d, w5500_dhcp_event_t ev, void *arg)
{
    (void)d;
    (void)arg;
    g_dhcp_events++;
    g_dhcp_event = ev;
}

static uint16_t build_dhcp_reply(uint8_t *b, uint8_t type, const uint8_t xid[4],
                                 const uint8_t ip[4])
{
    static const uint8_t mask[4] = {255, 255, 255, 0};
    static const uint8_t gw[4] = {10, 0, 0, 1};
    static const uint8_t dns[4] = {10, 0, 0, 2};
    uint16_t off = 240u;

    memset(b, 0, 320);
    b[0] = 2u; /* BOOTREPLY */
    b[1] = 1u;
    b[2] = 6u;
    memcpy(b + 4, xid, 4u);
    memcpy(b + 16, ip, 4u); /* yiaddr */
    b[236] = 0x63u;
    b[237] = 0x82u;
    b[238] = 0x53u;
    b[239] = 0x63u;

    b[off++] = 53u;
    b[off++] = 1u;
    b[off++] = type;
    b[off++] = 54u; /* server id */
    b[off++] = 4u;
    memcpy(b + off, gw, 4u);
    off = (uint16_t)(off + 4u);
    b[off++] = 51u; /* lease time */
    b[off++] = 4u;
    put32(b + off, 3600u);
    off = (uint16_t)(off + 4u);
    b[off++] = 1u;
    b[off++] = 4u;
    memcpy(b + off, mask, 4u);
    off = (uint16_t)(off + 4u);
    b[off++] = 3u;
    b[off++] = 4u;
    memcpy(b + off, gw, 4u);
    off = (uint16_t)(off + 4u);
    b[off++] = 6u;
    b[off++] = 4u;
    memcpy(b + off, dns, 4u);
    off = (uint16_t)(off + 4u);
    b[off++] = 255u;
    return off;
}

/* ------------------------------------------------------------------ */
/* DNS                                                                 */
/* ------------------------------------------------------------------ */

static unsigned g_dns_results;
static w5500_result_t g_dns_status;
static uint8_t g_dns_ip[4];

static void on_dns(w5500_dns_t *d, const uint8_t ip[4], w5500_result_t status,
                   void *arg)
{
    (void)d;
    (void)arg;
    g_dns_results++;
    g_dns_status = status;
    memcpy(g_dns_ip, ip, 4u);
}

static uint16_t build_dns_answer(uint8_t *b, uint16_t qid, const uint8_t ip[4])
{
    static const uint8_t qname[] = {7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
                                    3, 'c', 'o', 'm', 0};
    uint16_t off = 12u;

    memset(b, 0, 256);
    put16(b, qid);
    b[2] = 0x81u;
    b[3] = 0x80u;
    b[5] = 1u;
    b[7] = 1u;
    memcpy(b + off, qname, sizeof(qname));
    off = (uint16_t)(off + sizeof(qname));
    b[off++] = 0u;
    b[off++] = 1u;
    b[off++] = 0u;
    b[off++] = 1u;

    b[off++] = 0xC0u;
    b[off++] = 0x0Cu;
    b[off++] = 0u;
    b[off++] = 1u;
    b[off++] = 0u;
    b[off++] = 1u;
    put32(b + off, 60u);
    off = (uint16_t)(off + 4u);
    b[off++] = 0u;
    b[off++] = 4u;
    memcpy(b + off, ip, 4u);
    off = (uint16_t)(off + 4u);
    return off;
}

/* ------------------------------------------------------------------ */
/* TCP                                                                 */
/* ------------------------------------------------------------------ */

static unsigned g_tcp_connected;
static unsigned g_tcp_data;

static void on_tcp(w5500_socket_t s, w5500_event_t ev, w5500_result_t status,
                   void *arg)
{
    uint8_t buf[64];
    uint16_t n;

    (void)status;
    (void)arg;
    g_events++;
    if (ev == W5500_EVENT_CONNECTED) {
        g_tcp_connected++;
    } else if (ev == W5500_EVENT_DATA_RECEIVED) {
        while (w5500_socket_recv(s, buf, sizeof(buf), &n) == W5500_OK && n) {
            g_tcp_data += n;
        }
    }
}

static void pump(w5500_handle_t h)
{
    unsigned i;

    for (i = 0; i < 4u; i++) {
        (void)w5500_poll(h);
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    static uint8_t dhcp_mem[2048];
    static uint8_t dns_mem[2048];
    static const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    static const uint8_t lease_ip[4] = {10, 0, 0, 50};
    static const uint8_t answer_ip[4] = {93, 184, 216, 34};
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;
    w5500_dhcp_t *dhcp = NULL;
    w5500_dns_t *dns = NULL;
    w5500_socket_t tcp = NULL;
    uint8_t rx[320];
    uint8_t xid[4];
    uint8_t cfg_ip[4];
    uint8_t dns_srv[4];
    uint16_t len;
    uint16_t sent;
    uint16_t qid;

    mock_w5500_reset();
    mock_w5500_plat(&plat);
    CHECK(w5500_init(&h, &plat) == W5500_OK);
    CHECK(w5500_check(h) == W5500_OK);
    CHECK(w5500_set_mac(h, mac) == W5500_OK);

    /* --- DHCP ------------------------------------------------------- */
    g_dhcp_events = 0u;
    CHECK(w5500_dhcp_init(&dhcp, dhcp_mem, sizeof(dhcp_mem), h, mac, on_dhcp,
                          NULL) == W5500_OK);
    CHECK(w5500_dhcp_start(dhcp, 0u) == W5500_OK);
    memcpy(xid, g_mock.last_sent[0] + 4, 4u);
    pump(h);

    len = build_dhcp_reply(rx, 2u, xid, lease_ip); /* OFFER */
    mock_w5500_inject_rx(0, rx, len);
    CHECK(w5500_dhcp_poll(dhcp, 0u) == W5500_OK);
    pump(h);
    /* The client answered with REQUEST (option 53 = 3). */
    CHECK(g_mock.last_sent[0][240] == 53u);
    CHECK(g_mock.last_sent[0][242] == 3u);

    len = build_dhcp_reply(rx, 5u, xid, lease_ip); /* ACK */
    mock_w5500_inject_rx(0, rx, len);
    CHECK(w5500_dhcp_poll(dhcp, 0u) == W5500_OK);
    CHECK(g_dhcp_events == 1u && g_dhcp_event == W5500_DHCP_EVENT_BOUND);
    CHECK(w5500_dhcp_get_config(dhcp, cfg_ip, NULL, NULL, dns_srv) == W5500_OK);
    CHECK(memcmp(cfg_ip, lease_ip, 4) == 0);
    printf("dhcp: bound %u.%u.%u.%u (dns %u.%u.%u.%u)\n", cfg_ip[0], cfg_ip[1],
           cfg_ip[2], cfg_ip[3], dns_srv[0], dns_srv[1], dns_srv[2],
           dns_srv[3]);

    /* --- DNS -------------------------------------------------------- */
    g_dns_results = 0u;
    CHECK(w5500_dns_init(&dns, dns_mem, sizeof(dns_mem), h, dns_srv, on_dns,
                         NULL) == W5500_OK);
    CHECK(w5500_dns_start(dns, "example.com", 0u) == W5500_OK);
    qid = (uint16_t)(((uint16_t)g_mock.last_sent[1][0] << 8) |
                     g_mock.last_sent[1][1]);
    pump(h);
    len = build_dns_answer(rx, qid, answer_ip);
    mock_w5500_inject_rx(1, rx, len);
    CHECK(w5500_dns_poll(dns, 0u) == W5500_OK);
    CHECK(g_dns_results == 1u && g_dns_status == W5500_OK);
    CHECK(memcmp(g_dns_ip, answer_ip, 4) == 0);
    printf("dns: example.com -> %u.%u.%u.%u\n", g_dns_ip[0], g_dns_ip[1],
           g_dns_ip[2], g_dns_ip[3]);

    /* --- TCP -------------------------------------------------------- */
    g_events = 0u;
    g_tcp_connected = 0u;
    g_tcp_data = 0u;
    CHECK(w5500_socket_open_ex(h, W5500_PROTO_TCP, 40000u,
                               W5500_SOCK_F_NO_DELAY, on_tcp, NULL,
                               &tcp) == W5500_OK);
    CHECK(w5500_socket_connect(tcp, g_dns_ip, 80u) == W5500_OK);
    pump(h);
    CHECK(g_tcp_connected == 1u);
    CHECK(w5500_socket_send(tcp, (const uint8_t *)"GET / HTTP/1.0\r\n\r\n",
                            18u, &sent) == W5500_OK);
    CHECK(sent == 18u);
    pump(h);

    len = (uint16_t)snprintf((char *)rx, sizeof(rx), "HTTP/1.0 200 OK\r\n\r\nhi");
    mock_w5500_inject_rx(2, rx, len);
    pump(h);
    CHECK(g_tcp_data == len);
    printf("tcp: connected, sent %u, received %u bytes\n", sent, g_tcp_data);

    CHECK(w5500_socket_close(tcp) == W5500_OK);
    w5500_dhcp_stop(dhcp);
    w5500_dns_stop(dns);
    w5500_deinit(h);

    if (g_failures == 0) {
        printf("OK: host demo passed\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
