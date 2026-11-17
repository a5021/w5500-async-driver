/**
 * @file test_w5500.c
 * @brief Unit tests of the W5500 core and socket layers against the
 *        host-side chip emulator. Build & run: make test
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_registers.h"
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
/* Event log used by socket callbacks                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    w5500_event_t  event;
    w5500_result_t status;
} evt_log_t;

#define EVT_MAX 16u
static evt_log_t g_evt[EVT_MAX];
static unsigned  g_evt_count;

static void evt_cb(w5500_socket_t sock, w5500_event_t event,
                   w5500_result_t status, void *user_arg)
{
    (void)sock;
    (void)user_arg;
    if (g_evt_count < EVT_MAX) {
        g_evt[g_evt_count].event = event;
        g_evt[g_evt_count].status = status;
        g_evt_count++;
    }
}

static void evt_reset(void)
{
    memset(g_evt, 0, sizeof(g_evt));
    g_evt_count = 0u;
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static w5500_handle_t fixture_open(void)
{
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;

    mock_w5500_plat(&plat);
    CHECK_EQ(w5500_init(&h, &plat), W5500_OK);
    CHECK(h != NULL);
    CHECK_EQ(w5500_set_timeout(h, 50u), W5500_OK);
    return h;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_version(void)
{
    w5500_handle_t h = fixture_open();
    uint8_t ver = 0u;

    CHECK_EQ(w5500_check(h), W5500_OK);
    CHECK_EQ(w5500_get_hw_version(h, &ver), W5500_OK);
    CHECK_EQ(ver, 0x04);

    w5500_deinit(h);
}

static void test_version_burst_path(void)
{
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;

    mock_w5500_plat_burst(&plat);
    CHECK_EQ(w5500_init(&h, &plat), W5500_OK);
    CHECK_EQ(w5500_check(h), W5500_OK);
    w5500_deinit(h);
}

static void test_sw_reset(void)
{
    w5500_handle_t h = fixture_open();

    /* Corrupt something, then reset. */
    CHECK_EQ(w5500_set_ip(h, (const uint8_t[]){1, 2, 3, 4}), W5500_OK);
    CHECK_EQ(w5500_sw_reset(h), W5500_OK);
    CHECK_EQ(g_mock.common[W5500_REG_MR & 0x3Fu] & W5500_MR_RST, 0);
    w5500_deinit(h);
}

static void test_net_config(void)
{
    w5500_handle_t h = fixture_open();
    static const uint8_t ip[4] = {192, 168, 1, 42};
    static const uint8_t mask[4] = {255, 255, 255, 0};
    static const uint8_t gw[4] = {192, 168, 1, 1};
    static const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t back[6];

    CHECK_EQ(w5500_set_ip(h, ip), W5500_OK);
    CHECK_EQ(w5500_set_subnet_mask(h, mask), W5500_OK);
    CHECK_EQ(w5500_set_gateway(h, gw), W5500_OK);
    CHECK_EQ(w5500_set_mac(h, mac), W5500_OK);

    CHECK_EQ(w5500_get_ip(h, back), W5500_OK);
    CHECK(memcmp(back, ip, 4) == 0);
    CHECK_EQ(w5500_get_subnet_mask(h, back), W5500_OK);
    CHECK(memcmp(back, mask, 4) == 0);
    CHECK_EQ(w5500_get_gateway(h, back), W5500_OK);
    CHECK(memcmp(back, gw, 4) == 0);
    CHECK_EQ(w5500_get_mac(h, back), W5500_OK);
    CHECK(memcmp(back, mac, 6) == 0);

    /* No globals: registers really live in the chip model. */
    CHECK(memcmp(&g_mock.common[W5500_REG_SIPR & 0x3Fu], ip, 4) == 0);

    w5500_deinit(h);
}

static void test_buffer_sizes(void)
{
    w5500_handle_t h = fixture_open();
    unsigned i;

    CHECK_EQ(w5500_set_socket_buffer_size(h, 4u), W5500_OK);
    for (i = 0; i < 4u; i++) {
        CHECK_EQ(g_mock.sreg[i][W5500_Sn_TXBUF_SIZE], 4u);
        CHECK_EQ(g_mock.sreg[i][W5500_Sn_RXBUF_SIZE], 4u);
    }

    /* Invalid sizes are rejected. */
    CHECK_EQ(w5500_set_socket_buffer_size(h, 3u), W5500_ERR_PARAM);
    CHECK_EQ(w5500_set_socket_buffer_size(h, 32u), W5500_ERR_PARAM);
    /* 8 KB * 4 sockets exceeds the 16 KB budget per direction. */
    CHECK_EQ(w5500_set_socket_buffer_size(h, 8u), W5500_ERR_PARAM);

    CHECK_EQ(w5500_set_socket_buffer_size(h, 2u), W5500_OK);
    w5500_deinit(h);
}

