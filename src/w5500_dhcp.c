/**
 * @file w5500_dhcp.c
 * @brief Minimal DHCP client: DISCOVER/OFFER/REQUEST/ACK with renew.
 *
 * The client owns exactly one driver UDP socket (port 68) and is purely
 * tick driven. All packets are built into a fixed transmit buffer inside
 * the caller-provided state block; replies are received into a fixed RX
 * buffer, so no heap and no dynamic lengths are involved.
 */
#include <string.h>

#include "w5500_dhcp.h"
#include "w5500_sockets.h"
#include "w5500_dhcp_internal.h"

#define DHCP_MAX_PKT         576u
#define DHCP_FIXED_LEN       240u
#define DHCP_COOKIE          0x63825363u

#define DHCP_SERVER_PORT     67u
#define DHCP_CLIENT_PORT     68u

#define DHCP_RETRY_MS        4000u
#define DHCP_MAX_ATTEMPTS    4u
#define DHCP_DEFAULT_LEASE_S 3600u
#define DHCP_MAX_LEASE_S     (604800u) /* one week, protects the ms math */

/* Message types (option 53). */
#define DHCP_DISCOVER        1u
#define DHCP_OFFER           2u
#define DHCP_REQUEST         3u
#define DHCP_ACK             5u
#define DHCP_NAK             6u

/* Client state machine. */
#define DHCP_STATE_OFF        0u
#define DHCP_STATE_SELECTING  1u
#define DHCP_STATE_REQUESTING 2u
#define DHCP_STATE_BOUND      3u
#define DHCP_STATE_RENEWING   4u

struct w5500_dhcp {
    w5500_handle_t   chip;
    w5500_socket_t   sock;

    uint8_t          mac[6];
    uint32_t         xid_seed;

    uint32_t         t_last;    /**< last transmit time */
    uint32_t         t_renew;   /**< absolute deadline */
    uint32_t         t_expire;  /**< absolute deadline */
    uint32_t         lease_ms;

    uint8_t          state;
    uint8_t          attempts;

    uint8_t          ip[4];
    uint8_t          mask[4];
    uint8_t          gw[4];
    uint8_t          dns[4];
    uint8_t          server[4];

    w5500_dhcp_cb_t  cb;
    void            *user;

    uint8_t          rx[DHCP_MAX_PKT];
    uint8_t          tx[DHCP_MAX_PKT];
};

/* ------------------------------------------------------------------ */
/* Byte helpers                                                        */
/* ------------------------------------------------------------------ */

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int ip_nonzero(const uint8_t ip[4])
{
    return (ip[0] | ip[1] | ip[2] | ip[3]) != 0;
}

/** Wrap-safe signed difference: >0 when @p now is past @p then. */
static int32_t elapsed_since(uint32_t now, uint32_t then)
{
    return (int32_t)(now - then);
}

/* ------------------------------------------------------------------ */
/* Packet codec                                                        */
/* ------------------------------------------------------------------ */

int w5500_dhcp_parse_packet(const uint8_t *pkt, uint16_t len,
                            w5500_dhcp_parse_t *out)
{
    uint16_t off;

    if (pkt == NULL || out == NULL || len < DHCP_FIXED_LEN) {
        return -1;
    }
    if (pkt[0] != 2u) {
        return -1; /* BOOTREPLY only */
    }
    if (get32(pkt + 236u) != DHCP_COOKIE) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->ip, pkt + 16u, 4u); /* yiaddr */
    out->has_ip = (uint8_t)(ip_nonzero(out->ip) ? 1u : 0u);

    off = DHCP_FIXED_LEN;
    while (off < len) {
        uint8_t code = pkt[off++];
        uint8_t olen;

        if (code == 0u) {
            continue; /* padding */
        }
        if (code == 255u) {
            break; /* end of options */
        }
        if (off >= len) {
            return -1;
        }
        olen = pkt[off++];
        if ((uint32_t)off + (uint32_t)olen > (uint32_t)len) {
            return -1;
        }
        switch (code) {
        case 53u:
            if (olen >= 1u) {
                out->msg_type = pkt[off];
            }
            break;
        case 54u:
            if (olen == 4u) {
                memcpy(out->server, pkt + off, 4u);
                out->has_server = 1u;
            }
            break;
        case 51u:
            if (olen == 4u) {
                uint32_t s = get32(pkt + off);
                if (s != 0u) {
                    out->lease_s = (s > DHCP_MAX_LEASE_S) ? DHCP_MAX_LEASE_S : s;
                    out->has_lease = 1u;
                }
            }
            break;
        case 1u:
            if (olen == 4u) {
                memcpy(out->mask, pkt + off, 4u);
                out->has_mask = 1u;
            }
            break;
        case 3u:
            if (olen >= 4u) {
                memcpy(out->gw, pkt + off, 4u);
                out->has_gw = 1u;
            }
            break;
        case 6u:
            if (olen >= 4u) {
                memcpy(out->dns, pkt + off, 4u);
                out->has_dns = 1u;
            }
            break;
        default:
            break;
        }
        off = (uint16_t)(off + olen);
    }
    return (out->msg_type != 0u) ? 0 : -1;
}

