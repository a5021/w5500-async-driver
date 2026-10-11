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

    /* RX with peer addressing. */
    mock_w5500_inject_rx(0, (const uint8_t *)"ping", 4u);
    /* Overwrite DIPR/DPORT as the chip would latch them... the model keeps
     * whatever was last written; set the peer explicitly. */
    memcpy(&g_mock.sreg[0][W5500_Sn_DIPR], peer, 4u);
    g_mock.sreg[0][W5500_Sn_DPORT] = 0x12u;
    g_mock.sreg[0][W5500_Sn_DPORT + 1u] = 0x34u;

    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK_EQ(g_evt_count, 1u);
    CHECK_EQ(g_evt[0].event, W5500_EVENT_DATA_RECEIVED);

    CHECK_EQ(w5500_socket_recvfrom(s, buf, sizeof(buf), &n, ip, &port),
             W5500_OK);
    CHECK_EQ(n, 4u);
    CHECK(memcmp(buf, "ping", 4) == 0);
    CHECK(memcmp(ip, peer, 4) == 0);
    CHECK_EQ(port, 0x1234u);

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

    /* Second send must return immediately with 0 bytes accepted. */
    n = 0xFFFFu;
    CHECK_EQ(w5500_socket_send(s, blob, 16u, &n), W5500_OK);
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
    test_open_timeout_stuck_cr();
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