static void test_link_status(void)
{
    w5500_handle_t h = fixture_open();
    bool up = false;

    CHECK_EQ(w5500_get_link_status(h, &up), W5500_OK);
    CHECK(up == true);
    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x00u;
    CHECK_EQ(w5500_get_link_status(h, &up), W5500_OK);
    CHECK(up == false);
    w5500_deinit(h);
}

static void test_link_info(void)
{
    w5500_handle_t h = fixture_open();
    w5500_link_info_t info;

    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x01u; /* up, 10M, half */
    CHECK_EQ(w5500_get_link_info(h, &info), W5500_OK);
    CHECK_EQ(info.up, 1);
    CHECK_EQ(info.speed_100, 0);
    CHECK_EQ(info.full_duplex, 0);

    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x07u; /* up, 100M, full */
    CHECK_EQ(w5500_get_link_info(h, &info), W5500_OK);
    CHECK_EQ(info.up, 1);
    CHECK_EQ(info.speed_100, 1);
    CHECK_EQ(info.full_duplex, 1);

    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x00u; /* down */
    CHECK_EQ(w5500_get_link_info(h, &info), W5500_OK);
    CHECK_EQ(info.up, 0);
    w5500_deinit(h);
}

static unsigned g_link_calls;
static w5500_link_info_t g_link_last;
static w5500_handle_t g_link_handle;

static void link_cb(w5500_handle_t h, const w5500_link_info_t *info, void *arg)
{
    g_link_calls++;
    g_link_handle = h;
    g_link_last = *info;
    (void)arg;
}

static void test_link_event(void)
{
    w5500_handle_t h = fixture_open();

    g_link_calls = 0u;
    g_link_handle = NULL;
    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x01u;
    CHECK_EQ(w5500_set_link_cb(h, link_cb, NULL), W5500_OK);

    /* First poll only establishes the baseline. */
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_link_calls, 0u);

    /* One change -> exactly one callback with the new state. */
    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x00u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_link_calls, 1u);
    CHECK_EQ(g_link_last.up, 0);
    CHECK(g_link_handle == h);

    /* Steady state -> no further callbacks. */
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_link_calls, 1u);

    /* Speed/duplex change while up is also an event. */
    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x07u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_link_calls, 2u);
    CHECK_EQ(g_link_last.up, 1);
    CHECK_EQ(g_link_last.speed_100, 1);
    CHECK_EQ(g_link_last.full_duplex, 1);

    /* Cleared callback is never invoked. */
    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x00u;
    CHECK_EQ(w5500_set_link_cb(h, NULL, NULL), W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_link_calls, 2u);
    w5500_deinit(h);
}

static void test_irq_pending(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    uint8_t ir = 0xFFu;
    uint8_t sir = 0xFFu;

    CHECK_EQ(w5500_irq_pending(h, &ir, &sir), W5500_OK);
    CHECK_EQ(ir, 0);
    CHECK_EQ(sir, 0);

    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    g_mock.sreg[0][W5500_Sn_IR] = W5500_Sn_IR_RECV;
    CHECK_EQ(w5500_irq_pending(h, &ir, &sir), W5500_OK);
    CHECK_EQ(ir, 0);
    CHECK_EQ(sir, 0x01u); /* socket 0 summary bit */

    /* Poll services and clears the bit. */
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_irq_pending(h, &ir, &sir), W5500_OK);
    CHECK_EQ(sir, 0);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_IR], 0);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

static void test_ping_block(void)
{
    w5500_handle_t h = fixture_open();

    CHECK_EQ(w5500_set_ping_block(h, true), W5500_OK);
    CHECK(g_mock.common[W5500_REG_MR & 0x3Fu] & W5500_MR_PB);
    CHECK_EQ(w5500_set_ping_block(h, false), W5500_OK);
    CHECK_EQ(g_mock.common[W5500_REG_MR & 0x3Fu] & W5500_MR_PB, 0);
    w5500_deinit(h);
}

static void test_check_identity(void)
{
    w5500_handle_t h = fixture_open();
    uint8_t off = (uint8_t)(W5500_REG_RTR & 0x3Fu);

    CHECK_EQ(w5500_check(h), W5500_OK);
    g_mock.common[off + 1u] = 0x00u; /* break the RTR boot value */
    CHECK_EQ(w5500_check(h), W5500_ERR_CHIP);
    g_mock.common[off + 1u] = 0xD0u;
    CHECK_EQ(w5500_check(h), W5500_OK);
    w5500_deinit(h);
}

