/**
 * @file w5500_sockets.c
 * @brief Event-driven non-blocking socket layer on top of the W5500 core.
 *
 * Hardware sockets are allocated from a fixed array inside the driver
 * context (protected by the platform lock) — no heap, no globals.
 */
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_internal.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static w5500_result_t w5500_sock_valid(w5500_socket_t sock)
{
    if (sock == NULL || sock->owner == NULL || sock->in_use == 0u) {
        return W5500_ERR_PARAM;
    }
    return W5500_OK;
}

static w5500_result_t w5500_sock_read_sr(w5500_socket_t sock, uint8_t *sr)
{
    return w5500_read8(sock->owner, W5500_Sn_SR, W5500_BSB_SREG(sock->id), sr);
}

/* Pending bytes in the hardware RX buffer (caller holds the lock). */
static w5500_result_t w5500_sock_rx_pending(w5500_socket_t sock, uint16_t *n)
{
    return w5500_read16_stable(sock->owner, W5500_Sn_RX_RSR,
                               W5500_BSB_SREG(sock->id), n);
}

static void w5500_sock_release(w5500_socket_t sock)
{
    sock->in_use = 0u;
    sock->state = W5500_SSR_FREE;
    sock->proto = W5500_Sn_MR_CLOSE;
    sock->send_pending = 0u;
    sock->peer_closed = 0u;
    sock->udp_remaining = 0u;
    sock->cb = NULL;
    sock->user_arg = NULL;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

w5500_result_t w5500_socket_open_ex(w5500_handle_t handle,
                                    w5500_proto_t proto,
                                    uint16_t local_port,
                                    uint32_t flags,
                                    w5500_event_cb_t cb,
                                    void *user_arg,
                                    w5500_socket_t *out_sock)
{
    struct w5500_socket *slot = NULL;
    uint8_t i;
    uint8_t expect_sr;
    uint8_t mr;
    w5500_result_t rc;

    if (handle == NULL || cb == NULL || out_sock == NULL) {
        return W5500_ERR_PARAM;
    }
    if (proto != W5500_PROTO_TCP && proto != W5500_PROTO_UDP) {
        return W5500_ERR_PARAM;
    }
    if ((flags & ~(uint32_t)(W5500_SOCK_F_NO_DELAY | W5500_SOCK_F_MULTI |
                             W5500_SOCK_F_BROADCAST_BLOCK |
                             W5500_SOCK_F_IGMP_V2)) != 0u) {
        return W5500_ERR_PARAM; /* unknown flag bit */
    }
    if (proto == W5500_PROTO_TCP &&
        (flags & (W5500_SOCK_F_MULTI | W5500_SOCK_F_BROADCAST_BLOCK |
                  W5500_SOCK_F_IGMP_V2)) != 0u) {
        return W5500_ERR_PARAM; /* Sn_MR.MULTI/BCASTB/MC are UDP-only */
    }
    if (proto == W5500_PROTO_UDP && (flags & W5500_SOCK_F_NO_DELAY) != 0u) {
        return W5500_ERR_PARAM; /* Sn_MR.ND is TCP-only */
    }
    if ((flags & W5500_SOCK_F_IGMP_V2) != 0u &&
        (flags & W5500_SOCK_F_MULTI) == 0u) {
        return W5500_ERR_PARAM; /* IGMPv2 only makes sense for multicast */
    }

    mr = (proto == W5500_PROTO_TCP) ? W5500_Sn_MR_TCP : W5500_Sn_MR_UDP;
    if ((flags & W5500_SOCK_F_NO_DELAY) != 0u) {
        mr = (uint8_t)(mr | W5500_Sn_MR_ND);
    }
    if ((flags & W5500_SOCK_F_MULTI) != 0u) {
        mr = (uint8_t)(mr | W5500_Sn_MR_MULTI);
    }
    if ((flags & W5500_SOCK_F_BROADCAST_BLOCK) != 0u) {
        mr = (uint8_t)(mr | W5500_Sn_MR_BCASTB);
    }
    if ((flags & W5500_SOCK_F_IGMP_V2) != 0u) {
        mr = (uint8_t)(mr | W5500_Sn_MR_MC);
    }
    expect_sr = (proto == W5500_PROTO_TCP) ? W5500_SOCK_INIT : W5500_SOCK_UDP;

    w5500_lock(handle);

    for (i = 0; i < W5500_SOCKET_COUNT; i++) {
        if (handle->sockets[i].in_use == 0u) {
            slot = &handle->sockets[i];
            break;
        }
    }
    if (slot == NULL) {
        w5500_unlock(handle);
        return W5500_ERR_NOMEM;
    }

    /* Claim the slot before touching the chip. */
    slot->in_use = 1u;
    slot->owner = handle;
    slot->proto = (proto == W5500_PROTO_TCP) ? W5500_Sn_MR_TCP
                                             : W5500_Sn_MR_UDP;
    slot->send_pending = 0u;
    slot->state = W5500_SSR_INITIATED;
    slot->cb = cb;
    slot->user_arg = user_arg;
    slot->peer_closed = 0u;
    slot->udp_remaining = 0u;

    rc = w5500_write8(handle, W5500_Sn_MR, W5500_BSB_SREG(slot->id), mr);
    if (rc == W5500_OK) {
        rc = w5500_write16(handle, W5500_Sn_PORT, W5500_BSB_SREG(slot->id),
                           local_port);
    }
    if (rc == W5500_OK) {
        rc = w5500_write8(handle, W5500_Sn_TTL, W5500_BSB_SREG(slot->id),
                          128u);
    }
    if (rc == W5500_OK) {
        rc = w5500_write8(handle, W5500_Sn_KPALVTR, W5500_BSB_SREG(slot->id),
                          0u);
    }
    if (rc == W5500_OK) {
        rc = w5500_sn_exec_cmd(handle, slot->id, W5500_Sn_CR_OPEN);
    }
    if (rc == W5500_OK) {
        rc = w5500_wait_reg_eq(handle, W5500_Sn_SR, W5500_BSB_SREG(slot->id),
                               expect_sr);
    }

    if (rc != W5500_OK) {
        /* Best effort close, then give the slot back. */
        (void)w5500_sn_exec_cmd(handle, slot->id, W5500_Sn_CR_CLOSE);
        w5500_sock_release(slot);
        w5500_unlock(handle);
        return rc;
    }

    *out_sock = slot;
    if (handle->stats != NULL) {
        handle->stats->socket_opens++;
    }
    w5500_unlock(handle);
    return W5500_OK;
}

w5500_result_t w5500_socket_open(w5500_handle_t handle,
                                 w5500_proto_t proto,
                                 uint16_t local_port,
                                 w5500_event_cb_t cb,
                                 void *user_arg,
                                 w5500_socket_t *out_sock)
{
    return w5500_socket_open_ex(handle, proto, local_port, 0u, cb, user_arg,
                                out_sock);
}

w5500_result_t w5500_socket_close(w5500_socket_t sock)
{
    w5500_handle_t h;
    w5500_result_t rc;

    rc = w5500_sock_valid(sock);
    if (rc != W5500_OK) {
        return rc;
    }
    h = sock->owner;
    w5500_lock(h);
    rc = w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CLOSE);
    if (rc == W5500_OK) {
        rc = w5500_wait_reg_eq(h, W5500_Sn_SR, W5500_BSB_SREG(sock->id),
                               W5500_SOCK_CLOSED);
    }
    if (rc == W5500_OK) {
        w5500_sock_release(sock);
        if (h->stats != NULL) {
            h->stats->socket_closes++;
        }
    }
    w5500_unlock(h);
    return rc;
}

