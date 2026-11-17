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
#define DNS_CACHE_SLOTS    4u
#define DNS_CACHE_NAME_LEN 128u
#define DNS_NAME_MAX       254u

#define DNS_STATE_OFF      0u
#define DNS_STATE_WAITING  1u

typedef struct {
    char     name[DNS_CACHE_NAME_LEN];
    uint8_t  ip[4];
    uint32_t expire; /* absolute ms, saturated at 0x7FFFFFFF */
    uint8_t  used;
} dns_cache_entry_t;

struct w5500_dns {
    w5500_handle_t  chip;
    w5500_socket_t  sock;

    uint8_t         server[4];
    char            name[DNS_NAME_MAX];
    uint32_t        qid_counter;
    uint16_t        qid;

    uint32_t        t_last;
    uint8_t         state;
    uint8_t         attempts;
    uint8_t         cache_on;
    uint8_t         from_cache;
    uint8_t         cache_slot;
    w5500_dns_addr_t cache_hit;

    w5500_dns_cb_t       cb;
    w5500_dns_multi_cb_t multi_cb;
    void                *user;

    dns_cache_entry_t cache[DNS_CACHE_SLOTS];
    uint8_t           rx[DNS_MAX_PKT];
    uint8_t           tx[DNS_MAX_PKT];
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

/** Decode a (possibly compressed) name into a lowercase dotted string. */
static int dns_decode_name(const uint8_t *pkt, uint16_t len, uint16_t off,
                           char *out, size_t cap, uint16_t *end_off)
{
    uint16_t o = off;
    uint16_t jumps = 0u;
    uint16_t labels = 0u;
    size_t n = 0u;
    int followed = 0;

    if (pkt == NULL || out == NULL || end_off == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    for (;;) {
        uint8_t l;
        uint16_t i;

        if ((uint32_t)o >= (uint32_t)len) {
            return -1;
        }
        l = pkt[o];
        if ((l & 0xC0u) == 0xC0u) {
            uint16_t target;
            if ((uint32_t)o + 2u > (uint32_t)len) {
                return -1;
            }
            target = (uint16_t)((((uint16_t)(l & 0x3Fu)) << 8) |
                                pkt[o + 1u]);
            if ((uint32_t)target >= (uint32_t)len) {
                return -1;
            }
            if (!followed) {
                *end_off = (uint16_t)(o + 2u);
            }
            if (++jumps > 16u) {
                return -1;
            }
            o = target;
            followed = 1;
            continue;
        }
        if ((l & 0xC0u) != 0u) {
            return -1;
        }
        o = (uint16_t)(o + 1u);
        if (l == 0u) {
            if (!followed) {
                *end_off = o;
            }
            out[n] = '\0';
            return 0;
        }
        if ((uint32_t)o + (uint32_t)l > (uint32_t)len) {
            return -1;
        }
        if (n > 0u) {
            if (n + 1u >= cap) {
                return -1;
            }
            out[n++] = '.';
        }
        if (n + (size_t)l >= cap) {
            return -1;
        }
        for (i = 0u; i < (uint16_t)l; i++) {
            uint8_t c = pkt[o + i];
            out[n++] = (c >= (uint8_t)'A' && c <= (uint8_t)'Z')
                           ? (char)(c + 32u) : (char)c;
        }
        o = (uint16_t)(o + l);
        if (++labels > 128u) {
            return -1;
        }
    }
}

static int dns_name_eq(const char *a, const char *b)
{
    size_t i;

    if (a == NULL || b == NULL) {
        return 0;
    }
    for (i = 0u; a[i] != '\0' && b[i] != '\0'; i++) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];
        if (ca >= (uint8_t)'A' && ca <= (uint8_t)'Z') {
            ca = (uint8_t)(ca + 32u);
        }
        if (cb >= (uint8_t)'A' && cb <= (uint8_t)'Z') {
            cb = (uint8_t)(cb + 32u);
        }
        if (ca != cb) {
            return 0;
        }
    }
    return a[i] == '\0' && b[i] == '\0';
}

