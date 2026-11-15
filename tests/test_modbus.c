/**
 * @file test_modbus.c
 * @brief Unit tests for the Modbus TCP codec, server and client against the
 *        chip emulator. Build & run: make test
 */
#include <stdio.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_registers.h"
#include "w5500_modbus.h"
#include "../src/w5500_modbus_internal.h"
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

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void test_parse_read(void)
{
    /* FC3: txid 1, unit 1, read 2 registers at 0x10. */
    static const uint8_t req[12] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
                                    0x01, 0x03, 0x00, 0x10, 0x00, 0x02};
    w5500_mb_adu_t a;

    CHECK_EQ(w5500_mb_parse_adu(req, sizeof(req), &a), 0);
    CHECK_EQ(a.txid, 1);
    CHECK_EQ(a.unit, 1);
    CHECK_EQ(a.fc, W5500_MB_FC_READ_HOLDING);
    CHECK_EQ(a.addr, 0x10);
    CHECK_EQ(a.count, 2);
    CHECK_EQ(a.frame_len, 12);

    /* FC4 uses the same layout. */
    {
        uint8_t r4[12];
        memcpy(r4, req, sizeof(r4));
        r4[7] = W5500_MB_FC_READ_INPUT;
        CHECK_EQ(w5500_mb_parse_adu(r4, sizeof(r4), &a), 0);
        CHECK_EQ(a.fc, W5500_MB_FC_READ_INPUT);
        CHECK_EQ(a.count, 2);
    }
}

static void test_parse_writes(void)
{
    /* FC6: write 0xBEEF at 0x20. */
    static const uint8_t single[12] = {0x00, 0x02, 0x00, 0x00, 0x00, 0x06,
                                       0x01, 0x06, 0x00, 0x20, 0xBE, 0xEF};
    /* FC16: write 2 registers at 0x30. */
    static const uint8_t multi[17] = {0x00, 0x03, 0x00, 0x00, 0x00, 0x0B,
                                      0x01, 0x10, 0x00, 0x30, 0x00, 0x02,
                                      0x04, 0x11, 0x22, 0x33, 0x44};
    w5500_mb_adu_t a;

    CHECK_EQ(w5500_mb_parse_adu(single, sizeof(single), &a), 0);
    CHECK_EQ(a.fc, W5500_MB_FC_WRITE_SINGLE);
    CHECK_EQ(a.addr, 0x20);
    CHECK_EQ(a.value, 0xBEEF);
    CHECK_EQ(a.count, 1);

    CHECK_EQ(w5500_mb_parse_adu(multi, sizeof(multi), &a), 0);
    CHECK_EQ(a.fc, W5500_MB_FC_WRITE_MULTIPLE);
    CHECK_EQ(a.addr, 0x30);
    CHECK_EQ(a.count, 2);
    CHECK_EQ(a.data_len, 4);
    CHECK_EQ(rd16(a.data), 0x1122);
    CHECK_EQ(a.frame_len, 17);
}

static void test_parse_rejects(void)
{
    static const uint8_t req[12] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
                                    0x01, 0x03, 0x00, 0x10, 0x00, 0x02};
    uint8_t bad[17];
    w5500_mb_adu_t a;

    CHECK_EQ(w5500_mb_parse_adu(req, 11u, &a), -1); /* truncated */
    CHECK_EQ(w5500_mb_parse_adu(req, sizeof(req), NULL), -1);

    memcpy(bad, req, sizeof(req));
    bad[3] = 1u; /* protocol id != 0 */
    CHECK_EQ(w5500_mb_parse_adu(bad, sizeof(req), &a), -1);

    memcpy(bad, req, sizeof(req));
    bad[5] = 1u; /* MBAP length too short */
    CHECK_EQ(w5500_mb_parse_adu(bad, sizeof(req), &a), -1);

    memcpy(bad, req, sizeof(req));
    bad[11] = 0u; /* register count 0 */
    CHECK_EQ(w5500_mb_parse_adu(bad, sizeof(req), &a), -1);

    memcpy(bad, req, sizeof(req));
    bad[10] = 0x00;
    bad[11] = 126u; /* > 125 */
    CHECK_EQ(w5500_mb_parse_adu(bad, sizeof(req), &a), -1);

    memcpy(bad, req, sizeof(req));
    bad[5] = 7u; /* FC3 must be exactly 6 */
    CHECK_EQ(w5500_mb_parse_adu(bad, sizeof(req), &a), -1);

    /* FC16 with a byte count that disagrees with the register count. */
    memcpy(bad, req, sizeof(req));
    bad[5] = 0x0B;
    bad[7] = W5500_MB_FC_WRITE_MULTIPLE;
    bad[11] = 2u;
    bad[12] = 2u; /* should be 4 */
    CHECK_EQ(w5500_mb_parse_adu(bad, 13u, &a), -1);

    /* Unknown function code is a valid frame (exception territory). */
    memcpy(bad, req, sizeof(req));
    bad[7] = 0x2Bu;
    CHECK_EQ(w5500_mb_parse_adu(bad, sizeof(req), &a), 0);
    CHECK_EQ(a.fc, 0x2B);
}