w5500_result_t w5500_socket_disconnect(w5500_socket_t sock)
{
    w5500_handle_t h;
    uint8_t sr = 0u;
    w5500_result_t rc;

    rc = w5500_sock_valid(sock);
    if (rc != W5500_OK) {
        return rc;
    }
    if (sock->proto != W5500_Sn_MR_TCP) {
        return W5500_ERR_STATE;
    }

    h = sock->owner;
    w5500_lock(h);
    rc = w5500_sock_read_sr(sock, &sr);
    if (rc == W5500_OK &&
        (sr == W5500_SOCK_ESTABLISHED || sr == W5500_SOCK_CLOSE_WAIT)) {
        rc = w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_DISCON);
        if (rc == W5500_OK) {
            sock->state = W5500_SSR_CLOSING;
        }
    } else {
        rc = W5500_ERR_STATE;
    }
    w5500_unlock(h);
    return rc;
}

/* ------------------------------------------------------------------ */
/* TCP client / server                                                 */
/* ------------------------------------------------------------------ */

w5500_result_t w5500_socket_listen(w5500_socket_t sock)
{
    w5500_handle_t h;
    uint8_t sr = 0u;
    w5500_result_t rc;

    rc = w5500_sock_valid(sock);
    if (rc != W5500_OK) {
        return rc;
    }
    if (sock->proto != W5500_Sn_MR_TCP) {
        return W5500_ERR_STATE;
    }

    h = sock->owner;
    w5500_lock(h);
    rc = w5500_sock_read_sr(sock, &sr);
    if (rc == W5500_OK && sr == W5500_SOCK_INIT) {
        rc = w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_LISTEN);
        if (rc == W5500_OK) {
            /* Success: SR == LISTEN. Failure (e.g. port taken): SR == CLOSED. */
            rc = w5500_wait_reg_eq(h, W5500_Sn_SR, W5500_BSB_SREG(sock->id),
                                   W5500_SOCK_LISTEN);
            if (rc == W5500_OK) {
                sock->state = W5500_SSR_LISTENING;
            }
        }
    } else if (rc == W5500_OK) {
        rc = W5500_ERR_STATE;
    }
    w5500_unlock(h);
    return rc;
}

