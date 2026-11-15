/**
 * @file w5500_modbus.c
 * @brief Modbus TCP support: ADU codec, server and client.
 */
#include <string.h>

#include "w5500_modbus.h"
#include "w5500_sockets.h"
#include "w5500_modbus_internal.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t mbap_write(uint8_t *buf, uint16_t txid, uint16_t proto_len,
                           uint8_t unit)
{
    put16(buf, txid);
    put16(buf + 2, 0u); /* protocol id */
    put16(buf + 4, proto_len);
    buf[6] = unit;
    return 7u;
}

/* ------------------------------------------------------------------ */
/* Codec                                                               */
/* ------------------------------------------------------------------ */

int w5500_mb_parse_response(const uint8_t *buf, uint16_t len, uint16_t txid,
                            w5500_mb_adu_t *out)
{
    uint16_t mbap_len;
    uint16_t frame_len;
    uint8_t fc;

    if (buf == NULL || out == NULL || len < 9u) {
        return -1;
    }
    if (get16(buf) != txid || get16(buf + 2) != 0u) {
        return -1;
    }
    mbap_len = get16(buf + 4);
    if (mbap_len < 3u || mbap_len > 254u) {
        return -1;
    }
    frame_len = (uint16_t)(6u + mbap_len);
    if (len < frame_len) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->txid = txid;
    out->unit = buf[6];
    fc = buf[7];
    out->fc = fc;
    out->frame_len = frame_len;

    if ((fc & 0x80u) != 0u) {
        out->value = buf[8]; /* exception code */
        return 0;
    }
    switch (fc) {
    case W5500_MB_FC_READ_HOLDING:
    case W5500_MB_FC_READ_INPUT: {
        uint16_t byte_count = buf[8];

        if (byte_count > 250u || (byte_count & 1u) != 0u) {
            return -1;
        }
        if (mbap_len != (uint16_t)(3u + byte_count)) {
            return -1;
        }
        out->count = (uint16_t)(byte_count / 2u);
        out->data = buf + 9;
        out->data_len = byte_count;
        return 0;
    }
    case W5500_MB_FC_WRITE_SINGLE:
    case W5500_MB_FC_WRITE_MULTIPLE:
        if (mbap_len != 6u) {
            return -1;
        }
        out->addr = get16(buf + 8);
        out->value = get16(buf + 10);
        out->count = (fc == W5500_MB_FC_WRITE_SINGLE) ? 1u : out->value;
        return 0;
    default:
        return 0; /* unknown reply fc: the caller decides */
    }
}

int w5500_mb_parse_adu(const uint8_t *buf, uint16_t len, w5500_mb_adu_t *out)
{
    uint16_t mbap_len;
    uint16_t frame_len;
    uint8_t fc;

    if (buf == NULL || out == NULL || len < 8u) {
        return -1;
    }
    if (get16(buf + 2) != 0u) {
        return -1; /* only Modbus protocol id 0 */
    }
    mbap_len = get16(buf + 4);
    if (mbap_len < 2u || mbap_len > 254u) {
        return -1;
    }
    frame_len = (uint16_t)(6u + mbap_len);
    if (len < frame_len) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->txid = get16(buf);
    out->unit = buf[6];
    fc = buf[7];
    out->fc = fc;
    out->frame_len = frame_len;

    switch (fc) {
    case W5500_MB_FC_READ_HOLDING:
    case W5500_MB_FC_READ_INPUT:
        if (mbap_len != 6u) {
            return -1;
        }
        out->addr = get16(buf + 8);
        out->count = get16(buf + 10);
        if (out->count < 1u || out->count > 125u) {
            return -1;
        }
        return 0;

    case W5500_MB_FC_WRITE_SINGLE:
        if (mbap_len != 6u) {
            return -1;
        }
        out->addr = get16(buf + 8);
        out->value = get16(buf + 10);
        out->count = 1u;
        return 0;

    case W5500_MB_FC_WRITE_MULTIPLE: {
        uint16_t byte_count;

        if (mbap_len < 7u) {
            return -1;
        }
        out->addr = get16(buf + 8);
        out->count = get16(buf + 10);
        if (out->count < 1u || out->count > 123u) {
            return -1;
        }
        if (mbap_len != (uint16_t)(7u + 2u * out->count)) {
            return -1;
        }
        byte_count = buf[12];
        if (byte_count != (uint16_t)(2u * out->count)) {
            return -1;
        }
        out->data = buf + 13;
        out->data_len = byte_count;
        return 0;
    }

    default:
        /* Unknown function code: valid frame, answered with an exception. */
        return 0;
    }
}

