/**
 * @file test_stats.c
 * @brief Unit tests for the optional diagnostics counters.
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_registers.h"
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

static void noop_cb(w5500_socket_t sock, w5500_event_t ev,
                    w5500_result_t status, void *arg)
{
    (void)sock;
    (void)ev;
    (void)status;
    (void)arg;
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

static void test_attach_and_spi(void)
{
    w5500_handle_t h = fixture_open();
    w5500_stats_t st;
    uint8_t ver = 0u;
    uint32_t transfers;
    uint32_t bytes;

    CHECK(w5500_get_stats(h) == NULL);
    memset(&st, 0xEE, sizeof(st));
    w5500_stats_reset(&st);
    CHECK_EQ(st.spi_transfers, 0u);
    CHECK_EQ(st.timeouts, 0u);
    CHECK_EQ(w5500_set_stats(h, &st), W5500_OK);
    CHECK(w5500_get_stats(h) == &st);

    CHECK_EQ(w5500_get_hw_version(h, &ver), W5500_OK);
    transfers = st.spi_transfers;
    bytes = st.spi_bytes;
    CHECK(transfers > 0u);
    CHECK(bytes > 0u);
    CHECK_EQ(st.spi_errors, 0u);

    /* Detach: counters must stop moving. */
    CHECK_EQ(w5500_set_stats(h, NULL), W5500_OK);
    CHECK(w5500_get_stats(h) == NULL);
    CHECK_EQ(w5500_get_hw_version(h, &ver), W5500_OK);
    CHECK_EQ(st.spi_transfers, transfers);
    CHECK_EQ(st.spi_bytes, bytes);
    CHECK_EQ(w5500_set_stats(NULL, &st), W5500_ERR_PARAM);
    w5500_stats_reset(NULL);
    w5500_deinit(h);
}

static void test_socket_counters(void)
{
    w5500_handle_t h = fixture_open();
    w5500_stats_t st;
    w5500_socket_t s = NULL;
    static const uint8_t peer[4] = {10, 0, 0, 5};
    uint8_t buf[16];
    uint16_t n = 0u;
    uint16_t sent = 0u;

    w5500_stats_reset(&st);
    CHECK_EQ(w5500_set_stats(h, &st), W5500_OK);
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_UDP, 6000u, noop_cb, NULL, &s),
             W5500_OK);
    CHECK_EQ(st.socket_opens, 1u);

    CHECK_EQ(w5500_socket_sendto(s, peer, 1234u, (const uint8_t *)"hi", 2u,
                                 &sent), W5500_OK);
    CHECK_EQ(sent, 2u);
    CHECK_EQ(st.bytes_sent, 2u);
    CHECK_EQ(st.datagrams_sent, 1u);

    mock_w5500_inject_rx(0, (const uint8_t *)"ok", 2u);
    CHECK_EQ(w5500_socket_recv(s, buf, sizeof(buf), &n), W5500_OK);
    CHECK_EQ(n, 2u);
    CHECK_EQ(st.bytes_received, 2u);
    CHECK_EQ(st.datagrams_received, 1u);

    CHECK_EQ(w5500_socket_close(s), W5500_OK);
    CHECK_EQ(st.socket_closes, 1u);
    w5500_deinit(h);
    CHECK_EQ(g_mock.lock_count, g_mock.unlock_count);
}

static void test_timeout_and_link(void)
{
    w5500_handle_t h = fixture_open();
    w5500_stats_t st;
    w5500_socket_t s = NULL;
    uint32_t before;

    w5500_stats_reset(&st);
    CHECK_EQ(w5500_set_stats(h, &st), W5500_OK);

    /* Baseline link sample, then one real change. */
    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x01u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    before = st.link_changes;
    g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] = 0x07u;
    CHECK_EQ(w5500_poll(h), W5500_OK);
    CHECK(st.link_changes > before);

    /* A stuck command register must produce a bounded timeout. */
    g_mock.cr_stuck = 1u;
    CHECK_EQ(w5500_set_timeout(h, 1u), W5500_OK);
    CHECK_EQ(w5500_socket_open(h, W5500_PROTO_TCP, 5000u, noop_cb, NULL, &s),
             W5500_ERR_TIMEOUT);
    CHECK(st.timeouts > 0u);
    g_mock.cr_stuck = 0u;
    w5500_deinit(h);
    CHECK_EQ(g_mock.lock_count, g_mock.unlock_count);
}

int main(void)
{
    test_attach_and_spi();
    test_socket_counters();
    test_timeout_and_link();
    if (g_failures == 0) {
        printf("OK: all stats tests passed\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