w5500_result_t w5500_socket_connect(w5500_socket_t sock,
                                    const uint8_t ip[4],
                                    uint16_t port)
{
    w5500_handle_t h;
    uint8_t sr = 0u;
    w5500_result_t rc;

    rc = w5500_sock_valid(sock);
    if (rc != W5500_OK || ip == NULL) {
        return (rc != W5500_OK) ? rc : W5500_ERR_PARAM;
    }
    if (sock->proto != W5500_Sn_MR_TCP) {
        return W5500_ERR_STATE;
    }

    h = sock->owner;
    w5500_lock(h);
    rc = w5500_sock_read_sr(sock, &sr);
    if (rc == W5500_OK && sr == W5500_SOCK_INIT) {
        rc = w5500_ll_write(h, W5500_Sn_DIPR, W5500_BSB_SREG(sock->id),
                            ip, 4u);
        if (rc == W5500_OK) {
            rc = w5500_write16(h, W5500_Sn_DPORT, W5500_BSB_SREG(sock->id),
                               port);
        }
        if (rc == W5500_OK) {
            rc = w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CONNECT);
        }
        if (rc == W5500_OK) {
            /* Non-blocking: CONNECTED/ERROR events finish the handshake. */
            sock->state = W5500_SSR_CONNECTING;
        }
    } else if (rc == W5500_OK) {
        rc = W5500_ERR_STATE;
    }
    w5500_unlock(h);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Data path                                                           */
/* ------------------------------------------------------------------ */

static w5500_result_t w5500_sock_tx_common(w5500_socket_t sock,
                                           const uint8_t *data,
                                           uint16_t len,
                                           uint16_t *sent)
{
    w5500_handle_t h = sock->owner;
    uint16_t free_sz = 0u;
    uint16_t wr = 0u;
    uint16_t chunk;
    w5500_result_t rc;

    if (data == NULL || sent == NULL || len == 0u) {
        return W5500_ERR_PARAM;
    }
    *sent = 0u;

    rc = w5500_read16_stable(h, W5500_Sn_TX_FSR, W5500_BSB_SREG(sock->id),
                             &free_sz);
    if (rc != W5500_OK) {
        return rc;
    }
    if (free_sz == 0u) {
        return W5500_OK; /* TX buffer full: non-blocking, nothing accepted */
    }

    chunk = (len < free_sz) ? len : free_sz;

    rc = w5500_read16(h, W5500_Sn_TX_WR, W5500_BSB_SREG(sock->id), &wr);
    if (rc != W5500_OK) {
        return rc;
    }
    /* Zero-copy: user buffer goes straight to the chip TX memory. */
    rc = w5500_ll_write(h, wr, W5500_BSB_TXBUF(sock->id), data, chunk);
    if (rc != W5500_OK) {
        return rc;
    }
    rc = w5500_write16(h, W5500_Sn_TX_WR, W5500_BSB_SREG(sock->id),
                       (uint16_t)(wr + chunk));
    if (rc != W5500_OK) {
        return rc;
    }
    rc = w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_SEND);
    if (rc == W5500_OK) {
        sock->send_pending = 1u; /* one SEND in flight per socket */
        *sent = chunk;
        if (h->stats != NULL) {
            h->stats->bytes_sent += chunk;
            if (sock->proto == W5500_Sn_MR_UDP) {
                h->stats->datagrams_sent++;
            }
        }
    }
    return rc;
}