uint16_t w5500_mb_build_exception(uint8_t *buf, uint16_t cap, uint16_t txid,
                                  uint8_t unit, uint8_t fc, uint8_t code)
{
    if (buf == NULL || cap < 9u) {
        return 0u;
    }
    (void)mbap_write(buf, txid, 3u, unit);
    buf[7] = (uint8_t)(fc | 0x80u);
    buf[8] = code;
    return 9u;
}

uint16_t w5500_mb_build_read_response(uint8_t *buf, uint16_t cap,
                                      uint16_t txid, uint8_t unit, uint8_t fc,
                                      const uint16_t *regs, uint16_t count)
{
    uint16_t i;
    uint16_t total;

    if (buf == NULL || regs == NULL || count == 0u || count > 125u) {
        return 0u;
    }
    total = (uint16_t)(9u + 2u * count);
    if (cap < total) {
        return 0u;
    }
    (void)mbap_write(buf, txid, (uint16_t)(3u + 2u * count), unit);
    buf[7] = fc;
    buf[8] = (uint8_t)(2u * count);
    for (i = 0; i < count; i++) {
        put16(buf + 9u + 2u * i, regs[i]);
    }
    return total;
}

uint16_t w5500_mb_build_write_response(uint8_t *buf, uint16_t cap,
                                       uint16_t txid, uint8_t unit, uint8_t fc,
                                       uint16_t addr, uint16_t arg)
{
    if (buf == NULL || cap < 12u) {
        return 0u;
    }
    (void)mbap_write(buf, txid, 6u, unit);
    buf[7] = fc;
    put16(buf + 8, addr);
    put16(buf + 10, arg);
    return 12u;
}