static void test_retransmit(void)
{
    w5500_handle_t h = fixture_open();
    uint16_t rtr = 0u;
    uint8_t rcr = 0u;

    CHECK_EQ(w5500_get_retransmit(h, &rtr, &rcr), W5500_OK);
    CHECK_EQ(rtr, 0x07D0);
    CHECK_EQ(rcr, 8);
    CHECK_EQ(w5500_set_retransmit(h, 0x0BB8u, 5u), W5500_OK);
    CHECK_EQ(w5500_get_retransmit(h, &rtr, &rcr), W5500_OK);
    CHECK_EQ(rtr, 0x0BB8);
    CHECK_EQ(rcr, 5);
    CHECK_EQ(w5500_set_retransmit(h, 0u, 5u), W5500_ERR_PARAM);
    CHECK_EQ(w5500_set_retransmit(h, 0x07D0u, 8u), W5500_OK); /* restore */
    w5500_deinit(h);
}

static void test_open_flags(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;

    CHECK_EQ(w5500_socket_open_ex(h, W5500_PROTO_TCP, 5000u,
                                  W5500_SOCK_F_NO_DELAY, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_MR], W5500_Sn_MR_TCP | W5500_Sn_MR_ND);
    /* A flagged socket must still behave as plain TCP for the API. */
    CHECK_EQ(w5500_socket_connect(s, (const uint8_t[]){10, 0, 0, 5}, 80u),
             W5500_OK);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);

    CHECK_EQ(w5500_socket_open_ex(h, W5500_PROTO_UDP, 6000u,
                                  W5500_SOCK_F_MULTI |
                                      W5500_SOCK_F_BROADCAST_BLOCK,
                                  evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_MR],
             W5500_Sn_MR_UDP | W5500_Sn_MR_MULTI | W5500_Sn_MR_BCASTB);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);

    /* Protocol/flag mismatches and unknown bits are rejected. */
    CHECK_EQ(w5500_socket_open_ex(h, W5500_PROTO_TCP, 5000u,
                                  W5500_SOCK_F_MULTI, evt_cb, NULL, &s),
             W5500_ERR_PARAM);
    CHECK_EQ(w5500_socket_open_ex(h, W5500_PROTO_UDP, 5000u,
                                  W5500_SOCK_F_NO_DELAY, evt_cb, NULL, &s),
             W5500_ERR_PARAM);
    CHECK_EQ(w5500_socket_open_ex(h, W5500_PROTO_TCP, 5000u, 0x80u, evt_cb,
                                  NULL, &s),
             W5500_ERR_PARAM);

    /* Plain open keeps a clean Sn_MR. */
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_MR], W5500_Sn_MR_TCP);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);

    /* IGMPv2 rides on multicast sockets only. */
    CHECK_EQ(w5500_socket_open_ex(h, W5500_PROTO_UDP, 7000u,
                                  W5500_SOCK_F_MULTI | W5500_SOCK_F_IGMP_V2,
                                  evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_MR],
             W5500_Sn_MR_UDP | W5500_Sn_MR_MULTI | W5500_Sn_MR_MC);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    CHECK_EQ(w5500_socket_open_ex(h, W5500_PROTO_UDP, 7000u,
                                  W5500_SOCK_F_IGMP_V2, evt_cb, NULL, &s),
             W5500_ERR_PARAM);
    CHECK_EQ(w5500_socket_open_ex(h, W5500_PROTO_TCP, 7000u,
                                  W5500_SOCK_F_MULTI | W5500_SOCK_F_IGMP_V2,
                                  evt_cb, NULL, &s),
             W5500_ERR_PARAM);
    w5500_deinit(h);
}

static void test_socket_options(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    w5500_socket_t u = NULL;
    static const uint8_t srv[4] = {10, 0, 0, 5};
    uint16_t mss = 0u;
    uint8_t sr = 0u;

    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_TTL], 128); /* default at open */

    CHECK_EQ(w5500_socket_set_ttl(s, 42u), W5500_OK);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_TTL], 42);
    CHECK_EQ(w5500_socket_set_tos(s, 0x10u), W5500_OK);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_TOS], 0x10);

    CHECK_EQ(w5500_socket_set_mss(s, 536u), W5500_OK);
    CHECK_EQ(w5500_socket_get_mss(s, &mss), W5500_OK);
    CHECK_EQ(mss, 536);

    CHECK_EQ(w5500_socket_get_status(s, &sr), W5500_OK);
    CHECK_EQ(sr, W5500_SOCK_INIT);

    /* Keep-alive probe is TCP-established only. */
    CHECK_EQ(w5500_socket_keepalive_probe(s), W5500_ERR_STATE);
    CHECK_EQ(w5500_socket_connect(s, srv, 80u), W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_socket_keepalive_probe(s), W5500_OK);

    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_UDP, 6000u, evt_cb, NULL, &u),
             W5500_OK);
    CHECK_EQ(w5500_socket_keepalive_probe(u), W5500_ERR_STATE);

    CHECK_EQ(w5500_socket_close(u), W5500_OK);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