w5500_result_t w5500_socket_send(w5500_socket_t sock,
                                 const uint8_t *data,
                                 uint16_t len,
                                 uint16_t *sent)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK) {
        return rc;
    }
    if (sock->proto != W5500_Sn_MR_TCP || sock->state != W5500_SSR_ESTABLISHED) {
        return W5500_ERR_STATE;
    }
    /* Argument errors take precedence over the in-flight guard. */
    if (data == NULL || sent == NULL || len == 0u) {
        return W5500_ERR_PARAM;
    }

    w5500_lock(sock->owner);
    if (sock->send_pending != 0u) {
        w5500_unlock(sock->owner);
        if (sent != NULL) {
            *sent = 0u;
        }
        return W5500_ERR_BUSY; /* previous SEND still in flight */
    }
    rc = w5500_sock_tx_common(sock, data, len, sent);
    w5500_unlock(sock->owner);
    return rc;
}

w5500_result_t w5500_socket_sendto(w5500_socket_t sock,
                                   const uint8_t ip[4],
                                   uint16_t port,
                                   const uint8_t *data,
                                   uint16_t len,
                                   uint16_t *sent)
{
    w5500_handle_t h;
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK || ip == NULL) {
        return (rc != W5500_OK) ? rc : W5500_ERR_PARAM;
    }
    if (sock->proto != W5500_Sn_MR_UDP) {
        return W5500_ERR_STATE;
    }
    /* Argument errors take precedence over the in-flight guard. */
    if (data == NULL || sent == NULL || len == 0u) {
        return W5500_ERR_PARAM;
    }

    h = sock->owner;
    w5500_lock(h);
    if (sock->send_pending != 0u) {
        w5500_unlock(h);
        if (sent != NULL) {
            *sent = 0u;
        }
        return W5500_ERR_BUSY; /* previous SEND still in flight */
    }
    rc = w5500_ll_write(h, W5500_Sn_DIPR, W5500_BSB_SREG(sock->id), ip, 4u);
    if (rc == W5500_OK) {
        rc = w5500_write16(h, W5500_Sn_DPORT, W5500_BSB_SREG(sock->id), port);
    }
    if (rc == W5500_OK) {
        rc = w5500_sock_tx_common(sock, data, len, sent);
    }
    w5500_unlock(h);
    return rc;
}