/* ------------------------------------------------------------------ */
/* Server                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    w5500_socket_t sock;
    uint16_t       rx_len;
    uint16_t       tx_len;
    uint16_t       tx_off;
    uint8_t        rx[W5500_MB_FRAME_MAX];
    uint8_t        tx[W5500_MB_FRAME_MAX];
} mb_session_t;

struct w5500_modbus_server {
    w5500_handle_t  chip;
    uint16_t        port;
    uint8_t         max_clients;
    w5500_mb_read_cb  read_cb;
    w5500_mb_write_cb write_cb;
    void           *user;
    mb_session_t    s[MODBUS_MAX_CLIENTS];
};

static uint8_t mb_exception_for(w5500_result_t rc)
{
    return (rc == W5500_ERR_PARAM) ? W5500_MB_EX_ILLEGAL_ADDRESS
                                   : W5500_MB_EX_SERVER_FAILURE;
}

static uint16_t mb_server_dispatch(struct w5500_modbus_server *srv,
                                   const w5500_mb_adu_t *a, uint8_t *out)
{
    uint16_t regs[125];
    uint16_t i;
    w5500_result_t rc;

    switch (a->fc) {
    case W5500_MB_FC_READ_HOLDING:
    case W5500_MB_FC_READ_INPUT:
        rc = srv->read_cb(a->unit, a->fc, a->addr, a->count, regs, srv->user);
        if (rc != W5500_OK) {
            return w5500_mb_build_exception(out, W5500_MB_FRAME_MAX, a->txid,
                                            a->unit, a->fc,
                                            mb_exception_for(rc));
        }
        return w5500_mb_build_read_response(out, W5500_MB_FRAME_MAX, a->txid,
                                            a->unit, a->fc, regs, a->count);

    case W5500_MB_FC_WRITE_SINGLE:
        rc = srv->write_cb(a->unit, a->addr, 1u, &a->value, srv->user);
        if (rc != W5500_OK) {
            return w5500_mb_build_exception(out, W5500_MB_FRAME_MAX, a->txid,
                                            a->unit, a->fc,
                                            mb_exception_for(rc));
        }
        return w5500_mb_build_write_response(out, W5500_MB_FRAME_MAX, a->txid,
                                             a->unit, a->fc, a->addr,
                                             a->value);

    case W5500_MB_FC_WRITE_MULTIPLE:
        for (i = 0; i < a->count; i++) {
            regs[i] = get16(a->data + 2u * i);
        }
        rc = srv->write_cb(a->unit, a->addr, a->count, regs, srv->user);
        if (rc != W5500_OK) {
            return w5500_mb_build_exception(out, W5500_MB_FRAME_MAX, a->txid,
                                            a->unit, a->fc,
                                            mb_exception_for(rc));
        }
        return w5500_mb_build_write_response(out, W5500_MB_FRAME_MAX, a->txid,
                                             a->unit, a->fc, a->addr,
                                             a->count);

    default:
        return w5500_mb_build_exception(out, W5500_MB_FRAME_MAX, a->txid,
                                        a->unit, a->fc,
                                        W5500_MB_EX_ILLEGAL_FUNCTION);
    }
}

static void mb_server_drop(struct w5500_modbus_server *srv, mb_session_t *s)
{
    (void)srv;
    if (s->sock != NULL) {
        (void)w5500_socket_close(s->sock);
    }
    memset(s, 0, sizeof(*s));
}

static void mb_server_cb(w5500_socket_t sock, w5500_event_t event,
                         w5500_result_t status, void *user_arg)
{
    struct w5500_modbus_server *srv = user_arg;
    uint8_t i;

    (void)status;
    if (srv == NULL || sock == NULL) {
        return;
    }
    if (event != W5500_EVENT_DISCONNECTED && event != W5500_EVENT_ERROR) {
        return; /* data is drained by poll() */
    }
    for (i = 0; i < srv->max_clients; i++) {
        if (srv->s[i].sock == sock) {
            /* The driver already released the handle. */
            memset(&srv->s[i], 0, sizeof(srv->s[i]));
            break;
        }
    }
}

static w5500_result_t mb_server_open(struct w5500_modbus_server *srv,
                                     mb_session_t *s)
{
    w5500_socket_t sock = NULL;
    w5500_result_t rc;

    rc = w5500_socket_open_ex(srv->chip, W5500_PROTO_TCP, srv->port,
                              W5500_SOCK_F_NO_DELAY, mb_server_cb, srv, &sock);
    if (rc != W5500_OK) {
        return rc;
    }
    rc = w5500_socket_listen(sock);
    if (rc != W5500_OK) {
        (void)w5500_socket_close(sock);
        return rc;
    }
    memset(s, 0, sizeof(*s));
    s->sock = sock;
    return W5500_OK;
}

static void mb_server_flush(mb_session_t *s)
{
    while (s->tx_off < s->tx_len) {
        uint16_t sent = 0u;
        w5500_result_t rc = w5500_socket_send(s->sock, s->tx + s->tx_off,
                                              (uint16_t)(s->tx_len - s->tx_off),
                                              &sent);
        if (rc == W5500_ERR_BUSY) {
            return; /* previous SEND still in flight: retry on the next poll */
        }
        if (rc != W5500_OK) {
            s->tx_len = 0u;
            s->tx_off = 0u;
            return;
        }
        s->tx_off = (uint16_t)(s->tx_off + sent);
        if (sent == 0u) {
            return;
        }
    }
    s->tx_len = 0u;
    s->tx_off = 0u;
}

