/**
 * @file w5500_dns.c
 * @brief Minimal DNS A-record resolver over one driver UDP socket.
 *
 * Query encoding and response parsing are bounds checked; the resolver
 * itself is a small waiting state machine driven by w5500_dns_poll().
 */
#include <string.h>

#include "w5500_dns.h"
#include "w5500_sockets.h"
#include "w5500_dns_internal.h"

#define DNS_MAX_PKT        512u
#define DNS_HEADER_LEN     12u
#define DNS_SERVER_PORT    53u
#define DNS_LOCAL_PORT     53535u
#define DNS_RETRY_MS       2000u
#define DNS_MAX_ATTEMPTS   3u

#define DNS_STATE_OFF      0u
#define DNS_STATE_WAITING  1u

struct w5500_dns {
    w5500_handle_t  chip;
    w5500_socket_t  sock;

    uint8_t         server[4];
    char            name[254];
    uint32_t        qid_counter;
    uint16_t        qid;

    uint32_t        t_last;
    uint8_t         state;
    uint8_t         attempts;

    w5500_dns_cb_t  cb;
    void           *user;

    uint8_t         rx[DNS_MAX_PKT];
    uint8_t         tx[DNS_MAX_PKT];
};

/* ------------------------------------------------------------------ */
/* Name / packet helpers                                               */
/* ------------------------------------------------------------------ */

static int dns_name_valid(const char *name)
{
    size_t total = 0u;
    size_t label = 0u;

    if (name == NULL) {
        return 0;
    }
    while (*name != '\0') {
        if (*name == '.') {
            if (label == 0u || label > 63u) {
                return 0;
            }
            label = 0u;
        } else {
            label++;
        }
        if (++total > 253u) {
            return 0;
        }
        name++;
    }
    return (label > 0u && label <= 63u);
}

static int dns_skip_name(const uint8_t *pkt, uint16_t len, uint16_t *off)
{
    uint16_t o = *off;
    unsigned guard = 0u;

    while (o < len) {
        uint8_t l = pkt[o];

        if ((l & 0xC0u) == 0xC0u) {
            if ((uint32_t)o + 2u > len) {
                return -1;
            }
            *off = (uint16_t)(o + 2u); /* compression pointer ends the name */
            return 0;
        }
        if ((l & 0xC0u) != 0u) {
            return -1;
        }
        o++;
        if (l == 0u) {
            *off = o;
            return 0;
        }
        if ((uint32_t)o + (uint32_t)l > (uint32_t)len) {
            return -1;
        }
        o = (uint16_t)(o + l);
        if (++guard > 128u) {
            return -1;
        }
    }
    return -1;
}

int w5500_dns_parse_response(const uint8_t *pkt, uint16_t len, uint16_t qid,
                             uint8_t ip_out[4])
{
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t off;
    uint16_t i;

    if (pkt == NULL || ip_out == NULL || len < DNS_HEADER_LEN) {
        return -1;
    }
    if ((((uint16_t)pkt[0] << 8) | pkt[1]) != qid) {
        return -1;
    }
    flags = (uint16_t)(((uint16_t)pkt[2] << 8) | pkt[3]);
    if ((flags & 0x8000u) == 0u) {
        return -1; /* not a response */
    }
    if ((flags & 0x000Fu) != 0u) {
        return -1; /* RCODE != NOERROR */
    }
    qdcount = (uint16_t)(((uint16_t)pkt[4] << 8) | pkt[5]);
    ancount = (uint16_t)(((uint16_t)pkt[6] << 8) | pkt[7]);

    off = DNS_HEADER_LEN;
    for (i = 0; i < qdcount; i++) {
        if (dns_skip_name(pkt, len, &off) != 0 ||
            (uint32_t)off + 4u > (uint32_t)len) {
            return -1;
        }
        off = (uint16_t)(off + 4u);
    }

    for (i = 0; i < ancount; i++) {
        uint16_t type;
        uint16_t cls;
        uint16_t rdlen;

        if (dns_skip_name(pkt, len, &off) != 0) {
            return -1;
        }
        if ((uint32_t)off + 10u > (uint32_t)len) {
            return -1;
        }
        type = (uint16_t)(((uint16_t)pkt[off] << 8) | pkt[off + 1u]);
        cls = (uint16_t)(((uint16_t)pkt[off + 2u] << 8) | pkt[off + 3u]);
        rdlen = (uint16_t)(((uint16_t)pkt[off + 8u] << 8) | pkt[off + 9u]);
        off = (uint16_t)(off + 10u);
        if ((uint32_t)off + (uint32_t)rdlen > (uint32_t)len) {
            return -1;
        }
        if (type == 1u && cls == 1u && rdlen == 4u) {
            memcpy(ip_out, pkt + off, 4u);
            return 0;
        }
        off = (uint16_t)(off + rdlen);
    }
    return -1;
}

static uint16_t dns_build_query(struct w5500_dns *d)
{
    uint8_t *b = d->tx;
    const char *p = d->name;
    uint16_t off = DNS_HEADER_LEN;

    memset(b, 0, sizeof(d->tx));
    b[0] = (uint8_t)(d->qid >> 8);
    b[1] = (uint8_t)d->qid;
    b[2] = 0x01u; /* RD */
    b[3] = 0x00u;
    b[5] = 1u; /* QDCOUNT = 1 */

    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t l = (dot != NULL) ? (size_t)(dot - p) : strlen(p);

        b[off++] = (uint8_t)l;
        memcpy(b + off, p, l);
        off = (uint16_t)(off + l);
        p += l;
        if (*p == '.') {
            p++;
        }
    }
    b[off++] = 0u;
    b[off++] = 0u;
    b[off++] = 1u; /* QTYPE A */
    b[off++] = 0u;
    b[off++] = 1u; /* QCLASS IN */
    return off;
}

