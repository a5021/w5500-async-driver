/**
 * @file w5500_mqtt.c
 * @brief MQTT 3.1.1 client: packet codec and tick-driven state machine.
 */
#include <string.h>

#include "w5500_mqtt.h"
#include "w5500_sockets.h"
#include "w5500_mqtt_internal.h"

/* ------------------------------------------------------------------ */
/* Client state                                                        */
/* ------------------------------------------------------------------ */

#define MQTT_STATE_OFF      0u
#define MQTT_STATE_CONNECT  1u
#define MQTT_STATE_WAITING  2u
#define MQTT_STATE_READY    3u

struct w5500_mqtt {
    w5500_handle_t  chip;
    w5500_socket_t  sock;
    uint8_t         broker[4];
    uint16_t        port;
    char            client_id[48];
    w5500_mqtt_cb_t cb;
    void           *user;

    uint8_t         state;
    uint8_t         connected;
    uint16_t        keepalive_s;
    uint32_t        t_last;
    uint32_t        t_connect;
    uint16_t        packet_id;
    uint8_t         pending[W5500_MQTT_PKT_MAX];
    uint16_t        pending_len;
    uint8_t         rx[W5500_MQTT_PKT_MAX];
    uint16_t        rx_len;
};

/* ------------------------------------------------------------------ */
/* Codec                                                               */
/* ------------------------------------------------------------------ */

static uint8_t mqtt_remain_bytes(uint32_t len)
{
    uint8_t n = 1u;

    while (len >= 128u && n < 4u) {
        len /= 128u;
        n++;
    }
    return n;
}

static uint16_t put_remain(uint8_t *buf, uint16_t off, uint32_t len)
{
    do {
        uint8_t b = (uint8_t)(len % 128u);
        len /= 128u;
        if (len != 0u) {
            b |= 0x80u;
        }
        buf[off++] = b;
    } while (len != 0u);
    return off;
}

int w5500_mqtt_remaining_len(const uint8_t *buf, uint16_t len,
                             uint16_t *value, uint8_t *header_len)
{
    uint32_t v = 0u;
    uint32_t mult = 1u;
    uint8_t i;

    if (buf == NULL || value == NULL || header_len == NULL || len < 2u) {
        return -1;
    }
    for (i = 0; i < 4u; i++) {
        if ((uint32_t)i + 1u > (uint32_t)len) {
            return -1;
        }
        v += (uint32_t)(buf[i + 1u] & 0x7Fu) * mult;
        if ((buf[i + 1u] & 0x80u) == 0u) {
            if (v > 0xFFFFu) {
                return -1;
            }
            *value = (uint16_t)v;
            *header_len = (uint8_t)(i + 2u);
            return 0;
        }
        mult *= 128u;
    }
    return -1;
}

static uint16_t put_str(uint8_t *buf, uint16_t off, const char *s)
{
    size_t n = strlen(s);

    buf[off++] = (uint8_t)(n >> 8);
    buf[off++] = (uint8_t)(n & 0xFFu);
    memcpy(buf + off, s, n);
    return (uint16_t)(off + n);
}