static int mb_server_rx(struct w5500_modbus_server *srv, mb_session_t *s)
{
    /* Drain whatever the chip has, then answer every complete ADU. */
    for (;;) {
        uint16_t n = 0u;
        uint16_t room = (uint16_t)(sizeof(s->rx) - s->rx_len);
        w5500_result_t rc;

        if (room == 0u) {
            break;
        }
        rc = w5500_socket_recv(s->sock, s->rx + s->rx_len, room, &n);
        if (rc != W5500_OK) {
            mb_server_drop(srv, s);
            return -1;
        }
        if (n == 0u) {
            break;
        }
        s->rx_len = (uint16_t)(s->rx_len + n);
    }

    while (s->rx_len >= 8u) {
        uint16_t mbap_len = get16(s->rx + 4);
        uint16_t total = (uint16_t)(6u + mbap_len);
        w5500_mb_adu_t a;
        uint16_t resp_len;

        if (mbap_len < 2u || mbap_len > 254u || total > sizeof(s->rx)) {
            mb_server_drop(srv, s);
            return -1;
        }
        if (s->rx_len < total) {
            break; /* wait for the rest of the frame */
        }
        if (s->tx_len != 0u) {
            break; /* one outstanding reply per socket */
        }
        if (w5500_mb_parse_adu(s->rx, total, &a) != 0) {
            mb_server_drop(srv, s);
            return -1;
        }
        resp_len = mb_server_dispatch(srv, &a, s->tx);
        memmove(s->rx, s->rx + total, (size_t)(s->rx_len - total));
        s->rx_len = (uint16_t)(s->rx_len - total);
        if (resp_len == 0u) {
            continue;
        }
        s->tx_len = resp_len;
        s->tx_off = 0u;
        mb_server_flush(s);
        if (s->tx_len != 0u) {
            break; /* backpressure: finish on the next poll */
        }
    }
    return 0;
}

size_t w5500_modbus_server_size(void)
{
    return sizeof(struct w5500_modbus_server);
}

w5500_result_t w5500_modbus_server_init(w5500_modbus_server_t **out_srv,
                                        void *mem,
                                        size_t mem_size,
                                        w5500_handle_t chip,
                                        uint16_t port,
                                        uint8_t max_clients,
                                        w5500_mb_read_cb read_cb,
                                        w5500_mb_write_cb write_cb,
                                        void *user_arg)
{
    struct w5500_modbus_server *srv;

    if (out_srv == NULL || mem == NULL || chip == NULL || port == 0u ||
        read_cb == NULL || write_cb == NULL || max_clients == 0u ||
        max_clients > MODBUS_MAX_CLIENTS) {
        return W5500_ERR_PARAM;
    }
    if (mem_size < sizeof(*srv)) {
        return W5500_ERR_NOMEM;
    }
    srv = (struct w5500_modbus_server *)mem;
    memset(srv, 0, sizeof(*srv));
    srv->chip = chip;
    srv->port = port;
    srv->max_clients = max_clients;
    srv->read_cb = read_cb;
    srv->write_cb = write_cb;
    srv->user = user_arg;
    *out_srv = srv;
    return W5500_OK;
}

w5500_result_t w5500_modbus_server_start(w5500_modbus_server_t *srv)
{
    uint8_t i;

    if (srv == NULL) {
        return W5500_ERR_PARAM;
    }
    for (i = 0; i < srv->max_clients; i++) {
        if (srv->s[i].sock == NULL) {
            w5500_result_t rc = mb_server_open(srv, &srv->s[i]);
            if (rc != W5500_OK) {
                return rc;
            }
        }
    }
    return W5500_OK;
}

w5500_result_t w5500_modbus_server_poll(w5500_modbus_server_t *srv)
{
    uint8_t i;

    if (srv == NULL) {
        return W5500_ERR_PARAM;
    }
    for (i = 0; i < srv->max_clients; i++) {
        if (srv->s[i].sock == NULL) {
            (void)mb_server_open(srv, &srv->s[i]); /* reopen a freed slot */
            continue;
        }
        mb_server_flush(&srv->s[i]);
        if (mb_server_rx(srv, &srv->s[i]) != 0) {
            continue; /* dropped, reopened on the next poll */
        }
    }
    return W5500_OK;
}