/**
 * Build a request packet in d->tx and return its length.
 * @param requested_ip  Option 50 target; NULL omits it (DISCOVER).
 * @param server        Option 54 server id; NULL omits it.
 */
static uint16_t dhcp_build(struct w5500_dhcp *d, uint8_t type,
                           const uint8_t *requested_ip,
                           const uint8_t *server)
{
    uint8_t *b = d->tx;
    uint16_t off;

    memset(b, 0, DHCP_MAX_PKT);
    b[0] = 1u; /* BOOTREQUEST */
    b[1] = 1u; /* Ethernet */
    b[2] = 6u; /* hlen */
    put32(b + 4u, d->xid_seed);
    put16(b + 10u, 0x8000u); /* broadcast flag */
    memcpy(b + 28u, d->mac, 6u); /* chaddr */
    b[236] = 0x63u;
    b[237] = 0x82u;
    b[238] = 0x53u;
    b[239] = 0x63u;

    off = DHCP_FIXED_LEN;
    b[off++] = 53u;
    b[off++] = 1u;
    b[off++] = type;

    b[off++] = 61u; /* client identifier */
    b[off++] = 7u;
    b[off++] = 1u;
    memcpy(b + off, d->mac, 6u);
    off = (uint16_t)(off + 6u);

    if (requested_ip != NULL) {
        b[off++] = 50u;
        b[off++] = 4u;
        memcpy(b + off, requested_ip, 4u);
        off = (uint16_t)(off + 4u);
    }
    if (server != NULL) {
        b[off++] = 54u;
        b[off++] = 4u;
        memcpy(b + off, server, 4u);
        off = (uint16_t)(off + 4u);
    }

    b[off++] = 55u; /* parameter request list: mask, router, DNS */
    b[off++] = 3u;
    b[off++] = 1u;
    b[off++] = 3u;
    b[off++] = 6u;
    b[off++] = 255u;
    return off;
}

static uint32_t dhcp_next_xid(struct w5500_dhcp *d)
{
    /* Deterministic, cheap, not security relevant. */
    d->xid_seed = d->xid_seed * 1664525u + 1013904223u;
    return d->xid_seed;
}

static w5500_result_t dhcp_transmit(struct w5500_dhcp *d, uint16_t len)
{
    static const uint8_t broadcast[4] = {255u, 255u, 255u, 255u};
    uint16_t sent = 0u;
    w5500_result_t rc;

    rc = w5500_socket_sendto(d->sock, broadcast, DHCP_SERVER_PORT, d->tx, len,
                             &sent);
    if (rc == W5500_ERR_BUSY) {
        return W5500_OK; /* previous SEND pending; retransmit later */
    }
    return rc;
}

static void dhcp_noop_cb(w5500_socket_t sock, w5500_event_t event,
                         w5500_result_t status, void *user_arg)
{
    (void)sock;
    (void)event;
    (void)status;
    (void)user_arg;
}

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */

static void dhcp_apply_lease(struct w5500_dhcp *d,
                             const w5500_dhcp_parse_t *m,
                             uint32_t now, int renewed)
{
    uint32_t lease_s = m->has_lease ? m->lease_s : DHCP_DEFAULT_LEASE_S;

    d->lease_ms = lease_s * 1000u;
    if (m->has_mask) {
        memcpy(d->mask, m->mask, 4u);
    }
    if (m->has_gw) {
        memcpy(d->gw, m->gw, 4u);
    }
    if (m->has_dns) {
        memcpy(d->dns, m->dns, 4u);
    }
    d->t_renew = now + d->lease_ms / 2u;
    d->t_expire = now + d->lease_ms;
    d->state = DHCP_STATE_BOUND;
    d->attempts = 0u;

    (void)w5500_set_ip(d->chip, d->ip);
    (void)w5500_set_subnet_mask(d->chip, d->mask);
    (void)w5500_set_gateway(d->chip, d->gw);

    if (d->cb != NULL) {
        d->cb(d, renewed ? W5500_DHCP_EVENT_RENEWED : W5500_DHCP_EVENT_BOUND,
              d->user);
    }
}