w5500_result_t w5500_socket_recvfrom(w5500_socket_t sock,
                                     uint8_t *data,
                                     uint16_t len,
                                     uint16_t *recvd,
                                     uint8_t ip[4],
                                     uint16_t *port)
{
    w5500_handle_t h;
    uint16_t avail = 0u;
    uint16_t rd = 0u;
    uint16_t chunk;
    uint16_t payload = 0u;
    uint8_t hdr[8];
    int is_udp = (sock->proto == W5500_Sn_MR_UDP);
    w5500_result_t rc = w5500_sock_valid(sock);

    if (rc != W5500_OK) {
        return rc;
    }
    if (data == NULL || recvd == NULL || len == 0u) {
        return W5500_ERR_PARAM;
    }
    *recvd = 0u;

    h = sock->owner;
    w5500_lock(h);

    rc = w5500_read16_stable(h, W5500_Sn_RX_RSR, W5500_BSB_SREG(sock->id),
                             &avail);
    if (rc != W5500_OK) {
        w5500_unlock(h);
        return rc;
    }
    if (avail == 0u) {
        w5500_unlock(h);
        return W5500_OK;
    }

    rc = w5500_read16(h, W5500_Sn_RX_RD, W5500_BSB_SREG(sock->id), &rd);
    if (rc != W5500_OK) {
        w5500_unlock(h);
        return rc;
    }

    /* UDP datagrams carry an 8-byte header in RX memory: source IP (4),
       source port (2), datagram length (2). It is counted in Sn_RX_RSR and
       must be consumed together with the payload. */
    if (is_udp && sock->udp_remaining == 0u) {
        if (avail < 8u) {
            w5500_unlock(h);
            return W5500_ERR_STATE;
        }
        rc = w5500_ll_read(h, rd, W5500_BSB_RXBUF(sock->id), hdr, 8u);
        if (rc != W5500_OK) {
            w5500_unlock(h);
            return rc;
        }
        payload = (uint16_t)(((uint16_t)hdr[6] << 8) | hdr[7]);
        if (payload > (uint16_t)(avail - 8u)) {
            w5500_unlock(h);
            return W5500_ERR_STATE;
        }
        if (ip != NULL) {
            memcpy(ip, hdr, 4u);
        }
        if (port != NULL) {
            *port = (uint16_t)(((uint16_t)hdr[4] << 8) | hdr[5]);
        }
        rd = (uint16_t)(rd + 8u); /* skip header in chip RX memory */
        avail = payload;          /* only this datagram's payload is ours */
        if (payload == 0u) {
            /* Empty datagram: consume only the 8-byte header and finish. */
            rc = w5500_write16(h, W5500_Sn_RX_RD, W5500_BSB_SREG(sock->id),
                               (uint16_t)(rd));
            if (rc == W5500_OK) {
                rc = w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_RECV);
            }
            w5500_unlock(h);
            return rc;
        }
    }
    if (is_udp && sock->udp_remaining != 0u) {
        /* A previous call read part of this datagram: its header was already
           consumed, so the remaining bytes start at Sn_RX_RD. */
        if (sock->udp_remaining > avail) {
            w5500_unlock(h);
            return W5500_ERR_STATE;
        }
        avail = sock->udp_remaining;
    } else if (ip != NULL || port != NULL) {
        /* TCP peer is latched in Sn_DIPR/Sn_DPORT before data is consumed. */
        if (ip != NULL) {
            rc = w5500_ll_read(h, W5500_Sn_DIPR, W5500_BSB_SREG(sock->id), ip,
                               4u);
            if (rc != W5500_OK) {
                w5500_unlock(h);
                return rc;
            }
        }
        if (port != NULL) {
            rc = w5500_read16(h, W5500_Sn_DPORT, W5500_BSB_SREG(sock->id),
                              port);
            if (rc != W5500_OK) {
                w5500_unlock(h);
                return rc;
            }
        }
    }

    chunk = (len < avail) ? len : avail;
    /* Zero-copy: chip RX memory is streamed straight into the user buffer. */
    rc = w5500_ll_read(h, rd, W5500_BSB_RXBUF(sock->id), data, chunk);
    if (rc != W5500_OK) {
        w5500_unlock(h);
        return rc;
    }
    rc = w5500_write16(h, W5500_Sn_RX_RD, W5500_BSB_SREG(sock->id),
                       (uint16_t)(rd + chunk));
    if (rc == W5500_OK && is_udp) {
        /* Keep track of a partially consumed datagram; do not issue RECV
           until the whole payload has been read. */
        sock->udp_remaining = (uint16_t)(avail - chunk);
        if (sock->udp_remaining == 0u) {
            rc = w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_RECV);
        }
    }
    if (rc == W5500_OK) {
        *recvd = chunk;
        if (h->stats != NULL) {
            h->stats->bytes_received += chunk;
            if (is_udp && sock->udp_remaining == 0u) {
                h->stats->datagrams_received++;
            }
        }
    }
    w5500_unlock(h);
    return rc;
}

w5500_result_t w5500_socket_recv(w5500_socket_t sock,
                                 uint8_t *data,
                                 uint16_t len,
                                 uint16_t *recvd)
{
    return w5500_socket_recvfrom(sock, data, len, recvd, NULL, NULL);
}

w5500_result_t w5500_socket_rx_available(w5500_socket_t sock, uint16_t *avail)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK || avail == NULL) {
        return (rc != W5500_OK) ? rc : W5500_ERR_PARAM;
    }
    w5500_lock(sock->owner);
    rc = w5500_read16_stable(sock->owner, W5500_Sn_RX_RSR,
                             W5500_BSB_SREG(sock->id), avail);
    w5500_unlock(sock->owner);
    return rc;
}

w5500_result_t w5500_socket_tx_free(w5500_socket_t sock, uint16_t *free_bytes)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK || free_bytes == NULL) {
        return (rc != W5500_OK) ? rc : W5500_ERR_PARAM;
    }
    w5500_lock(sock->owner);
    rc = w5500_read16_stable(sock->owner, W5500_Sn_TX_FSR,
                             W5500_BSB_SREG(sock->id), free_bytes);
    w5500_unlock(sock->owner);
    return rc;
}