static void test_builders(void)
{
    uint8_t out[W5500_MB_FRAME_MAX];
    static const uint16_t regs[2] = {0x0102u, 0xA0B0u};
    uint16_t n;

    n = w5500_mb_build_exception(out, sizeof(out), 0x1234u, 1u, 3u,
                                 W5500_MB_EX_ILLEGAL_ADDRESS);
    CHECK_EQ(n, 9);
    CHECK_EQ(rd16(out), 0x1234);
    CHECK_EQ(rd16(out + 2), 0);
    CHECK_EQ(rd16(out + 4), 3);
    CHECK_EQ(out[7], 0x83);
    CHECK_EQ(out[8], 2);

    n = w5500_mb_build_read_response(out, sizeof(out), 7u, 1u,
                                     W5500_MB_FC_READ_HOLDING, regs, 2u);
    CHECK_EQ(n, 13);
    CHECK_EQ(rd16(out + 4), 7);
    CHECK_EQ(out[7], 3);
    CHECK_EQ(out[8], 4);
    CHECK_EQ(rd16(out + 9), 0x0102);
    CHECK_EQ(rd16(out + 11), 0xA0B0);

    n = w5500_mb_build_write_response(out, sizeof(out), 9u, 1u,
                                      W5500_MB_FC_WRITE_MULTIPLE, 0x30u, 2u);
    CHECK_EQ(n, 12);
    CHECK_EQ(out[7], 16);
    CHECK_EQ(rd16(out + 8), 0x30);
    CHECK_EQ(rd16(out + 10), 2);
}

/* ------------------------------------------------------------------ */
/* Fixture + server                                                    */
/* ------------------------------------------------------------------ */

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

static uint16_t g_hold[64];
static uint16_t g_inp[32];
static unsigned g_writes;

static w5500_result_t map_read(uint8_t unit, uint8_t fc, uint16_t addr,
                               uint16_t count, uint16_t *regs, void *user)
{
    const uint16_t *src;
    uint16_t cap;
    uint16_t i;

    (void)user;
    if (unit != 1u) {
        return W5500_ERR_PARAM;
    }
    if (fc == W5500_MB_FC_READ_HOLDING) {
        src = g_hold;
        cap = 64u;
    } else if (fc == W5500_MB_FC_READ_INPUT) {
        src = g_inp;
        cap = 32u;
    } else {
        return W5500_ERR_PARAM;
    }
    if ((uint32_t)addr + (uint32_t)count > (uint32_t)cap) {
        return W5500_ERR_PARAM;
    }
    for (i = 0; i < count; i++) {
        regs[i] = src[addr + i];
    }
    return W5500_OK;
}

static w5500_result_t map_write(uint8_t unit, uint16_t addr, uint16_t count,
                                const uint16_t *regs, void *user)
{
    (void)user;
    if (unit != 1u) {
        return W5500_ERR_PARAM;
    }
    if ((uint32_t)addr + (uint32_t)count > 64u) {
        return W5500_ERR_PARAM;
    }
    memcpy(&g_hold[addr], regs, (size_t)count * sizeof(uint16_t));
    g_writes++;
    return W5500_OK;
}