int w5500_dns_parse_response_ex(const uint8_t *pkt, uint16_t len, uint16_t qid,
                                const char *wanted, w5500_dns_addr_t *addrs,
                                uint8_t max_addrs, uint8_t *count)
{
    char     want[DNS_NAME_MAX];
    char     owner[DNS_NAME_MAX];
    char     target[DNS_NAME_MAX];
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t off;
    uint16_t i;
    uint8_t  found = 0u;

    if (pkt == NULL || addrs == NULL || count == NULL || max_addrs == 0u ||
        max_addrs > W5500_DNS_MAX_ADDRS || len < DNS_HEADER_LEN) {
        return -1;
    }
    *count = 0u;
    if ((((uint16_t)pkt[0] << 8) | pkt[1]) != qid) {
        return -1;
    }
    flags = (uint16_t)(((uint16_t)pkt[2] << 8) | pkt[3]);
    if ((flags & 0x8000u) == 0u || (flags & 0x000Fu) != 0u) {
        return -1;
    }
    qdcount = (uint16_t)(((uint16_t)pkt[4] << 8) | pkt[5]);
    ancount = (uint16_t)(((uint16_t)pkt[6] << 8) | pkt[7]);

    if (wanted != NULL && strlen(wanted) >= sizeof(want)) {
        return -1;
    }
    if (wanted != NULL) {
        strcpy(want, wanted);
    }

    off = DNS_HEADER_LEN;
    for (i = 0u; i < qdcount; i++) {
        if (dns_skip_name(pkt, len, &off) != 0 ||
            (uint32_t)off + 4u > (uint32_t)len) {
            return -1;
        }
        off = (uint16_t)(off + 4u);
    }

    for (i = 0u; i < ancount; i++) {
        uint16_t rtype;
        uint16_t cls;
        uint32_t ttl;
        uint16_t rdlen;
        uint16_t rdata;
        uint16_t end;

        if (dns_decode_name(pkt, len, off, owner, sizeof(owner), &end) != 0) {
            return -1;
        }
        off = end;
        if ((uint32_t)off + 10u > (uint32_t)len) {
            return -1;
        }
        rtype = (uint16_t)(((uint16_t)pkt[off] << 8) | pkt[off + 1u]);
        cls = (uint16_t)(((uint16_t)pkt[off + 2u] << 8) | pkt[off + 3u]);
        ttl = ((uint32_t)pkt[off + 4u] << 24) |
              ((uint32_t)pkt[off + 5u] << 16) |
              ((uint32_t)pkt[off + 6u] << 8) |
              (uint32_t)pkt[off + 7u];
        rdlen = (uint16_t)(((uint16_t)pkt[off + 8u] << 8) | pkt[off + 9u]);
        rdata = (uint16_t)(off + 10u);
        if ((uint32_t)rdata + (uint32_t)rdlen > (uint32_t)len) {
            return -1;
        }
        off = (uint16_t)(rdata + rdlen);

        if (cls != 1u) {
            continue;
        }
        if (rtype == 5u) { /* CNAME */
            if (wanted == NULL || dns_name_eq(owner, want)) {
                if (dns_decode_name(pkt, len, rdata, target, sizeof(target),
                                    &end) != 0 ||
                    strlen(target) >= sizeof(want)) {
                    return -1;
                }
                strcpy(want, target);
            }
        } else if (rtype == 1u && rdlen == 4u) { /* A */
            if (wanted == NULL || dns_name_eq(owner, want)) {
                if (found < max_addrs) {
                    memcpy(addrs[found].ip, pkt + rdata, 4u);
                    addrs[found].ttl_s = ttl;
                    found++;
                }
            }
        }
    }
    *count = found;
    return 0;
}