void w5500_modbus_server_stop(w5500_modbus_server_t *srv)
{
    uint8_t i;

    if (srv == NULL) {
        return;
    }
    for (i = 0; i < srv->max_clients; i++) {
        mb_server_drop(srv, &srv->s[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Client                                                              */
/* ------------------------------------------------------------------ */

#define MB_CLIENT_OFF      0u
#define MB_CLIENT_CONNECT  1u
#define MB_CLIENT_SEND     2u
#define MB_CLIENT_AWAIT    3u
#define MB_CLIENT_TIMEOUT_MS 2000u

struct w5500_modbus_client {
    w5500_handle_t      chip;
    uint8_t             ip[4];
    uint16_t            port;
    w5500_socket_t      sock;
    w5500_mb_client_cb  cb;
    void               *user;

    uint8_t             state;
    uint8_t             unit;
    uint8_t             fc;
    uint8_t             resent;
    uint16_t            txid;
    uint16_t            txid_counter;
    uint16_t            expect_count;
    uint16_t            req_len;
    uint16_t            rx_len;
    uint32_t            t_last;

    uint8_t             req[W5500_MB_FRAME_MAX];
    uint8_t             rx[W5500_MB_FRAME_MAX];
    uint16_t            result[125];
};

static void mb_client_finish(struct w5500_modbus_client *cli,
                             w5500_result_t status, uint16_t count)
{
    cli->state = MB_CLIENT_OFF;
    if (cli->sock != NULL) {
        (void)w5500_socket_close(cli->sock);
        cli->sock = NULL;
    }
    if (cli->cb != NULL) {
        cli->cb(cli, status,
                (status == W5500_OK && count > 0u) ? cli->result : NULL,
                count, cli->user);
    }
}

static void mb_client_cb(w5500_socket_t sock, w5500_event_t event,
                         w5500_result_t status, void *user_arg)
{
    struct w5500_modbus_client *cli = user_arg;

    (void)status;
    if (cli == NULL || sock == NULL || sock != cli->sock) {
        return;
    }
    switch (event) {
    case W5500_EVENT_CONNECTED:
        if (cli->state == MB_CLIENT_CONNECT) {
            cli->state = MB_CLIENT_SEND;
        }
        break;
    case W5500_EVENT_ERROR:
        cli->sock = NULL; /* released by the driver */
        mb_client_finish(cli, W5500_ERR_TIMEOUT, 0u);
        break;
    case W5500_EVENT_DISCONNECTED:
        cli->sock = NULL;
        mb_client_finish(cli, W5500_ERR_STATE, 0u);
        break;
    default:
        break;
    }
}

size_t w5500_modbus_client_size(void)
{
    return sizeof(struct w5500_modbus_client);
}

w5500_result_t w5500_modbus_client_init(w5500_modbus_client_t **out_cli,
                                        void *mem,
                                        size_t mem_size,
                                        w5500_handle_t chip,
                                        const uint8_t server_ip[4],
                                        uint16_t port,
                                        w5500_mb_client_cb cb,
                                        void *user_arg)
{
    struct w5500_modbus_client *cli;

    if (out_cli == NULL || mem == NULL || chip == NULL || server_ip == NULL ||
        port == 0u || cb == NULL) {
        return W5500_ERR_PARAM;
    }
    if (mem_size < sizeof(*cli)) {
        return W5500_ERR_NOMEM;
    }
    cli = (struct w5500_modbus_client *)mem;
    memset(cli, 0, sizeof(*cli));
    cli->chip = chip;
    memcpy(cli->ip, server_ip, 4u);
    cli->port = port;
    cli->cb = cb;
    cli->user = user_arg;
    cli->state = MB_CLIENT_OFF;
    *out_cli = cli;
    return W5500_OK;
}

static w5500_result_t mb_client_begin(struct w5500_modbus_client *cli,
                                      uint32_t now_ms)
{
    w5500_socket_t sock = NULL;
    w5500_result_t rc;

    cli->txid_counter++;
    cli->txid = cli->txid_counter;
    cli->rx_len = 0u;
    cli->resent = 0u;
    cli->t_last = now_ms;

    rc = w5500_socket_open_ex(cli->chip, W5500_PROTO_TCP, 0u,
                              W5500_SOCK_F_NO_DELAY, mb_client_cb, cli, &sock);
    if (rc != W5500_OK) {
        return rc;
    }
    rc = w5500_socket_connect(sock, cli->ip, cli->port);
    if (rc != W5500_OK) {
        (void)w5500_socket_close(sock);
        return rc;
    }
    cli->sock = sock;
    cli->state = MB_CLIENT_CONNECT;
    return W5500_OK;
}

w5500_result_t w5500_modbus_client_read(w5500_modbus_client_t *cli,
                                        uint32_t now_ms,
                                        uint8_t unit,
                                        uint8_t fc,
                                        uint16_t addr,
                                        uint16_t count)
{
    if (cli == NULL || cli->state != MB_CLIENT_OFF ||
        (fc != W5500_MB_FC_READ_HOLDING && fc != W5500_MB_FC_READ_INPUT) ||
        count < 1u || count > 125u) {
        return (cli != NULL && cli->state != MB_CLIENT_OFF)
                   ? W5500_ERR_BUSY
                   : W5500_ERR_PARAM;
    }
    cli->unit = unit;
    cli->fc = fc;
    cli->expect_count = count;
    {
        w5500_result_t rc = mb_client_begin(cli, now_ms);
        if (rc != W5500_OK) {
            return rc;
        }
    }
    (void)mbap_write(cli->req, cli->txid, 6u, unit);
    cli->req[7] = fc;
    put16(cli->req + 8, addr);
    put16(cli->req + 10, count);
    cli->req_len = 12u;
    return W5500_OK;
}

w5500_result_t w5500_modbus_client_write_single(w5500_modbus_client_t *cli,
                                                uint32_t now_ms,
                                                uint8_t unit,
                                                uint16_t addr,
                                                uint16_t value)
{
    if (cli == NULL || cli->state != MB_CLIENT_OFF) {
        return (cli != NULL) ? W5500_ERR_BUSY : W5500_ERR_PARAM;
    }
    cli->unit = unit;
    cli->fc = W5500_MB_FC_WRITE_SINGLE;
    cli->expect_count = 0u;
    {
        w5500_result_t rc = mb_client_begin(cli, now_ms);
        if (rc != W5500_OK) {
            return rc;
        }
    }
    (void)mbap_write(cli->req, cli->txid, 6u, unit);
    cli->req[7] = W5500_MB_FC_WRITE_SINGLE;
    put16(cli->req + 8, addr);
    put16(cli->req + 10, value);
    cli->req_len = 12u;
    return W5500_OK;
}

w5500_result_t w5500_modbus_client_write_multiple(
    w5500_modbus_client_t *cli,
    uint32_t now_ms,
    uint8_t unit,
    uint16_t addr,
    const uint16_t *values,
    uint16_t count)
{
    uint16_t i;

    if (cli == NULL || values == NULL || cli->state != MB_CLIENT_OFF ||
        count < 1u || count > 123u) {
        return (cli != NULL && cli->state != MB_CLIENT_OFF)
                   ? W5500_ERR_BUSY
                   : W5500_ERR_PARAM;
    }
    cli->unit = unit;
    cli->fc = W5500_MB_FC_WRITE_MULTIPLE;
    cli->expect_count = 0u;
    {
        w5500_result_t rc = mb_client_begin(cli, now_ms);
        if (rc != W5500_OK) {
            return rc;
        }
    }
    (void)mbap_write(cli->req, cli->txid, (uint16_t)(7u + 2u * count), unit);
    cli->req[7] = W5500_MB_FC_WRITE_MULTIPLE;
    put16(cli->req + 8, addr);
    put16(cli->req + 10, count);
    cli->req[12] = (uint8_t)(2u * count);
    for (i = 0; i < count; i++) {
        put16(cli->req + 13u + 2u * i, values[i]);
    }
    cli->req_len = (uint16_t)(13u + 2u * count);
    return W5500_OK;
}

w5500_result_t w5500_modbus_client_poll(w5500_modbus_client_t *cli,
                                        uint32_t now_ms)
{
    if (cli == NULL) {
        return W5500_ERR_PARAM;
    }
    if (cli->state == MB_CLIENT_OFF) {
        return W5500_ERR_STATE;
    }

    if (cli->state == MB_CLIENT_CONNECT) {
        if ((int32_t)(now_ms - cli->t_last) >=
            (int32_t)MB_CLIENT_TIMEOUT_MS) {
            mb_client_finish(cli, W5500_ERR_TIMEOUT, 0u);
        }
        return W5500_OK;
    }

    if (cli->state == MB_CLIENT_SEND) {
        uint16_t sent = 0u;
        w5500_result_t rc = w5500_socket_send(cli->sock, cli->req, cli->req_len,
                                              &sent);
        if (rc == W5500_ERR_BUSY) {
            return W5500_OK; /* SEND_DONE will let us retry */
        }
        if (rc != W5500_OK || sent != cli->req_len) {
            mb_client_finish(cli, W5500_ERR_STATE, 0u);
            return W5500_OK;
        }
        cli->state = MB_CLIENT_AWAIT;
        cli->t_last = now_ms;
        return W5500_OK;
    }

    /* MB_CLIENT_AWAIT: collect the reply. */
    for (;;) {
        uint16_t n = 0u;
        uint16_t room = (uint16_t)(sizeof(cli->rx) - cli->rx_len);
        w5500_result_t rc;

        if (room == 0u) {
            mb_client_finish(cli, W5500_ERR_STATE, 0u);
            return W5500_OK;
        }
        rc = w5500_socket_recv(cli->sock, cli->rx + cli->rx_len, room, &n);
        if (rc != W5500_OK) {
            mb_client_finish(cli, W5500_ERR_STATE, 0u);
            return W5500_OK;
        }
        if (n == 0u) {
            break;
        }
        cli->rx_len = (uint16_t)(cli->rx_len + n);
    }

    if (cli->rx_len >= 8u) {
        uint16_t mbap_len = get16(cli->rx + 4);
        uint16_t total = (uint16_t)(6u + mbap_len);
        w5500_mb_adu_t a;

        if (mbap_len < 2u || mbap_len > 254u || total > sizeof(cli->rx)) {
            mb_client_finish(cli, W5500_ERR_STATE, 0u);
            return W5500_OK;
        }
        if (cli->rx_len >= total &&
            w5500_mb_parse_response(cli->rx, total, cli->txid, &a) == 0) {
            if ((a.fc & 0x80u) != 0u || a.fc != cli->fc) {
                mb_client_finish(cli, W5500_ERR_STATE, 0u); /* exception */
                return W5500_OK;
            }
            if (cli->fc == W5500_MB_FC_READ_HOLDING ||
                cli->fc == W5500_MB_FC_READ_INPUT) {
                uint16_t i;
                uint16_t count = a.count;

                if (count != cli->expect_count) {
                    mb_client_finish(cli, W5500_ERR_STATE, 0u);
                    return W5500_OK;
                }
                for (i = 0; i < count; i++) {
                    cli->result[i] = get16(a.data + 2u * i);
                }
                cli->state = MB_CLIENT_OFF;
                if (cli->sock != NULL) {
                    (void)w5500_socket_close(cli->sock);
                    cli->sock = NULL;
                }
                cli->cb(cli, W5500_OK, cli->result, count, cli->user);
                return W5500_OK;
            }
            mb_client_finish(cli, W5500_OK, 0u); /* write echo */
            return W5500_OK;
        }
    }

    if ((int32_t)(now_ms - cli->t_last) >= (int32_t)MB_CLIENT_TIMEOUT_MS) {
        if (cli->resent == 0u) {
            uint16_t sent = 0u;
            (void)w5500_socket_send(cli->sock, cli->req, cli->req_len, &sent);
            cli->resent = 1u;
            cli->t_last = now_ms;
        } else {
            mb_client_finish(cli, W5500_ERR_TIMEOUT, 0u);
        }
    }
    return W5500_OK;
}

void w5500_modbus_client_stop(w5500_modbus_client_t *cli)
{
    if (cli == NULL) {
        return;
    }
    if (cli->sock != NULL) {
        (void)w5500_socket_close(cli->sock);
        cli->sock = NULL;
    }
    cli->state = MB_CLIENT_OFF;
}