static uint32_t g_delay_us_total;
static void test_delay_us_hook(uint32_t us)
{
    g_delay_us_total += us;
}

static void test_delay_us_path(void)
{
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;
    w5500_socket_t s = NULL;
    uint8_t ver = 0u;

    mock_w5500_plat(&plat);
    plat.delay_us = test_delay_us_hook;
    CHECK_EQ(w5500_init(&h, &plat), W5500_OK);
    CHECK_EQ(w5500_set_timeout(h, 20u), W5500_OK);
    CHECK_EQ(w5500_get_hw_version(h, &ver), W5500_OK);

    /* Healthy chip: every wait resolves on the first poll, no delay needed. */
    g_delay_us_total = 0u;
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(g_delay_us_total, 0u);

    /* Stuck command: still bounded by the timeout, stepped at 100 us. */
    g_mock.cr_stuck = 1u;
    g_delay_us_total = 0u;
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5001u, evt_cb, NULL, &s),
             W5500_ERR_TIMEOUT);
    CHECK(g_delay_us_total >= 20000u); /* at least the 20 ms timeout */
    CHECK(g_delay_us_total <= 60000u); /* OPEN wait + best-effort CLOSE wait */
    g_mock.cr_stuck = 0u;
    w5500_deinit(h);
}

static void test_socket_buffers_asym(void)
{
    w5500_handle_t h = fixture_open();
    static const uint8_t kb_ok[4] = {4, 4, 2, 2};
    static const uint8_t kb_sum[4] = {8, 8, 1, 1}; /* 18 KB > 16 KB */
    static const uint8_t kb_bad[4] = {4, 3, 2, 1}; /* 3 KB is not legal */
    uint8_t i;

    CHECK_EQ(w5500_set_socket_buffers(h, kb_ok), W5500_OK);
    for (i = 0; i < 4u; i++) {
        CHECK_EQ(g_mock.sreg[i][W5500_Sn_TXBUF_SIZE], kb_ok[i]);
        CHECK_EQ(g_mock.sreg[i][W5500_Sn_RXBUF_SIZE], kb_ok[i]);
    }
    CHECK_EQ(w5500_set_socket_buffers(h, kb_sum), W5500_ERR_PARAM);
    CHECK_EQ(w5500_set_socket_buffers(h, kb_bad), W5500_ERR_PARAM);
    CHECK_EQ(w5500_set_socket_buffers(h, NULL), W5500_ERR_PARAM);
    /* Rejected calls left the layout alone. */
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_TXBUF_SIZE], 4);
    CHECK_EQ(g_mock.sreg[3][W5500_Sn_RXBUF_SIZE], 2);

    /* Asymmetric TX/RX layout: the two 16 KB memories are independent. */
    {
        static const uint8_t tx_kb[4] = {8, 2, 2, 2}; /* 14 KB TX */
        static const uint8_t rx_kb[4] = {2, 2, 4, 4}; /* 12 KB RX */
        static const uint8_t tx_bad[4] = {8, 8, 1, 1}; /* 18 KB TX */

        CHECK_EQ(w5500_set_socket_buffers_ex(h, tx_kb, rx_kb), W5500_OK);
        for (i = 0; i < 4u; i++) {
            CHECK_EQ(g_mock.sreg[i][W5500_Sn_TXBUF_SIZE], tx_kb[i]);
            CHECK_EQ(g_mock.sreg[i][W5500_Sn_RXBUF_SIZE], rx_kb[i]);
        }
        CHECK_EQ(w5500_set_socket_buffers_ex(h, tx_bad, rx_kb),
                 W5500_ERR_PARAM);
        CHECK_EQ(w5500_set_socket_buffers_ex(h, tx_kb, NULL), W5500_ERR_PARAM);
        /* The rejected request moved nothing. */
        CHECK_EQ(g_mock.sreg[0][W5500_Sn_TXBUF_SIZE], 8);
        CHECK_EQ(g_mock.sreg[2][W5500_Sn_RXBUF_SIZE], 4);
    }
    /* Back to the chip default. */
    CHECK_EQ(w5500_set_socket_buffer_size(h, 2u), W5500_OK);
    w5500_deinit(h);
}