static void dns_noop_cb(w5500_socket_t sock, w5500_event_t event,
                        w5500_result_t status, void *user_arg)
{
    (void)sock;
    (void)event;
    (void)status;
    (void)user_arg;
}

static void dns_finish(struct w5500_dns *d, const uint8_t ip[4],
                       w5500_result_t status)
{
    static const uint8_t zero[4] = {0u, 0u, 0u, 0u};

    d->state = DNS_STATE_OFF;
    if (d->cb != NULL) {
        d->cb(d, (ip != NULL) ? ip : zero, status, d->user);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle / state machine                                           */
/* ------------------------------------------------------------------ */

size_t w5500_dns_size(void)
{
    return sizeof(struct w5500_dns);
}

w5500_result_t w5500_dns_init(w5500_dns_t **out_dns,
                              void *mem,
                              size_t mem_size,
                              w5500_handle_t chip,
                              const uint8_t server[4],
                              w5500_dns_cb_t cb,
                              void *user_arg)
{
    struct w5500_dns *d;

    if (out_dns == NULL || mem == NULL || chip == NULL || server == NULL ||
        cb == NULL) {
        return W5500_ERR_PARAM;
    }
    if (mem_size < sizeof(*d)) {
        return W5500_ERR_NOMEM;
    }
    d = (struct w5500_dns *)mem;
    memset(d, 0, sizeof(*d));
    d->chip = chip;
    memcpy(d->server, server, 4u);
    d->cb = cb;
    d->user = user_arg;
    d->state = DNS_STATE_OFF;
    *out_dns = d;
    return W5500_OK;
}

w5500_result_t w5500_dns_set_server(w5500_dns_t *dns, const uint8_t server[4])
{
    if (dns == NULL || server == NULL) {
        return W5500_ERR_PARAM;
    }
    memcpy(dns->server, server, 4u);
    return W5500_OK;
}

w5500_result_t w5500_dns_start(w5500_dns_t *dns, const char *name,
                               uint32_t now_ms)
{
    uint16_t len;
    uint16_t sent = 0u;
    w5500_result_t rc;

    if (dns == NULL || !dns_name_valid(name)) {
        return W5500_ERR_PARAM;
    }

    if (dns->sock == NULL) {
        rc = w5500_socket_open(dns->chip, W5500_PROTO_UDP, DNS_LOCAL_PORT,
                               dns_noop_cb, dns, &dns->sock);
        if (rc != W5500_OK) {
            dns->sock = NULL;
            return rc;
        }
    }

    memset(dns->name, 0, sizeof(dns->name));
    strncpy(dns->name, name, sizeof(dns->name) - 1u);

    dns->qid_counter = dns->qid_counter * 1664525u + 1013904223u;
    dns->qid = (uint16_t)(dns->qid_counter >> 16);
    dns->state = DNS_STATE_WAITING;
    dns->attempts = 1u;
    dns->t_last = now_ms;

    len = dns_build_query(dns);
    rc = w5500_socket_sendto(dns->sock, dns->server, DNS_SERVER_PORT, dns->tx,
                             len, &sent);
    if (rc == W5500_ERR_BUSY) {
        return W5500_OK; /* retried from poll() on the next tick */
    }
    return rc;
}

w5500_result_t w5500_dns_poll(w5500_dns_t *dns, uint32_t now_ms)
{
    uint8_t packets = 0u;

    if (dns == NULL) {
        return W5500_ERR_PARAM;
    }
    if (dns->state != DNS_STATE_WAITING) {
        return W5500_ERR_STATE;
    }

    while (packets < 4u) {
        uint16_t n = 0u;
        w5500_result_t rc = w5500_socket_recv(dns->sock, dns->rx,
                                              (uint16_t)sizeof(dns->rx), &n);
        if (rc != W5500_OK) {
            return rc;
        }
        if (n == 0u) {
            break;
        }
        if (n >= DNS_HEADER_LEN &&
            ((((uint16_t)dns->rx[0] << 8) | dns->rx[1]) == dns->qid)) {
            uint16_t flags = (uint16_t)(((uint16_t)dns->rx[2] << 8) |
                                        dns->rx[3]);
            uint8_t ip[4];

            if ((flags & 0x8000u) != 0u && (flags & 0x000Fu) != 0u) {
                dns_finish(dns, NULL, W5500_ERR_STATE); /* NXDOMAIN / SERVFAIL */
                return W5500_OK;
            }
            if (w5500_dns_parse_response(dns->rx, n, dns->qid, ip) == 0) {
                dns_finish(dns, ip, W5500_OK);
                return W5500_OK;
            }
        }
        packets++;
    }

    if ((int32_t)(now_ms - dns->t_last) >= (int32_t)DNS_RETRY_MS) {
        uint16_t sent = 0u;
        uint16_t len;
        w5500_result_t rc;

        if (dns->attempts >= DNS_MAX_ATTEMPTS) {
            dns_finish(dns, NULL, W5500_ERR_TIMEOUT);
            return W5500_OK;
        }
        len = dns_build_query(dns);
        rc = w5500_socket_sendto(dns->sock, dns->server, DNS_SERVER_PORT,
                                 dns->tx, len, &sent);
        if (rc != W5500_OK && rc != W5500_ERR_BUSY) {
            dns_finish(dns, NULL, rc);
            return W5500_OK;
        }
        dns->t_last = now_ms;
        dns->attempts++;
    }
    return W5500_OK;
}

void w5500_dns_stop(w5500_dns_t *dns)
{
    if (dns == NULL) {
        return;
    }
    if (dns->sock != NULL) {
        (void)w5500_socket_close(dns->sock);
        dns->sock = NULL;
    }
    dns->state = DNS_STATE_OFF;
}