static void put16b(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t mk_read(uint8_t *b, uint16_t txid, uint8_t unit, uint8_t fc,
                        uint16_t addr, uint16_t count)
{
    put16b(b, txid);
    put16b(b + 2, 0u);
    put16b(b + 4, 6u);
    b[6] = unit;
    b[7] = fc;
    put16b(b + 8, addr);
    put16b(b + 10, count);
    return 12u;
}

static uint16_t mk_write_single(uint8_t *b, uint16_t txid, uint8_t unit,
                                uint16_t addr, uint16_t value)
{
    uint16_t n = mk_read(b, txid, unit, W5500_MB_FC_WRITE_SINGLE, addr, 1u);
    put16b(b + 10, value);
    return n;
}

static uint16_t mk_write_multi(uint8_t *b, uint16_t txid, uint8_t unit,
                               uint16_t addr, const uint16_t *values,
                               uint16_t count)
{
    uint16_t i;

    put16b(b, txid);
    put16b(b + 2, 0u);
    put16b(b + 4, (uint16_t)(7u + 2u * count));
    b[6] = unit;
    b[7] = W5500_MB_FC_WRITE_MULTIPLE;
    put16b(b + 8, addr);
    put16b(b + 10, count);
    b[12] = (uint8_t)(2u * count);
    for (i = 0; i < count; i++) {
        put16b(b + 13u + 2u * i, values[i]);
    }
    return (uint16_t)(13u + 2u * count);
}

/* Let events, pending SENDs and the server all settle. */
static void server_pump(w5500_handle_t h, w5500_modbus_server_t *srv)
{
    unsigned k;

    for (k = 0; k < 4u; k++) {
        (void)w5500_poll(h);
        (void)w5500_modbus_server_poll(srv);
    }
}

static void test_server_read_write(void)
{
    static uint8_t mem[8192];
    w5500_handle_t h = fixture_open();
    w5500_modbus_server_t *srv = NULL;
    static const uint8_t peer[4] = {10, 0, 0, 9};
    uint8_t req[64];
    uint16_t n;

    memset(g_hold, 0, sizeof(g_hold));
    memset(g_inp, 0, sizeof(g_inp));
    g_writes = 0u;
    g_hold[0x10] = 0x1111u;
    g_hold[0x11] = 0x2222u;

    CHECK(w5500_modbus_server_size() <= sizeof(mem));
    CHECK_EQ(w5500_modbus_server_init(&srv, mem, sizeof(mem), h, 502u, 2u,
                                      map_read, map_write, NULL),
             W5500_OK);
    CHECK_EQ(w5500_modbus_server_start(srv), W5500_OK);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_SR], W5500_SOCK_LISTEN);
    CHECK_EQ(g_mock.sreg[1][W5500_Sn_SR], W5500_SOCK_LISTEN);

    /* Client connects to the first listener. */
    mock_w5500_remote_connect(0, peer, 40000u);
    (void)w5500_poll(h);
    CHECK_EQ(g_mock.sreg[0][W5500_Sn_SR], W5500_SOCK_ESTABLISHED);

    /* FC3: read two holding registers. */
    n = mk_read(req, 0x0101u, 1u, W5500_MB_FC_READ_HOLDING, 0x10u, 2u);
    mock_w5500_inject_rx(0, req, n);
    server_pump(h, srv);
    CHECK_EQ(g_mock.last_sent_len[0], 13);
    CHECK_EQ(rd16(g_mock.last_sent[0]), 0x0101);
    CHECK_EQ(g_mock.last_sent[0][7], 3);
    CHECK_EQ(g_mock.last_sent[0][8], 4);
    CHECK_EQ(rd16(g_mock.last_sent[0] + 9), 0x1111);
    CHECK_EQ(rd16(g_mock.last_sent[0] + 11), 0x2222);

    /* FC4 from the input map. */
    g_inp[0] = 0x7777u;
    n = mk_read(req, 0x0102u, 1u, W5500_MB_FC_READ_INPUT, 0u, 1u);
    mock_w5500_inject_rx(0, req, n);
    server_pump(h, srv);
    CHECK_EQ(g_mock.last_sent[0][7], 4);
    CHECK_EQ(rd16(g_mock.last_sent[0] + 9), 0x7777);

    /* FC6: write one holding register. */
    n = mk_write_single(req, 0x0103u, 1u, 0x20u, 0xBEEFu);
    mock_w5500_inject_rx(0, req, n);
    server_pump(h, srv);
    CHECK_EQ(g_hold[0x20], 0xBEEF);
    CHECK_EQ(g_writes, 1u);
    CHECK_EQ(g_mock.last_sent_len[0], 12);
    CHECK_EQ(g_mock.last_sent[0][7], 6);
    CHECK_EQ(rd16(g_mock.last_sent[0] + 8), 0x20);
    CHECK_EQ(rd16(g_mock.last_sent[0] + 10), 0xBEEF);

    /* FC16: write two registers. */
    {
        static const uint16_t vals[2] = {0xAABBu, 0xCCDDu};
        n = mk_write_multi(req, 0x0104u, 1u, 0x30u, vals, 2u);
        mock_w5500_inject_rx(0, req, n);
        server_pump(h, srv);
        CHECK_EQ(g_hold[0x30], 0xAABB);
        CHECK_EQ(g_hold[0x31], 0xCCDD);
        CHECK_EQ(g_mock.last_sent[0][7], 16);
        CHECK_EQ(rd16(g_mock.last_sent[0] + 8), 0x30);
        CHECK_EQ(rd16(g_mock.last_sent[0] + 10), 2);
    }

    /* Illegal address -> exception 02. */
    n = mk_read(req, 0x0105u, 1u, W5500_MB_FC_READ_HOLDING, 60u, 8u);
    mock_w5500_inject_rx(0, req, n);
    server_pump(h, srv);
    CHECK_EQ(g_mock.last_sent[0][7], 0x83);
    CHECK_EQ(g_mock.last_sent[0][8], W5500_MB_EX_ILLEGAL_ADDRESS);

    /* Unknown function -> exception 01. */
    n = mk_read(req, 0x0106u, 1u, 0x2Bu, 0u, 1u);
    mock_w5500_inject_rx(0, req, n);
    server_pump(h, srv);
    CHECK_EQ(g_mock.last_sent[0][7], 0xAB);
    CHECK_EQ(g_mock.last_sent[0][8], W5500_MB_EX_ILLEGAL_FUNCTION);

    /* A second client is served on the other listener. */
    mock_w5500_remote_connect(1, peer, 40001u);
    (void)w5500_poll(h);
    n = mk_read(req, 0x0201u, 1u, W5500_MB_FC_READ_HOLDING, 0x10u, 1u);
    mock_w5500_inject_rx(1, req, n);
    server_pump(h, srv);
    CHECK_EQ(rd16(g_mock.last_sent[1]), 0x0201);
    CHECK_EQ(rd16(g_mock.last_sent[1] + 9), 0x1111);

    /* A malformed frame drops that client; the listener comes back. */
    req[4] = 0u;
    req[5] = 1u; /* MBAP length 1 is illegal */
    mock_w5500_inject_rx(1, req, 8u);
    server_pump(h, srv);
    /* The slot was dropped and reopened for the next client. */
    CHECK_EQ(g_mock.sreg[1][W5500_Sn_SR], W5500_SOCK_LISTEN);
    CHECK_EQ(g_mock.last_sent_len[1], 11u); /* no new reply was produced */

    w5500_modbus_server_stop(srv);
    w5500_deinit(h);
    CHECK_EQ(g_mock.lock_count, g_mock.unlock_count);
}