static void test_udp_zero_length(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    uint8_t buf[4];
    uint16_t n = 0xFFFFu;

    evt_reset();
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_UDP, 6000u, evt_cb, NULL, &s),
             W5500_OK);

    /* Datasheet: a zero-length datagram raises RECV with an empty buffer. */
    g_mock.sreg[0][W5500_Sn_IR] |= W5500_Sn_IR_RECV;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 1u);
    CHECK_EQ(g_evt[0].event, W5500_EVENT_DATA_RECEIVED);

    CHECK_EQ(w5500_socket_recv(s, buf, sizeof(buf), &n), W5500_OK);
    CHECK_EQ(n, 0u);
    CHECK_EQ(w5500_socket_rx_available(s, (uint16_t *)&n), W5500_OK);
    CHECK_EQ(n, 0u);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

static void test_udp_partial_read(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    static const uint8_t datagram[4] = {1, 2, 3, 4};
    uint8_t buf[8];
    uint16_t n = 0u;

    evt_reset();
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_UDP, 6000u, evt_cb, NULL, &s),
             W5500_OK);
    mock_w5500_inject_rx(0, datagram, sizeof(datagram));
    CHECK_EQ(w5500_poll(h), W5500_OK);

    /* Read the 4-byte datagram in two chunks: the 8-byte UDP header is
       consumed once, not per read. */
    CHECK_EQ(w5500_socket_recv(s, buf, 2u, &n), W5500_OK);
    CHECK_EQ(n, 2u);
    CHECK_EQ(buf[0], 1);
    CHECK_EQ(buf[1], 2);
    CHECK_EQ(w5500_socket_rx_available(s, &n), W5500_OK);
    CHECK_EQ(n, 2u);
    CHECK_EQ(w5500_socket_recv(s, buf, sizeof(buf), &n), W5500_OK);
    CHECK_EQ(n, 2u);
    CHECK_EQ(buf[0], 3);
    CHECK_EQ(buf[1], 4);
    CHECK_EQ(w5500_socket_rx_available(s, &n), W5500_OK);
    CHECK_EQ(n, 0u);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

static void test_open_timeout_stuck_cr(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;

    g_mock.cr_stuck = 1u;
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_ERR_TIMEOUT);
    CHECK(s == NULL);
    /* The failed attempt must not leak the socket slot. */
    CHECK_EQ(w5500_poll(h), W5500_OK);

    g_mock.cr_stuck = 0u;
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK(s != NULL);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

static void test_close_timeout_releases_slot(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;

    /* Open a working socket first. */
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK(s != NULL);

    /* Make Sn_CR stick so close() times out. */
    g_mock.cr_stuck = 1u;
    CHECK_EQ(w5500_socket_close(s), W5500_ERR_TIMEOUT);
    g_mock.cr_stuck = 0u;

    /* The slot must have been released despite the timeout: a fresh open
       must still succeed instead of returning ERR_NOMEM. */
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK(s != NULL);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

static void test_open_rejects_bad_args(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;

    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 1u, NULL, NULL, &s),
             W5500_ERR_PARAM);
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 1u, evt_cb, NULL, NULL),
             W5500_ERR_PARAM);
    CHECK_EQ(w5500_socket_open(NULL, W5500_PROTO_TCP, 1u, evt_cb, NULL, &s),
             W5500_ERR_PARAM);
    w5500_deinit(h);
}

static void test_tcp_client_flow(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    static const uint8_t srv[4] = {10, 0, 0, 5};
    uint16_t n = 0u;
    uint8_t buf[16];

    evt_reset();
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(w5500_socket_connect(s, srv, 80u), W5500_OK);

    /* CONNECT already succeeded in the model => CON pending. */
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 1u);
    CHECK_EQ(g_evt[0].event, W5500_EVENT_CONNECTED);

    /* Send: goes straight into the chip TX buffer. */
    CHECK_EQ(w5500_socket_send(s, (const uint8_t *)"world", 5u, &n),
             W5500_OK);
    CHECK_EQ(n, 5u);
    CHECK_EQ(g_mock.last_sent_len[0], 5u);
    CHECK(memcmp(g_mock.last_sent[0], "world", 5) == 0);

    CHECK_EQ(w5500_poll(h), W5500_OK); /* SEND_OK => SEND_DONE */
    CHECK_EQ(g_evt_count, 2u);
    CHECK_EQ(g_evt[1].event, W5500_EVENT_SEND_DONE);

    /* Receive: injected data is streamed zero-copy into the user buffer. */
    mock_w5500_inject_rx(0, (const uint8_t *)"hello", 5u);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 3u);
    CHECK_EQ(g_evt[2].event, W5500_EVENT_DATA_RECEIVED);

    memset(buf, 0, sizeof(buf));
    CHECK_EQ(w5500_socket_recv(s, buf, sizeof(buf), &n), W5500_OK);
    CHECK_EQ(n, 5u);
    CHECK(memcmp(buf, "hello", 5) == 0);
    CHECK_EQ(w5500_socket_recv(s, buf, sizeof(buf), &n), W5500_OK);
    CHECK_EQ(n, 0u); /* drained */

    /* Peer closes => DISCONNECTED and the slot is recycled. */
    mock_w5500_remote_disconnect(0);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 4u);
    CHECK_EQ(g_evt[3].event, W5500_EVENT_DISCONNECTED);
    CHECK_EQ(g_evt[3].status, W5500_OK);
    CHECK_EQ(w5500_socket_close(s), W5500_ERR_PARAM); /* handle is dead */

    w5500_deinit(h);
}