int w5500_dns_parse_response(const uint8_t *pkt, uint16_t len, uint16_t qid,
                             uint8_t ip_out[4])
{
    w5500_dns_addr_t addr;
    uint8_t count = 0u;

    if (ip_out == NULL) {
        return -1;
    }
    if (w5500_dns_parse_response_ex(pkt, len, qid, NULL, &addr, 1u,
                                    &count) != 0 || count == 0u) {
        return -1;
    }
    memcpy(ip_out, addr.ip, 4u);
    return 0;
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

static void dns_finish(struct w5500_dns *d, const w5500_dns_addr_t *addrs,
                       uint8_t count, w5500_result_t status)
{
    static const uint8_t zero[4] = {0u, 0u, 0u, 0u};

    d->state = DNS_STATE_OFF;
    d->from_cache = 0u;
    if (d->multi_cb != NULL) {
        d->multi_cb(d, (count > 0u) ? addrs : NULL, count, status, d->user);
    } else if (d->cb != NULL) {
        d->cb(d, (count > 0u) ? addrs[0].ip : zero, status, d->user);
    }
}

static void dns_cache_clear(struct w5500_dns *d)
{
    memset(d->cache, 0, sizeof(d->cache));
    d->cache_slot = 0u;
}

static uint32_t dns_expire(uint32_t now, uint32_t ttl_s)
{
    uint64_t end = (uint64_t)now + (uint64_t)ttl_s * 1000u;

    if (end > 0x7FFFFFFFu) {
        return 0x7FFFFFFFu;
    }
    return (uint32_t)end;
}

static void dns_cache_put(struct w5500_dns *d, const w5500_dns_addr_t *addr,
                          uint32_t now)
{
    dns_cache_entry_t *e;
    size_t n = strlen(d->name);

    if (n == 0u || n >= sizeof(e->name) || addr->ttl_s == 0u) {
        return;
    }
    e = &d->cache[d->cache_slot];
    d->cache_slot = (uint8_t)((d->cache_slot + 1u) % DNS_CACHE_SLOTS);
    memset(e, 0, sizeof(*e));
    strcpy(e->name, d->name);
    memcpy(e->ip, addr->ip, 4u);
    e->expire = dns_expire(now, addr->ttl_s);
    e->used = 1u;
}

static int dns_cache_get(struct w5500_dns *d, uint32_t now,
                         w5500_dns_addr_t *out)
{
    uint8_t i;

    if (!d->cache_on) {
        return 0;
    }
    for (i = 0u; i < DNS_CACHE_SLOTS; i++) {
        dns_cache_entry_t *e = &d->cache[i];
        if (!e->used || !dns_name_eq(e->name, d->name)) {
            continue;
        }
        if ((int32_t)(e->expire - now) <= 0) {
            e->used = 0u;
            continue;
        }
        memcpy(out->ip, e->ip, 4u);
        out->ttl_s = (uint32_t)((e->expire - now) / 1000u);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle / state machine                                           */
/* ------------------------------------------------------------------ */

size_t w5500_dns_size(void)
{
    return sizeof(struct w5500_dns);
}

static w5500_result_t dns_init_common(w5500_dns_t **out_dns,
                                      void *mem, size_t mem_size,
                                      w5500_handle_t chip,
                                      const uint8_t server[4],
                                      void *user_arg)
{
    struct w5500_dns *d;

    if (out_dns == NULL || mem == NULL || chip == NULL || server == NULL) {
        return W5500_ERR_PARAM;
    }
    if (mem_size < sizeof(*d)) {
        return W5500_ERR_NOMEM;
    }
    d = (struct w5500_dns *)mem;
    memset(d, 0, sizeof(*d));
    d->chip = chip;
    memcpy(d->server, server, 4u);
    d->user = user_arg;
    d->state = DNS_STATE_OFF;
    *out_dns = d;
    return W5500_OK;
}

w5500_result_t w5500_dns_init(w5500_dns_t **out_dns,
                              void *mem,
                              size_t mem_size,
                              w5500_handle_t chip,
                              const uint8_t server[4],
                              w5500_dns_cb_t cb,
                              void *user_arg)
{
    struct w5500_dns *d = NULL;
    w5500_result_t rc;

    if (cb == NULL) {
        return W5500_ERR_PARAM;
    }
    rc = dns_init_common(&d, mem, mem_size, chip, server, user_arg);
    if (rc != W5500_OK) {
        return rc;
    }
    d->cb = cb;
    *out_dns = d;
    return W5500_OK;
}

w5500_result_t w5500_dns_init_multi(w5500_dns_t **out_dns,
                                    void *mem,
                                    size_t mem_size,
                                    w5500_handle_t chip,
                                    const uint8_t server[4],
                                    w5500_dns_multi_cb_t cb,
                                    void *user_arg)
{
    struct w5500_dns *d = NULL;
    w5500_result_t rc;

    if (cb == NULL) {
        return W5500_ERR_PARAM;
    }
    rc = dns_init_common(&d, mem, mem_size, chip, server, user_arg);
    if (rc != W5500_OK) {
        return rc;
    }
    d->multi_cb = cb;
    *out_dns = d;
    return W5500_OK;
}

void w5500_dns_set_cache(w5500_dns_t *dns, uint8_t enable)
{
    if (dns == NULL) {
        return;
    }
    dns->cache_on = (enable != 0u) ? 1u : 0u;
    if (!dns->cache_on) {
        dns_cache_clear(dns);
    }
}

void w5500_dns_cache_clear(w5500_dns_t *dns)
{
    if (dns != NULL) {
        dns_cache_clear(dns);
    }
}

w5500_result_t w5500_dns_set_server(w5500_dns_t *dns, const uint8_t server[4])
{
    if (dns == NULL || server == NULL) {
        return W5500_ERR_PARAM;
    }
    memcpy(dns->server, server, 4u);
    dns_cache_clear(dns);
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

    memset(dns->name, 0, sizeof(dns->name));
    strncpy(dns->name, name, sizeof(dns->name) - 1u);

    if (dns->cache_on) {
        w5500_dns_addr_t cached;

        if (dns_cache_get(dns, now_ms, &cached)) {
            dns->cache_hit = cached;
            dns->from_cache = 1u;
            dns->state = DNS_STATE_WAITING;
            return W5500_OK;
        }
    }

    if (dns->sock == NULL) {
        rc = w5500_socket_open(dns->chip, W5500_PROTO_UDP, DNS_LOCAL_PORT,
                               dns_noop_cb, dns, &dns->sock);
        if (rc != W5500_OK) {
            dns->sock = NULL;
            return rc;
        }
    }

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
    if (dns->from_cache) {
        dns_finish(dns, &dns->cache_hit, 1u, W5500_OK);
        return W5500_OK;
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

            if ((flags & 0x8000u) != 0u && (flags & 0x000Fu) != 0u) {
                dns_finish(dns, NULL, 0u, W5500_ERR_STATE); /* NXDOMAIN */
                return W5500_OK;
            }
            {
                w5500_dns_addr_t addrs[W5500_DNS_MAX_ADDRS];
                uint8_t count = 0u;

                if (w5500_dns_parse_response_ex(dns->rx, n, dns->qid,
                                                dns->name, addrs,
                                                (uint8_t)W5500_DNS_MAX_ADDRS,
                                                &count) == 0 && count > 0u) {
                    if (dns->cache_on) {
                        dns_cache_put(dns, &addrs[0], now_ms);
                    }
                    dns_finish(dns, addrs, count, W5500_OK);
                    return W5500_OK;
                }
            }
        }
        packets++;
    }

    if ((int32_t)(now_ms - dns->t_last) >= (int32_t)DNS_RETRY_MS) {
        uint16_t sent = 0u;
        uint16_t len;
        w5500_result_t rc;

        if (dns->attempts >= DNS_MAX_ATTEMPTS) {
            dns_finish(dns, NULL, 0u, W5500_ERR_TIMEOUT);
            return W5500_OK;
        }
        len = dns_build_query(dns);
        rc = w5500_socket_sendto(dns->sock, dns->server, DNS_SERVER_PORT,
                                 dns->tx, len, &sent);
        if (rc != W5500_OK && rc != W5500_ERR_BUSY) {
            dns_finish(dns, NULL, 0u, rc);
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
    dns->from_cache = 0u;
}