int w5500_mqtt_parse(const uint8_t *buf, uint16_t len,
                     w5500_mqtt_packet_t *out, uint16_t *consumed)
{
    uint16_t remain = 0u;
    uint8_t hdr = 0u;
    uint16_t total;
    uint16_t off;

    if (w5500_mqtt_remaining_len(buf, len, &remain, &hdr) != 0) {
        return -1;
    }
    total = (uint16_t)(hdr + remain);
    if (total > len || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->type = (uint8_t)(buf[0] >> 4);
    out->flags = (uint8_t)(buf[0] & 0x0Fu);
    off = hdr;

    if (out->type == W5500_MQTT_CONNACK) {
        if (remain < 2u) {
            return -1;
        }
        out->flags = buf[off + 1u]; /* CONNACK return code */
        *consumed = total;
        return 0;
    }
    if (out->type == W5500_MQTT_PUBLISH) {
        uint16_t tlen;
        uint16_t payload_off;

        out->qos = (uint8_t)((out->flags >> 1) & 0x03u);
        out->retain = (uint8_t)(out->flags & 0x01u);
        if (remain < 2u || (uint32_t)off + 2u > (uint32_t)total) {
            return -1;
        }
        tlen = (uint16_t)(((uint16_t)buf[off] << 8) | buf[off + 1u]);
        payload_off = (uint16_t)(off + 2u + tlen);
        if ((uint32_t)payload_off > (uint32_t)total) {
            return -1;
        }
        out->topic = buf + off + 2u;
        out->topic_len = tlen;
        if (out->qos > 0u) {
            if ((uint32_t)payload_off + 2u > (uint32_t)total) {
                return -1;
            }
            out->packet_id = (uint16_t)(((uint16_t)buf[payload_off] << 8) |
                                        buf[payload_off + 1u]);
            payload_off = (uint16_t)(payload_off + 2u);
        }
        out->payload = buf + payload_off;
        out->payload_len = (uint16_t)(total - payload_off);
        *consumed = total;
        return 0;
    }
    if (out->type == W5500_MQTT_SUBACK || out->type == W5500_MQTT_PUBACK ||
        out->type == W5500_MQTT_UNSUBACK) {
        if (remain >= 2u) {
            out->packet_id = (uint16_t)(((uint16_t)buf[off] << 8) |
                                        buf[off + 1u]);
        }
        *consumed = total;
        return 0;
    }
    *consumed = total; /* PINGRESP and simple packets */
    return 0;
}

uint16_t w5500_mqtt_build_connect(uint8_t *buf, uint16_t cap,
                                  const char *client_id,
                                  uint16_t keepalive_s,
                                  uint8_t clean_session,
                                  const char *user, const char *pass)
{
    static const char magic[] = "MQTT";
    size_t client_len;
    size_t user_len = 0u;
    size_t pass_len = 0u;
    uint32_t remain;
    uint16_t off = 0u;
    uint8_t flags = (uint8_t)((clean_session != 0u) ? 0x02u : 0u);

    if (buf == NULL || client_id == NULL) {
        return 0u;
    }
    client_len = strlen(client_id);
    if (user != NULL) {
        user_len = strlen(user);
        flags |= 0x80u;
        if (pass != NULL) {
            pass_len = strlen(pass);
            flags |= 0x40u;
        }
    }
    remain = (uint32_t)(sizeof(magic) - 1u + 1u + 1u + 2u +
                        2u + client_len + (user != NULL ? 2u + user_len : 0u) +
                        (pass != NULL ? 2u + pass_len : 0u));
    if (remain > 0xFFFFu ||
        (uint32_t)(1u + mqtt_remain_bytes(remain) + remain) > (uint32_t)cap) {
        return 0u;
    }
    buf[off++] = (uint8_t)(W5500_MQTT_CONNECT << 4);
    off = (uint16_t)put_remain(buf, off, remain);
    memcpy(buf + off, magic, sizeof(magic) - 1u);
    off = (uint16_t)(off + sizeof(magic) - 1u);
    buf[off++] = 4u; /* protocol level 4 (3.1.1) */
    buf[off++] = flags;
    buf[off++] = (uint8_t)(keepalive_s >> 8);
    buf[off++] = (uint8_t)(keepalive_s & 0xFFu);
    off = (uint16_t)put_str(buf, off, client_id);
    if (user != NULL) {
        off = (uint16_t)put_str(buf, off, user);
    }
    if (pass != NULL) {
        off = (uint16_t)put_str(buf, off, pass);
    }
    return off;
}

uint16_t w5500_mqtt_build_publish(uint8_t *buf, uint16_t cap,
                                  const char *topic, const uint8_t *payload,
                                  uint16_t len, uint8_t retain)
{
    size_t tlen;
    uint32_t remain;
    uint16_t off = 0u;

    if (buf == NULL || topic == NULL || (len != 0u && payload == NULL)) {
        return 0u;
    }
    tlen = strlen(topic);
    remain = (uint32_t)(2u + tlen + len);
    if (remain > 0xFFFFu ||
        (uint32_t)(1u + mqtt_remain_bytes(remain) + remain) > (uint32_t)cap) {
        return 0u;
    }
    buf[off++] = (uint8_t)((W5500_MQTT_PUBLISH << 4) |
                           ((retain != 0u) ? 0x01u : 0u));
    off = (uint16_t)put_remain(buf, off, remain);
    buf[off++] = (uint8_t)(tlen >> 8);
    buf[off++] = (uint8_t)(tlen & 0xFFu);
    memcpy(buf + off, topic, tlen);
    off = (uint16_t)(off + tlen);
    if (len != 0u) {
        memcpy(buf + off, payload, len);
        off = (uint16_t)(off + len);
    }
    return off;
}

uint16_t w5500_mqtt_build_subscribe(uint8_t *buf, uint16_t cap,
                                    const char *topic, uint16_t packet_id,
                                    uint8_t qos)
{
    size_t tlen;
    uint32_t remain;
    uint16_t off = 0u;

    if (buf == NULL || topic == NULL || qos > 1u) {
        return 0u;
    }
    tlen = strlen(topic);
    remain = (uint32_t)(2u + 2u + tlen + 1u);
    if (remain > 0xFFFFu ||
        (uint32_t)(1u + mqtt_remain_bytes(remain) + remain) > (uint32_t)cap) {
        return 0u;
    }
    buf[off++] = (uint8_t)((W5500_MQTT_SUBSCRIBE << 4) | 0x02u);
    off = (uint16_t)put_remain(buf, off, remain);
    buf[off++] = (uint8_t)(packet_id >> 8);
    buf[off++] = (uint8_t)(packet_id & 0xFFu);
    buf[off++] = (uint8_t)(tlen >> 8);
    buf[off++] = (uint8_t)(tlen & 0xFFu);
    memcpy(buf + off, topic, tlen);
    off = (uint16_t)(off + tlen);
    buf[off++] = qos;
    return off;
}

uint16_t w5500_mqtt_build_simple(uint8_t *buf, uint16_t cap, uint8_t type)
{
    if (buf == NULL || cap < 2u) {
        return 0u;
    }
    buf[0] = (uint8_t)(type << 4);
    buf[1] = 0u;
    return 2u;
}

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */

static void mqtt_finish(w5500_mqtt_t *cli, w5500_mqtt_event_t ev)
{
    if (cli->sock != NULL) {
        (void)w5500_socket_close(cli->sock);
        cli->sock = NULL;
    }
    cli->state = MQTT_STATE_OFF;
    cli->connected = 0u;
    if (cli->cb != NULL) {
        cli->cb(cli, ev, NULL, 0u, NULL, 0u, cli->user);
    }
}

static void mqtt_on_event(w5500_socket_t sock, w5500_event_t event,
                         w5500_result_t status, void *user_arg)
{
    w5500_mqtt_t *cli = user_arg;
    (void)status;
    if (cli == NULL || sock == NULL || sock != cli->sock) {
        return;
    }
    switch (event) {
    case W5500_EVENT_CONNECTED:
        if (cli->state == MQTT_STATE_CONNECT) {
            cli->state = MQTT_STATE_WAITING;
        }
        break;
    case W5500_EVENT_DISCONNECTED:
    case W5500_EVENT_ERROR:
        mqtt_finish(cli, W5500_MQTT_EVENT_DISCONNECTED);
        break;
    default:
        break;
    }
}

static w5500_result_t mqtt_open(w5500_mqtt_t *cli)
{
    w5500_result_t rc;

    if (cli->sock != NULL) {
        return W5500_OK;
    }
    rc = w5500_socket_open_ex(cli->chip, W5500_PROTO_TCP, 0u,
                              W5500_SOCK_F_NO_DELAY, mqtt_on_event, cli,
                              &cli->sock);
    if (rc != W5500_OK) {
        return rc;
    }
    rc = w5500_socket_connect(cli->sock, cli->broker, cli->port);
    if (rc != W5500_OK) {
        (void)w5500_socket_close(cli->sock);
        cli->sock = NULL;
        return rc;
    }
    cli->rx_len = 0u;
    cli->state = MQTT_STATE_CONNECT;
    return W5500_OK;
}

size_t w5500_mqtt_size(void)
{
    return sizeof(struct w5500_mqtt);
}

w5500_result_t w5500_mqtt_init(w5500_mqtt_t **out_cli,
                               void *mem, size_t mem_size,
                               w5500_handle_t chip,
                               const uint8_t broker_ip[4], uint16_t port,
                               const char *client_id,
                               w5500_mqtt_cb_t cb, void *user_arg)
{
    struct w5500_mqtt *c;

    if (out_cli == NULL || mem == NULL || chip == NULL || broker_ip == NULL ||
        client_id == NULL || port == 0u || cb == NULL ||
        strlen(client_id) >= sizeof(((struct w5500_mqtt *)0)->client_id)) {
        return W5500_ERR_PARAM;
    }
    if (mem_size < sizeof(*c)) {
        return W5500_ERR_NOMEM;
    }
    c = (struct w5500_mqtt *)mem;
    memset(c, 0, sizeof(*c));
    c->chip = chip;
    memcpy(c->broker, broker_ip, 4u);
    c->port = port;
    strcpy(c->client_id, client_id);
    c->cb = cb;
    c->user = user_arg;
    *out_cli = c;
    return W5500_OK;
}

w5500_result_t w5500_mqtt_connect(w5500_mqtt_t *cli, uint32_t now_ms,
                                  uint16_t keepalive_s, uint8_t clean_session,
                                  const char *user, const char *pass)
{
    uint16_t n;
    w5500_result_t rc;

    if (cli == NULL || cli->state != MQTT_STATE_OFF) {
        return (cli != NULL) ? W5500_ERR_BUSY : W5500_ERR_PARAM;
    }
    n = w5500_mqtt_build_connect(cli->pending, sizeof(cli->pending),
                                 cli->client_id, keepalive_s, clean_session,
                                 user, pass);
    if (n == 0u) {
        return W5500_ERR_PARAM;
    }
    cli->keepalive_s = keepalive_s;
    cli->pending_len = n;
    cli->t_connect = now_ms;
    rc = mqtt_open(cli);
    if (rc != W5500_OK) {
        cli->pending_len = 0u;
    }
    return rc;
}

w5500_result_t w5500_mqtt_publish(w5500_mqtt_t *cli, uint32_t now_ms,
                                  const char *topic, const uint8_t *payload,
                                  uint16_t len, uint8_t retain)
{
    uint16_t n;

    (void)now_ms;
    if (cli == NULL || cli->state != MQTT_STATE_READY) {
        return (cli != NULL) ? W5500_ERR_STATE : W5500_ERR_PARAM;
    }
    if (cli->pending_len != 0u) {
        return W5500_ERR_BUSY;
    }
    n = w5500_mqtt_build_publish(cli->pending, sizeof(cli->pending),
                                 topic, payload, len, retain);
    if (n == 0u) {
        return W5500_ERR_PARAM;
    }
    cli->pending_len = n;
    return W5500_OK;
}

w5500_result_t w5500_mqtt_subscribe(w5500_mqtt_t *cli, uint32_t now_ms,
                                    const char *topic, uint8_t qos)
{
    uint16_t n;

    (void)now_ms;
    if (cli == NULL || cli->state != MQTT_STATE_READY) {
        return (cli != NULL) ? W5500_ERR_STATE : W5500_ERR_PARAM;
    }
    if (cli->pending_len != 0u) {
        return W5500_ERR_BUSY;
    }
    cli->packet_id++;
    n = w5500_mqtt_build_subscribe(cli->pending, sizeof(cli->pending),
                                   topic, cli->packet_id, qos);
    if (n == 0u) {
        return W5500_ERR_PARAM;
    }
    cli->pending_len = n;
    return W5500_OK;
}

static void mqtt_handle_rx(w5500_mqtt_t *cli)
{
    uint16_t consumed = 0u;
    w5500_mqtt_packet_t pkt;

    while (cli->rx_len >= 2u) {
        if (w5500_mqtt_parse(cli->rx, cli->rx_len, &pkt, &consumed) != 0 ||
            consumed == 0u || consumed > cli->rx_len) {
            cli->rx_len = 0u;
            break;
        }
        if (pkt.type == W5500_MQTT_CONNACK) {
            if (pkt.flags != 0u) {
                mqtt_finish(cli, W5500_MQTT_EVENT_ERROR);
                return;
            }
            cli->connected = 1u;
            cli->state = MQTT_STATE_READY;
            cli->t_last = 0u;
            if (cli->cb != NULL) {
                cli->cb(cli, W5500_MQTT_EVENT_CONNECTED, NULL, 0u, NULL, 0u,
                        cli->user);
            }
        } else if (pkt.type == W5500_MQTT_PUBLISH) {
            if (cli->cb != NULL) {
                cli->cb(cli, W5500_MQTT_EVENT_PUBLISH, pkt.topic,
                        pkt.topic_len, pkt.payload, pkt.payload_len,
                        cli->user);
            }
        } else if (pkt.type == W5500_MQTT_SUBACK) {
            if (cli->cb != NULL) {
                cli->cb(cli, W5500_MQTT_EVENT_SUBACK, NULL, 0u, NULL, 0u,
                        cli->user);
            }
        }
        memmove(cli->rx, cli->rx + consumed, (size_t)(cli->rx_len - consumed));
        cli->rx_len = (uint16_t)(cli->rx_len - consumed);
    }
}

w5500_result_t w5500_mqtt_poll(w5500_mqtt_t *cli, uint32_t now_ms)
{
    uint8_t chunk[128];

    if (cli == NULL) {
        return W5500_ERR_PARAM;
    }
    if (cli->state == MQTT_STATE_OFF) {
        return W5500_ERR_STATE;
    }
    if (cli->t_last == 0u) {
        cli->t_last = now_ms;
    }

    if (cli->pending_len != 0u && cli->sock != NULL) {
        uint16_t sent = 0u;
        w5500_result_t rc = w5500_socket_send(cli->sock, cli->pending,
                                              cli->pending_len, &sent);
        if (rc == W5500_ERR_BUSY) {
            /* retry next tick */
        } else if (rc != W5500_OK || sent != cli->pending_len) {
            mqtt_finish(cli, W5500_MQTT_EVENT_ERROR);
            return W5500_OK;
        } else {
            cli->pending_len = 0u;
        }
    }

    while (cli->rx_len < sizeof(cli->rx) && cli->sock != NULL) {
        uint16_t room = (uint16_t)(sizeof(cli->rx) - cli->rx_len);
        uint16_t n = 0u;

        if (room > sizeof(chunk)) {
            room = sizeof(chunk);
        }
        if (w5500_socket_recv(cli->sock, cli->rx + cli->rx_len, room, &n) !=
            W5500_OK || n == 0u) {
            break;
        }
        cli->rx_len = (uint16_t)(cli->rx_len + n);
    }
    if (cli->rx_len != 0u) {
        mqtt_handle_rx(cli);
        if (cli->state == MQTT_STATE_OFF) {
            return W5500_OK;
        }
    }

    if (cli->state == MQTT_STATE_CONNECT || cli->state == MQTT_STATE_WAITING) {
        if ((int32_t)(now_ms - cli->t_connect) > 5000) {
            mqtt_finish(cli, W5500_MQTT_EVENT_ERROR);
        }
        return W5500_OK;
    }

    if (cli->keepalive_s != 0u && cli->pending_len == 0u) {
        uint32_t interval = (uint32_t)cli->keepalive_s * 500u;
        if ((int32_t)(now_ms - cli->t_last) >= (int32_t)interval) {
            uint16_t n = w5500_mqtt_build_simple(cli->pending,
                                                 sizeof(cli->pending),
                                                 W5500_MQTT_PINGREQ);
            if (n != 0u) {
                cli->pending_len = n;
            }
            cli->t_last = now_ms;
        }
    }
    return W5500_OK;
}

void w5500_mqtt_stop(w5500_mqtt_t *cli)
{
    if (cli == NULL) {
        return;
    }
    if (cli->sock != NULL) {
        (void)w5500_socket_close(cli->sock);
        cli->sock = NULL;
    }
    cli->state = MQTT_STATE_OFF;
    cli->connected = 0u;
    cli->pending_len = 0u;
    cli->rx_len = 0u;
}