static void test_tcp_connect_fail(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    static const uint8_t srv[4] = {10, 0, 0, 9};

    evt_reset();
    g_mock.connect_ok = 0u;
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(w5500_socket_connect(s, srv, 80u), W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 1u);
    CHECK_EQ(g_evt[0].event, W5500_EVENT_ERROR);
    CHECK_EQ(g_evt[0].status, W5500_ERR_TIMEOUT);
    g_mock.connect_ok = 1u;
    w5500_deinit(h);
}

static void test_tcp_server_flow(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    static const uint8_t peer[4] = {192, 168, 1, 10};
    uint8_t ip[4];
    uint16_t port = 0u;

    evt_reset();
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 80u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(w5500_socket_listen(s), W5500_OK);
    CHECK_EQ(mock_w5500_reg(0, W5500_Sn_SR), W5500_SOCK_LISTEN);

    mock_w5500_remote_connect(0, peer, 4321u);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 1u);
    CHECK_EQ(g_evt[0].event, W5500_EVENT_CONNECTED);

    CHECK_EQ(w5500_socket_get_peer(s, ip, &port), W5500_OK);
    CHECK(memcmp(ip, peer, 4) == 0);
    CHECK_EQ(port, 4321u);

    /* Graceful disconnect completes asynchronously. */
    CHECK_EQ(w5500_socket_disconnect(s), W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 2u);
    CHECK_EQ(g_evt[1].event, W5500_EVENT_DISCONNECTED);
    w5500_deinit(h);
}

static void test_udp_flow(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    static const uint8_t peer[4] = {192, 168, 1, 77};
    uint8_t buf[16];
    uint8_t ip[4];
    uint16_t port = 0u;
    uint16_t n = 0u;

    evt_reset();
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_UDP, 5000u, evt_cb, NULL, &s),
             W5500_OK);

    /* RX with peer addressing. The emulator's 8-byte UDP header carries
       the sender (10.0.0.1:5000), which the driver reads into ip/port. */
    mock_w5500_inject_rx(0, (const uint8_t *)"ping", 4u);

    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 1u);
    CHECK_EQ(g_evt[0].event, W5500_EVENT_DATA_RECEIVED);

    CHECK_EQ(w5500_socket_recvfrom(s, buf, sizeof(buf), &n, ip, &port),
             W5500_OK);
    CHECK_EQ(n, 4u);
    CHECK(memcmp(buf, "ping", 4) == 0);
    CHECK_EQ(ip[0], 10u);
    CHECK_EQ(ip[3], 1u);
    CHECK_EQ(port, 5000u);

    /* TX to an explicit destination. */
    CHECK_EQ(w5500_socket_sendto(s, peer, 9999u, (const uint8_t *)"pong", 4u,
                                 &n),
             W5500_OK);
    CHECK_EQ(n, 4u);
    CHECK_EQ(g_mock.last_sent_len[0], 4u);
    CHECK(memcmp(g_mock.last_sent[0], "pong", 4) == 0);
    CHECK(memcmp(&g_mock.sreg[0][W5500_Sn_DIPR], peer, 4) == 0);

    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

static void test_tx_full_is_nonblocking(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    static const uint8_t srv[4] = {10, 0, 0, 5};
    uint8_t blob[2048];
    uint16_t n = 0u;

    evt_reset();
    memset(blob, 'A', sizeof(blob));
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(w5500_socket_connect(s, srv, 80u), W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);

    /* Default buffers are 2 KB; first send fills the TX buffer. */
    g_mock.send_stall = 1u; /* hold the data in the chip until completed */
    CHECK_EQ(w5500_socket_send(s, blob, (uint16_t)sizeof(blob), &n),
             W5500_OK);
    CHECK_EQ(n, (uint16_t)sizeof(blob));

    /* Second send while the first SEND is in flight: refused with BUSY. */
    n = 0xFFFFu;
    CHECK_EQ(w5500_socket_send(s, blob, 16u, &n), W5500_ERR_BUSY);
    CHECK_EQ(n, 0u);

    /* After SEND completes (poll consumes SEND_OK) space is free again. */
    g_mock.send_stall = 0u;
    mock_w5500_complete_send(0);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_socket_send(s, blob, 16u, &n), W5500_OK);
    CHECK_EQ(n, 16u);
    w5500_deinit(h);
}