w5500_result_t w5500_socket_get_peer(w5500_socket_t sock,
                                     uint8_t ip[4],
                                     uint16_t *port)
{
    w5500_handle_t h;
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK || (ip == NULL && port == NULL)) {
        return (rc != W5500_OK) ? rc : W5500_ERR_PARAM;
    }

    h = sock->owner;
    w5500_lock(h);
    if (ip != NULL) {
        rc = w5500_ll_read(h, W5500_Sn_DIPR, W5500_BSB_SREG(sock->id), ip, 4u);
    }
    if (rc == W5500_OK && port != NULL) {
        rc = w5500_read16(h, W5500_Sn_DPORT, W5500_BSB_SREG(sock->id), port);
    }
    w5500_unlock(h);
    return rc;
}

w5500_result_t w5500_socket_set_keepalive(w5500_socket_t sock,
                                          uint16_t interval_5s)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK) {
        return rc;
    }
    w5500_lock(sock->owner);
    rc = w5500_write16(sock->owner, W5500_Sn_KPALVTR,
                       W5500_BSB_SREG(sock->id), interval_5s);
    w5500_unlock(sock->owner);
    return rc;
}

w5500_result_t w5500_socket_set_ttl(w5500_socket_t sock, uint8_t ttl)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK) {
        return rc;
    }
    w5500_lock(sock->owner);
    rc = w5500_write8(sock->owner, W5500_Sn_TTL, W5500_BSB_SREG(sock->id), ttl);
    w5500_unlock(sock->owner);
    return rc;
}

w5500_result_t w5500_socket_set_tos(w5500_socket_t sock, uint8_t tos)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK) {
        return rc;
    }
    w5500_lock(sock->owner);
    rc = w5500_write8(sock->owner, W5500_Sn_TOS, W5500_BSB_SREG(sock->id), tos);
    w5500_unlock(sock->owner);
    return rc;
}

w5500_result_t w5500_socket_get_mss(w5500_socket_t sock, uint16_t *mss)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK || mss == NULL) {
        return (rc != W5500_OK) ? rc : W5500_ERR_PARAM;
    }
    w5500_lock(sock->owner);
    rc = w5500_read16(sock->owner, W5500_Sn_MSSR, W5500_BSB_SREG(sock->id),
                      mss);
    w5500_unlock(sock->owner);
    return rc;
}

w5500_result_t w5500_socket_set_mss(w5500_socket_t sock, uint16_t mss)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK) {
        return rc;
    }
    w5500_lock(sock->owner);
    rc = w5500_write16(sock->owner, W5500_Sn_MSSR, W5500_BSB_SREG(sock->id),
                       mss);
    w5500_unlock(sock->owner);
    return rc;
}

w5500_result_t w5500_socket_get_status(w5500_socket_t sock, uint8_t *status)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK || status == NULL) {
        return (rc != W5500_OK) ? rc : W5500_ERR_PARAM;
    }
    w5500_lock(sock->owner);
    rc = w5500_sock_read_sr(sock, status);
    w5500_unlock(sock->owner);
    return rc;
}

w5500_result_t w5500_socket_keepalive_probe(w5500_socket_t sock)
{
    w5500_result_t rc = w5500_sock_valid(sock);
    if (rc != W5500_OK) {
        return rc;
    }
    if (sock->proto != W5500_Sn_MR_TCP ||
        sock->state != W5500_SSR_ESTABLISHED) {
        return W5500_ERR_STATE;
    }
    w5500_lock(sock->owner);
    rc = w5500_sn_exec_cmd(sock->owner, sock->id, W5500_Sn_CR_SEND_KEEP);
    w5500_unlock(sock->owner);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Polling / event dispatch                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    w5500_socket_t    sock;
    w5500_event_t     event;
    w5500_result_t    status;
    w5500_event_cb_t  cb;      /**< captured before the slot may be freed */
    void             *user_arg;
} w5500_evt_t;

#define W5500_POLL_MAX_EVENTS (W5500_SOCKET_COUNT * 4u)

static void w5500_poll_push(w5500_evt_t *events, uint8_t *count,
                            w5500_socket_t sock, w5500_event_t event,
                            w5500_result_t status)
{
    if (*count >= W5500_POLL_MAX_EVENTS) {
        return;
    }
    events[*count].sock = sock;
    events[*count].event = event;
    events[*count].status = status;
    events[*count].cb = sock->cb;
    events[*count].user_arg = sock->user_arg;
    (*count)++;
}