/* ------------------------------------------------------------------ */
/* Client                                                              */
/* ------------------------------------------------------------------ */

static unsigned g_cli_calls;
static w5500_result_t g_cli_status;
static uint16_t g_cli_regs[8];
static uint16_t g_cli_count;

static void cli_cb(w5500_modbus_client_t *cli, w5500_result_t status,
                   const uint16_t *regs, uint16_t count, void *user)
{
    (void)cli;
    (void)user;
    g_cli_calls++;
    g_cli_status = status;
    g_cli_count = count;
    if (regs != NULL && count <= 8u) {
        memcpy(g_cli_regs, regs, (size_t)count * sizeof(uint16_t));
    }
}

static void cli_pump(w5500_handle_t h, w5500_modbus_client_t *cli,
                     uint32_t now)
{
    unsigned k;

    for (k = 0; k < 4u; k++) {
        (void)w5500_poll(h);
        (void)w5500_modbus_client_poll(cli, now);
    }
}

static w5500_result_t open_client(w5500_handle_t h, w5500_modbus_client_t **cli,
                                  void *mem, size_t mem_size)
{
    static const uint8_t srv_ip[4] = {10, 0, 0, 5};

    return w5500_modbus_client_init(cli, mem, mem_size, h, srv_ip, 502u,
                                    cli_cb, NULL);
}

static void test_client_read(void)
{
    static uint8_t mem[8192];
    w5500_handle_t h = fixture_open();
    w5500_modbus_client_t *cli = NULL;
    uint8_t reply[32];
    uint16_t txid;

    CHECK(w5500_modbus_client_size() <= sizeof(mem));
    CHECK_EQ(open_client(h, &cli, mem, sizeof(mem)), W5500_OK);
    g_cli_calls = 0u;

    CHECK_EQ(w5500_modbus_client_read(cli, 0u, 1u, W5500_MB_FC_READ_HOLDING,
                                      0x10u, 2u),
             W5500_OK);
    CHECK_EQ(w5500_modbus_client_read(cli, 0u, 1u, W5500_MB_FC_READ_HOLDING,
                                      0x10u, 2u),
             W5500_ERR_BUSY);
    cli_pump(h, cli, 0u);
    CHECK_EQ(g_mock.last_sent_len[0], 12);
    CHECK_EQ(g_mock.last_sent[0][7], 3);
    CHECK_EQ(rd16(g_mock.last_sent[0] + 10), 2);
    txid = rd16(g_mock.last_sent[0]);

    put16b(reply, txid);
    put16b(reply + 2, 0u);
    put16b(reply + 4, 7u);
    reply[6] = 1u;
    reply[7] = 3u;
    reply[8] = 4u;
    put16b(reply + 9, 0x0102u);
    put16b(reply + 11, 0x0304u);
    mock_w5500_inject_rx(0, reply, 13u);
    cli_pump(h, cli, 0u);

    CHECK_EQ(g_cli_calls, 1);
    CHECK_EQ(g_cli_status, W5500_OK);
    CHECK_EQ(g_cli_count, 2);
    CHECK_EQ(g_cli_regs[0], 0x0102);
    CHECK_EQ(g_cli_regs[1], 0x0304);

    w5500_modbus_client_stop(cli);
    w5500_deinit(h);
    CHECK_EQ(g_mock.lock_count, g_mock.unlock_count);
}