static void test_static_init(void)
{
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;
    static uint8_t storage[1024];

    mock_w5500_plat(&plat);
    CHECK(w5500_context_size() <= sizeof(storage));
    CHECK_EQ(w5500_init_static(&h, storage, sizeof(storage), &plat),
             W5500_OK);
    CHECK_EQ(w5500_check(h), W5500_OK);
    w5500_deinit(h); /* must not free static storage */

    /* Too-small storage is rejected. */
    h = NULL;
    CHECK_EQ(w5500_init_static(&h, storage, 8u, &plat), W5500_ERR_NOMEM);
}

static void test_lock_balance(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;

    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_UDP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);

    CHECK(g_mock.lock_count > 0u);
    CHECK_EQ(g_mock.lock_count, g_mock.unlock_count);
}

static void test_half_close_keeps_data(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    static const uint8_t srv[4] = {10, 0, 0, 5};
    uint8_t buf[8];
    uint16_t n = 0u;

    evt_reset();
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(w5500_socket_connect(s, srv, 80u), W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 1u); /* CONNECTED */

    /* Peer sends 3 bytes and then FIN in the same window. */
    mock_w5500_inject_rx(0, (const uint8_t *)"abc", 3u);
    mock_w5500_remote_disconnect(0);
    CHECK_EQ(w5500_poll(h), W5500_OK);

    /* DATA_RECEIVED must arrive with the slot still alive. */
    CHECK_EQ(g_evt_count, 2u);
    CHECK_EQ(g_evt[1].event, W5500_EVENT_DATA_RECEIVED);
    CHECK_EQ(w5500_socket_recv(s, buf, sizeof(buf), &n), W5500_OK);
    CHECK_EQ(n, 3u);
    CHECK_EQ(memcmp(buf, "abc", 3), 0);

    /* Only after the RX buffer is empty does the close event arrive. */
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 3u);
    CHECK_EQ(g_evt[2].event, W5500_EVENT_DISCONNECTED);
    CHECK_EQ(w5500_socket_recv(s, buf, sizeof(buf), &n), W5500_ERR_PARAM);
    w5500_deinit(h);
}

static void test_send_done_recovery(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    static const uint8_t srv[4] = {10, 0, 0, 5};
    uint8_t blob[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint16_t n = 0u;

    evt_reset();
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(w5500_socket_connect(s, srv, 80u), W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);

    g_mock.send_stall = 1u;
    CHECK_EQ(w5500_socket_send(s, blob, 4u, &n), W5500_OK);
    CHECK_EQ(n, 4u);
    CHECK_EQ(w5500_socket_send(s, blob, 4u, &n), W5500_ERR_BUSY);
    CHECK_EQ(n, 0u);

    /* The chip finishes the frame, but the SEND_OK interrupt is lost. */
    g_mock.send_stall = 0u;
    mock_w5500_complete_send(0);
    g_mock.sreg[0][W5500_Sn_IR] = 0u;
    evt_reset();
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 1u);
    CHECK_EQ(g_evt[0].event, W5500_EVENT_SEND_DONE);

    /* The socket is usable again. */
    CHECK_EQ(w5500_socket_send(s, blob, 4u, &n), W5500_OK);
    CHECK_EQ(n, 4u);
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

static void test_poll_read_budget(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t u = NULL;
    w5500_socket_t t = NULL;

    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_UDP, 6000u, evt_cb, NULL, &u),
             W5500_OK);
    g_mock.bus_reads = 0u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    /* SIR + IR + PHYCFGR; UDP has no passive close, so no Sn_SR read. */
    CHECK_EQ(g_mock.bus_reads, 3u);
    CHECK_EQ(w5500_socket_close(u), W5500_OK);

    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &t),
             W5500_OK);
    g_mock.bus_reads = 0u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    /* One extra read: Sn_SR for the passive-close fallback. */
    CHECK_EQ(g_mock.bus_reads, 4u);
    CHECK_EQ(w5500_socket_close(t), W5500_OK);
    w5500_deinit(h);
}