static void w5500_poll_one(w5500_handle_t h, w5500_socket_t sock, uint8_t sir,
                           w5500_evt_t *events, uint8_t *count)
{
    uint8_t ir = 0u;
    uint8_t sr = 0u;

    /* SIR is the OR of all Sn_IR registers: skip the per-socket read unless
       this socket has a pending summary bit. */
    if ((sir & (uint8_t)(1u << sock->id)) != 0u) {
        if (w5500_read8(h, W5500_Sn_IR, W5500_BSB_SREG(sock->id), &ir) !=
            W5500_OK) {
            return;
        }
    }

    if (ir == 0u) {
        uint16_t pending = 0u;

        /* Half-closed by the peer: keep the slot until the app drains RX. */
        if (sock->peer_closed) {
            if (w5500_sock_rx_pending(sock, &pending) == W5500_OK &&
                pending == 0u) {
                (void)w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CLOSE);
                w5500_poll_push(events, count, sock, W5500_EVENT_DISCONNECTED,
                                W5500_OK);
                w5500_sock_release(sock);
            }
            return;
        }

        /* Passive close is a TCP concept; UDP sockets need no SR read. */
        if (sock->proto == W5500_Sn_MR_TCP) {
            (void)w5500_sock_read_sr(sock, &sr);
            if (sock->state == W5500_SSR_CLOSING &&
                sr == W5500_SOCK_CLOSED) {
                (void)w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CLOSE);
                w5500_poll_push(events, count, sock,
                                W5500_EVENT_DISCONNECTED, W5500_OK);
                w5500_sock_release(sock);
                return;
            }
            if (sock->state != W5500_SSR_CLOSING &&
                sr == W5500_SOCK_CLOSED) {
                /* Connection dropped without an IR flag. */
                w5500_poll_push(events, count, sock,
                                W5500_EVENT_DISCONNECTED, W5500_OK);
                w5500_sock_release(sock);
                return;
            }
        }

        /* Recover a SEND whose SEND_OK interrupt was lost: the chip only
           advances TX_RD past TX_WR once the frame left the buffer. */
        if (sock->send_pending != 0u) {
            uint16_t rd = 0u;
            uint16_t wr = 0u;

            if (w5500_read16(h, W5500_Sn_TX_RD, W5500_BSB_SREG(sock->id),
                             &rd) == W5500_OK &&
                w5500_read16(h, W5500_Sn_TX_WR, W5500_BSB_SREG(sock->id),
                             &wr) == W5500_OK &&
                rd == wr) {
                sock->send_pending = 0u;
                w5500_poll_push(events, count, sock, W5500_EVENT_SEND_DONE,
                                W5500_OK);
            }
        }
        return;
    }

    /* Write-1-to-clear the bits we are about to process. */
    (void)w5500_write8(h, W5500_Sn_IR, W5500_BSB_SREG(sock->id), ir);

    /* A fatal timeout wins over everything else: no data events, no state
       transitions, the socket is closed and released immediately. */
    if ((ir & W5500_Sn_IR_TIMEOUT) != 0u) {
        (void)w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CLOSE);
        w5500_poll_push(events, count, sock, W5500_EVENT_ERROR,
                        W5500_ERR_TIMEOUT);
        w5500_sock_release(sock);
        return;
    }

    if ((ir & W5500_Sn_IR_CON) != 0u) {
        sock->state = W5500_SSR_ESTABLISHED;
        w5500_poll_push(events, count, sock, W5500_EVENT_CONNECTED, W5500_OK);
    }

    if ((ir & W5500_Sn_IR_RECV) != 0u) {
        w5500_poll_push(events, count, sock, W5500_EVENT_DATA_RECEIVED,
                        W5500_OK);
    }

    if ((ir & W5500_Sn_IR_SENDOK) != 0u) {
        sock->send_pending = 0u;
        w5500_poll_push(events, count, sock, W5500_EVENT_SEND_DONE, W5500_OK);
    }

    if ((ir & W5500_Sn_IR_DISCON) != 0u) {
        if (sock->proto == W5500_Sn_MR_TCP) {
            uint16_t pending = 0u;

            /* Half-close: keep the slot while RX still holds data so the
               application can drain it from the DATA_RECEIVED callback. */
            sock->peer_closed = 1u;
            if (w5500_sock_rx_pending(sock, &pending) == W5500_OK &&
                pending == 0u) {
                (void)w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CLOSE);
                w5500_poll_push(events, count, sock,
                                W5500_EVENT_DISCONNECTED, W5500_OK);
                w5500_sock_release(sock);
            } else if (pending != 0u &&
                       (ir & W5500_Sn_IR_RECV) == 0u) {
                w5500_poll_push(events, count, sock,
                                W5500_EVENT_DATA_RECEIVED, W5500_OK);
            }
            return;
        }
        (void)w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CLOSE);
        w5500_poll_push(events, count, sock, W5500_EVENT_DISCONNECTED,
                        W5500_OK);
        w5500_sock_release(sock);
    }
}