static void test_client_write_and_exception(void)
{
    static uint8_t mem[8192];
    w5500_handle_t h = fixture_open();
    w5500_modbus_client_t *cli = NULL;
    uint8_t reply[32];
    uint16_t txid;

    CHECK_EQ(open_client(h, &cli, mem, sizeof(mem)), W5500_OK);
    g_cli_calls = 0u;

    /* FC6 echo -> OK with no register payload. */
    CHECK_EQ(w5500_modbus_client_write_single(cli, 0u, 1u, 0x20u, 0x1234u),
             W5500_OK);
    cli_pump(h, cli, 0u);
    CHECK_EQ(g_mock.last_sent[0][7], 6);
    txid = rd16(g_mock.last_sent[0]);
    put16b(reply, txid);
    put16b(reply + 2, 0u);
    put16b(reply + 4, 6u);
    reply[6] = 1u;
    reply[7] = 6u;
    put16b(reply + 8, 0x20u);
    put16b(reply + 10, 0x1234u);
    mock_w5500_inject_rx(0, reply, 12u);
    cli_pump(h, cli, 0u);
    CHECK_EQ(g_cli_calls, 1);
    CHECK_EQ(g_cli_status, W5500_OK);
    CHECK_EQ(g_cli_count, 0);

    /* Exception reply -> ERR_STATE. */
    CHECK_EQ(w5500_modbus_client_read(cli, 0u, 1u, W5500_MB_FC_READ_HOLDING,
                                      0x10u, 1u),
             W5500_OK);
    cli_pump(h, cli, 0u);
    txid = rd16(g_mock.last_sent[0]);
    reply[0] = (uint8_t)(txid >> 8);
    reply[1] = (uint8_t)txid;
    put16b(reply + 2, 0u);
    put16b(reply + 4, 3u);
    reply[6] = 1u;
    reply[7] = 0x83u;
    reply[8] = W5500_MB_EX_ILLEGAL_ADDRESS;
    mock_w5500_inject_rx(0, reply, 9u);
    cli_pump(h, cli, 0u);
    CHECK_EQ(g_cli_calls, 2);
    CHECK_EQ(g_cli_status, W5500_ERR_STATE);

    w5500_modbus_client_stop(cli);
    w5500_deinit(h);
}

static void test_client_timeout(void)
{
    static uint8_t mem[8192];
    w5500_handle_t h = fixture_open();
    w5500_modbus_client_t *cli = NULL;

    CHECK_EQ(open_client(h, &cli, mem, sizeof(mem)), W5500_OK);
    g_cli_calls = 0u;

    CHECK_EQ(w5500_modbus_client_read(cli, 0u, 1u, W5500_MB_FC_READ_HOLDING,
                                      0u, 1u),
             W5500_OK);
    cli_pump(h, cli, 0u);
    CHECK_EQ(g_cli_calls, 0u);

    /* One resend at 2 s, failure at 4 s. */
    (void)w5500_modbus_client_poll(cli, 2000u);
    CHECK_EQ(g_cli_calls, 0u);
    (void)w5500_modbus_client_poll(cli, 4000u);
    CHECK_EQ(g_cli_calls, 1u);
    CHECK_EQ(g_cli_status, W5500_ERR_TIMEOUT);

    w5500_modbus_client_stop(cli);
    w5500_deinit(h);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_parse_read();
    test_parse_writes();
    test_parse_rejects();
    test_builders();
    test_server_read_write();
    test_client_read();
    test_client_write_and_exception();
    test_client_timeout();

    if (g_failures == 0) {
        printf("OK: all modbus tests passed\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