static void test_timeout_discards_data(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    static const uint8_t srv[4] = {10, 0, 0, 5};
    uint8_t buf[4];
    uint16_t n = 0u;

    evt_reset();
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(w5500_socket_connect(s, srv, 80u), W5500_OK);
    CHECK_EQ(w5500_poll(h), W5500_OK);
    evt_reset();

    /* Buffered data plus a fatal TIMEOUT: the error wins, data is dropped. */
    mock_w5500_inject_rx(0, (const uint8_t *)"xy", 2u);
    g_mock.sreg[0][W5500_Sn_IR] |= W5500_Sn_IR_TIMEOUT;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 1u);
    CHECK_EQ(g_evt[0].event, W5500_EVENT_ERROR);
    CHECK_EQ(g_evt[0].status, W5500_ERR_TIMEOUT);
    CHECK_EQ(w5500_socket_recv(s, buf, sizeof(buf), &n), W5500_ERR_PARAM);
    w5500_deinit(h);
}

/* ------------------------------------------------------------------ */

static void test_unstable_counter_timeout(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    uint16_t n = 0u;

    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, evt_cb, NULL, &s),
             W5500_OK);
    /* A counter that never returns two equal samples must produce ERR_TIMEOUT
       instead of hanging in read16_stable(). */
    g_mock.unstable_counter = 1u;
    g_mock.unstable_phase = 0u;
    CHECK_EQ(w5500_socket_rx_available(s, &n), W5500_ERR_TIMEOUT);
    CHECK_EQ(w5500_socket_tx_free(s, &n), W5500_ERR_TIMEOUT);
    g_mock.unstable_counter = 0u;
    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

static void test_udp_header_errors(void)
{
    w5500_handle_t h = fixture_open();
    w5500_socket_t s = NULL;
    uint8_t bad[8];
    uint16_t n = 0u;

    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_UDP, 6000u, evt_cb, NULL, &s),
             W5500_OK);

    /* A truncated header (4 bytes in RX) must be rejected. No IR poke:
       the callback in evt_cb() would drain RX during poll(). */
    {
        uint16_t wr = (uint16_t)(((uint16_t)g_mock.sreg[0][W5500_Sn_RX_WR] << 8) |
                                  g_mock.sreg[0][W5500_Sn_RX_WR + 1u]);
        memcpy(&g_mock.rx_mem[0][wr & 0x7FFu], "\x01\x02\x03\x04", 4u);
        g_mock.sreg[0][W5500_Sn_RX_WR + 1u] = (uint8_t)((wr + 4u) & 0xFFu);
        g_mock.sreg[0][W5500_Sn_RX_WR] = (uint8_t)(((wr + 4u) >> 8) & 0xFFu);
    }
    CHECK_EQ(w5500_socket_recv(s, bad, sizeof(bad), &n), W5500_ERR_STATE);

    /* A full header whose length field exceeds the RX data: inconsistent. */
    {
        uint16_t rd = (uint16_t)(((uint16_t)g_mock.sreg[0][W5500_Sn_RX_RD] << 8) |
                                  g_mock.sreg[0][W5500_Sn_RX_RD + 1u]);
        memset(bad, 0, sizeof(bad));
        bad[6] = 0xFFu;
        bad[7] = 0xFFu;
        memcpy(&g_mock.rx_mem[0][rd & 0x7FFu], bad, 8u);
        g_mock.sreg[0][W5500_Sn_RX_WR + 1u] = (uint8_t)((rd + 8u) & 0xFFu);
        g_mock.sreg[0][W5500_Sn_RX_WR] = (uint8_t)(((rd + 8u) >> 8) & 0xFFu);
    }
    CHECK_EQ(w5500_socket_rx_available(s, &n), W5500_OK);
    CHECK_EQ(n, 8u);
    CHECK_EQ(w5500_socket_recv(s, bad, sizeof(bad), &n), W5500_ERR_STATE);

    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    w5500_deinit(h);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    mock_w5500_reset();

    test_version();
    test_version_burst_path();
    test_sw_reset();
    test_net_config();
    test_buffer_sizes();
    test_link_status();
    test_link_info();
    test_link_event();
    test_irq_pending();
    test_ping_block();
    test_check_identity();
    test_retransmit();
    test_open_flags();
    test_socket_options();
    test_delay_us_path();
    test_socket_buffers_asym();
    test_udp_zero_length();
    test_udp_partial_read();
    test_half_close_keeps_data();
    test_timeout_discards_data();
    test_unstable_counter_timeout();
    test_udp_header_errors();
    test_send_done_recovery();
    test_poll_read_budget();
    test_open_timeout_stuck_cr();
    test_close_timeout_releases_slot();
    test_open_rejects_bad_args();
    test_tcp_client_flow();
    test_tcp_connect_fail();
    test_tcp_server_flow();
    test_udp_flow();
    test_tx_full_is_nonblocking();
    test_static_init();
    test_lock_balance();

    if (g_failures == 0) {
        printf("OK: all tests passed\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