static void dhcp_handle_reply(struct w5500_dhcp *d, const uint8_t *pkt,
                              uint16_t len, uint32_t now)
{
    w5500_dhcp_parse_t m;
    uint16_t out_len;

    if (w5500_dhcp_parse_packet(pkt, len, &m) != 0) {
        return;
    }
    if (get32(pkt + 4u) != d->xid_seed) {
        return; /* not our transaction */
    }

    switch (m.msg_type) {
    case DHCP_OFFER:
        if (d->state != DHCP_STATE_SELECTING || !m.has_ip || !m.has_server) {
            return;
        }
        memcpy(d->ip, m.ip, 4u);
        memcpy(d->server, m.server, 4u);
        out_len = dhcp_build(d, DHCP_REQUEST, d->ip, d->server);
        (void)dhcp_transmit(d, out_len);
        d->state = DHCP_STATE_REQUESTING;
        d->t_last = now;
        d->attempts = 1u;
        break;
    case DHCP_ACK:
        if (d->state != DHCP_STATE_REQUESTING &&
            d->state != DHCP_STATE_RENEWING) {
            return;
        }
        if (!m.has_ip) {
            return;
        }
        memcpy(d->ip, m.ip, 4u);
        if (m.has_server) {
            memcpy(d->server, m.server, 4u);
        }
        dhcp_apply_lease(d, &m, now,
                         d->state == DHCP_STATE_RENEWING ? 1 : 0);
        break;
    case DHCP_NAK:
        if (d->state != DHCP_STATE_REQUESTING &&
            d->state != DHCP_STATE_RENEWING) {
            return;
        }
        d->xid_seed = dhcp_next_xid(d);
        out_len = dhcp_build(d, DHCP_DISCOVER, NULL, NULL);
        (void)dhcp_transmit(d, out_len);
        d->state = DHCP_STATE_SELECTING;
        d->t_last = now;
        d->attempts = 1u;
        break;
    default:
        break;
    }
}

static void dhcp_fail(struct w5500_dhcp *d)
{
    w5500_dhcp_stop(d);
    if (d->cb != NULL) {
        d->cb(d, W5500_DHCP_EVENT_FAILED, d->user);
    }
}