w5500_result_t w5500_irq_pending(w5500_handle_t handle,
                                 uint8_t *ir,
                                 uint8_t *sir)
{
    w5500_result_t rc;

    if (handle == NULL || ir == NULL || sir == NULL) {
        return W5500_ERR_PARAM;
    }
    w5500_lock(handle);
    rc = w5500_read8(handle, W5500_REG_IR, W5500_BSB_COMMON, ir);
    if (rc == W5500_OK) {
        rc = w5500_read8(handle, W5500_REG_SIR, W5500_BSB_COMMON, sir);
    }
    w5500_unlock(handle);
    return rc;
}

w5500_result_t w5500_poll(w5500_handle_t handle)
{
    w5500_evt_t events[W5500_POLL_MAX_EVENTS];
    w5500_link_info_t link;
    w5500_link_cb_t link_cb = NULL;
    void *link_user = NULL;
    uint8_t count = 0u;
    uint8_t i;
    uint8_t ir = 0u;
    uint8_t sir = 0u;
    uint8_t phy = 0u;
    uint8_t link_changed = 0u;
    w5500_result_t rc = W5500_OK;

    if (handle == NULL) {
        return W5500_ERR_PARAM;
    }

    w5500_lock(handle);
    if (w5500_read8(handle, W5500_REG_SIR, W5500_BSB_COMMON, &sir) !=
        W5500_OK) {
        sir = 0u;
    }
    if (w5500_read8(handle, W5500_REG_IR, W5500_BSB_COMMON, &ir) != W5500_OK) {
        ir = 0u;
    }
    for (i = 0; i < W5500_SOCKET_COUNT; i++) {
        if (handle->sockets[i].in_use != 0u) {
            w5500_poll_one(handle, &handle->sockets[i], sir, events, &count);
        }
    }

    /* Link status is sampled only when nothing else interrupted: a busy
       socket round must not cost an extra PHYCFGR read. */
    if (ir == 0u && sir == 0u &&
        w5500_read8(handle, W5500_REG_PHYCFGR, W5500_BSB_COMMON, &phy) ==
            W5500_OK) {
        phy = (uint8_t)(phy & (W5500_PHYCFGR_LNK_ON | W5500_PHYCFGR_SPD_100 |
                               W5500_PHYCFGR_DPX_FULL));
        if (handle->link_snapshot == 0xFFu) {
            handle->link_snapshot = phy; /* baseline only, no event */
        } else if (phy != handle->link_snapshot) {
            handle->link_snapshot = phy;
            if (handle->stats != NULL) {
                handle->stats->link_changes++;
            }
            link_changed = 1u;
            link.up = (uint8_t)((phy & W5500_PHYCFGR_LNK_ON) != 0u);
            link.speed_100 = (uint8_t)((phy & W5500_PHYCFGR_SPD_100) != 0u);
            link.full_duplex =
                (uint8_t)((phy & W5500_PHYCFGR_DPX_FULL) != 0u);
        }
    }
    link_cb = handle->link_cb;
    link_user = handle->link_user;
    w5500_unlock(handle);

    /* Callbacks run with the lock released — re-entering the driver is safe. */
    for (i = 0; i < count; i++) {
        if (events[i].cb != NULL) {
            events[i].cb(events[i].sock, events[i].event, events[i].status,
                         events[i].user_arg);
        }
    }
    if (link_changed != 0u && link_cb != NULL) {
        link_cb(handle, &link, link_user);
    }
    return rc;
}