w5500_result_t w5500_dhcp_poll(w5500_dhcp_t *dhcp, uint32_t now_ms)
{
    uint8_t packets = 0u;

    if (dhcp == NULL) {
        return W5500_ERR_PARAM;
    }
    if (dhcp->state == DHCP_STATE_OFF) {
        return W5500_ERR_STATE;
    }

    /* Drain a few replies per call so a burst cannot starve the caller. */
    while (packets < 4u) {
        uint16_t n = 0u;
        w5500_result_t rc = w5500_socket_recv(dhcp->sock, dhcp->rx,
                                              (uint16_t)sizeof(dhcp->rx), &n);
        if (rc != W5500_OK) {
            return rc;
        }
        if (n == 0u) {
            break;
        }
        dhcp_handle_reply(dhcp, dhcp->rx, n, now_ms);
        packets++;
        if (dhcp->state == DHCP_STATE_OFF) {
            return W5500_OK;
        }
    }

    if (dhcp->state == DHCP_STATE_SELECTING ||
        dhcp->state == DHCP_STATE_REQUESTING ||
        dhcp->state == DHCP_STATE_RENEWING) {
        if (elapsed_since(now_ms, dhcp->t_last) >= (int32_t)DHCP_RETRY_MS) {
            uint16_t len;

            if (dhcp->attempts >= DHCP_MAX_ATTEMPTS) {
                dhcp_fail(dhcp);
                return W5500_OK;
            }
            if (dhcp->state == DHCP_STATE_REQUESTING) {
                len = dhcp_build(dhcp, DHCP_REQUEST, dhcp->ip, dhcp->server);
            } else if (dhcp->state == DHCP_STATE_RENEWING) {
                len = dhcp_build(dhcp, DHCP_REQUEST, dhcp->ip, dhcp->server);
            } else {
                len = dhcp_build(dhcp, DHCP_DISCOVER, NULL, NULL);
            }
            (void)dhcp_transmit(dhcp, len);
            dhcp->t_last = now_ms;
            dhcp->attempts++;
        }
    } else if (dhcp->state == DHCP_STATE_BOUND) {
        if (elapsed_since(now_ms, dhcp->t_renew) >= 0) {
            uint16_t len = dhcp_build(dhcp, DHCP_REQUEST, dhcp->ip,
                                      dhcp->server);
            (void)dhcp_transmit(dhcp, len);
            dhcp->state = DHCP_STATE_RENEWING;
            dhcp->t_last = now_ms;
            dhcp->attempts = 1u;
        }
    }

    if (dhcp->state == DHCP_STATE_RENEWING &&
        elapsed_since(now_ms, dhcp->t_expire) >= 0) {
        uint16_t len;
        dhcp->xid_seed = dhcp_next_xid(dhcp);
        len = dhcp_build(dhcp, DHCP_DISCOVER, NULL, NULL);
        (void)dhcp_transmit(dhcp, len);
        dhcp->state = DHCP_STATE_SELECTING;
        dhcp->t_last = now_ms;
        dhcp->attempts = 1u;
    }

    return W5500_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

size_t w5500_dhcp_size(void)
{
    return sizeof(struct w5500_dhcp);
}

w5500_result_t w5500_dhcp_init(w5500_dhcp_t **out_dhcp,
                               void *mem,
                               size_t mem_size,
                               w5500_handle_t chip,
                               const uint8_t mac[6],
                               w5500_dhcp_cb_t cb,
                               void *user_arg)
{
    struct w5500_dhcp *d;

    if (out_dhcp == NULL || mem == NULL || chip == NULL || mac == NULL ||
        cb == NULL) {
        return W5500_ERR_PARAM;
    }
    if (mem_size < sizeof(*d)) {
        return W5500_ERR_NOMEM;
    }
    d = (struct w5500_dhcp *)mem;
    memset(d, 0, sizeof(*d));
    d->chip = chip;
    memcpy(d->mac, mac, 6u);
    d->cb = cb;
    d->user = user_arg;
    d->state = DHCP_STATE_OFF;
    *out_dhcp = d;
    return W5500_OK;
}

w5500_result_t w5500_dhcp_start(w5500_dhcp_t *dhcp, uint32_t now_ms)
{
    uint16_t len;
    w5500_result_t rc;

    if (dhcp == NULL) {
        return W5500_ERR_PARAM;
    }
    if (dhcp->state != DHCP_STATE_OFF) {
        w5500_dhcp_stop(dhcp);
    }

    rc = w5500_socket_open(dhcp->chip, W5500_PROTO_UDP, DHCP_CLIENT_PORT,
                           dhcp_noop_cb, dhcp, &dhcp->sock);
    if (rc != W5500_OK) {
        dhcp->sock = NULL;
        return rc;
    }

    dhcp->xid_seed = dhcp_next_xid(dhcp);
    dhcp->state = DHCP_STATE_SELECTING;
    dhcp->t_last = now_ms;
    dhcp->attempts = 1u;

    len = dhcp_build(dhcp, DHCP_DISCOVER, NULL, NULL);
    rc = dhcp_transmit(dhcp, len);
    if (rc != W5500_OK) {
        w5500_dhcp_stop(dhcp);
    }
    return rc;
}

void w5500_dhcp_stop(w5500_dhcp_t *dhcp)
{
    if (dhcp == NULL) {
        return;
    }
    if (dhcp->sock != NULL) {
        (void)w5500_socket_close(dhcp->sock);
        dhcp->sock = NULL;
    }
    dhcp->state = DHCP_STATE_OFF;
}

w5500_result_t w5500_dhcp_get_config(const w5500_dhcp_t *dhcp,
                                     uint8_t ip[4],
                                     uint8_t mask[4],
                                     uint8_t gw[4],
                                     uint8_t dns[4])
{
    if (dhcp == NULL) {
        return W5500_ERR_PARAM;
    }
    if (ip != NULL) {
        memcpy(ip, dhcp->ip, 4u);
    }
    if (mask != NULL) {
        memcpy(mask, dhcp->mask, 4u);
    }
    if (gw != NULL) {
        memcpy(gw, dhcp->gw, 4u);
    }
    if (dns != NULL) {
        memcpy(dns, dhcp->dns, 4u);
    }
    return W5500_OK;
}
