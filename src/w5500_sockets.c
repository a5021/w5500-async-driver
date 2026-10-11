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

static void w5500_sock_release(w5500_socket_t sock)
{
    sock->in_use = 0u;
    sock->state = W5500_SSR_FREE;
    sock->proto = W5500_Sn_MR_CLOSE;
    sock->cb = NULL;
    sock->user_arg = NULL;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

w5500_result_t w5500_socket_open(w5500_handle_t handle,
                                 w5500_proto_t proto,
                                 uint16_t local_port,
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

    mr = (proto == W5500_PROTO_TCP) ? W5500_Sn_MR_TCP : W5500_Sn_MR_UDP;
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
    slot->proto = mr;
    slot->state = W5500_SSR_INITIATED;
    slot->cb = cb;
    slot->user_arg = user_arg;

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
    w5500_unlock(handle);
    return W5500_OK;
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
        *sent = chunk;
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

    w5500_lock(sock->owner);
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

    h = sock->owner;
    w5500_lock(h);
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

    /* Peer info must be latched before the data is consumed (datasheet). */
    if (ip != NULL) {
        rc = w5500_ll_read(h, W5500_Sn_DIPR, W5500_BSB_SREG(sock->id), ip, 4u);
        if (rc != W5500_OK) {
            w5500_unlock(h);
            return rc;
        }
    }
    if (port != NULL) {
        rc = w5500_read16(h, W5500_Sn_DPORT, W5500_BSB_SREG(sock->id), port);
        if (rc != W5500_OK) {
            w5500_unlock(h);
            return rc;
        }
    }

    chunk = (len < avail) ? len : avail;

    rc = w5500_read16(h, W5500_Sn_RX_RD, W5500_BSB_SREG(sock->id), &rd);
    if (rc != W5500_OK) {
        w5500_unlock(h);
        return rc;
    }
    /* Zero-copy: chip RX memory is streamed straight into the user buffer. */
    rc = w5500_ll_read(h, rd, W5500_BSB_RXBUF(sock->id), data, chunk);
    if (rc != W5500_OK) {
        w5500_unlock(h);
        return rc;
    }
    rc = w5500_write16(h, W5500_Sn_RX_RD, W5500_BSB_SREG(sock->id),
                       (uint16_t)(rd + chunk));
    if (rc == W5500_OK) {
        rc = w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_RECV);
    }
    if (rc == W5500_OK) {
        *recvd = chunk;
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

static void w5500_poll_one(w5500_handle_t h, w5500_socket_t sock,
                           w5500_evt_t *events, uint8_t *count)
{
    uint8_t ir = 0u;
    uint8_t sr = 0u;
    w5500_result_t rc;

    rc = w5500_read8(h, W5500_Sn_IR, W5500_BSB_SREG(sock->id), &ir);
    if (rc != W5500_OK) {
        return;
    }
    (void)w5500_sock_read_sr(sock, &sr);

    if (ir == 0u) {
        /* Passive close can also be observed via SR alone (e.g. RST). */
        if (sock->state == W5500_SSR_CLOSING && sr == W5500_SOCK_CLOSED) {
            (void)w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CLOSE);
            w5500_poll_push(events, count, sock, W5500_EVENT_DISCONNECTED,
                            W5500_OK);
            w5500_sock_release(sock);
        } else if (sock->state != W5500_SSR_CLOSING &&
                   sock->proto == W5500_Sn_MR_TCP &&
                   sr == W5500_SOCK_CLOSED) {
            /* Connection dropped without an IR flag. */
            w5500_poll_push(events, count, sock, W5500_EVENT_DISCONNECTED,
                            W5500_OK);
            w5500_sock_release(sock);
        }
        return;
    }

    /* Write-1-to-clear the bits we are about to process. */
    (void)w5500_write8(h, W5500_Sn_IR, W5500_BSB_SREG(sock->id), ir);

    if ((ir & W5500_Sn_IR_CON) != 0u) {
        sock->state = W5500_SSR_ESTABLISHED;
        w5500_poll_push(events, count, sock, W5500_EVENT_CONNECTED, W5500_OK);
    }

    if ((ir & W5500_Sn_IR_RECV) != 0u) {
        w5500_poll_push(events, count, sock, W5500_EVENT_DATA_RECEIVED,
                        W5500_OK);
    }

    if ((ir & W5500_Sn_IR_SENDOK) != 0u) {
        w5500_poll_push(events, count, sock, W5500_EVENT_SEND_DONE, W5500_OK);
    }

    if ((ir & W5500_Sn_IR_TIMEOUT) != 0u) {
        (void)w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CLOSE);
        w5500_poll_push(events, count, sock, W5500_EVENT_ERROR,
                        W5500_ERR_TIMEOUT);
        w5500_sock_release(sock);
        return;
    }

    if ((ir & W5500_Sn_IR_DISCON) != 0u) {
        (void)w5500_sn_exec_cmd(h, sock->id, W5500_Sn_CR_CLOSE);
        w5500_poll_push(events, count, sock, W5500_EVENT_DISCONNECTED,
                        W5500_OK);
        w5500_sock_release(sock);
    }
}

w5500_result_t w5500_poll(w5500_handle_t handle)
{
    w5500_evt_t events[W5500_POLL_MAX_EVENTS];
    uint8_t count = 0u;
    uint8_t i;
    w5500_result_t rc = W5500_OK;

    if (handle == NULL) {
        return W5500_ERR_PARAM;
    }

    w5500_lock(handle);
    for (i = 0; i < W5500_SOCKET_COUNT; i++) {
        if (handle->sockets[i].in_use != 0u) {
            w5500_poll_one(handle, &handle->sockets[i], events, &count);
        }
    }
    w5500_unlock(handle);

    /* Callbacks run with the lock released — re-entering the driver is safe. */
    for (i = 0; i < count; i++) {
        if (events[i].cb != NULL) {
            events[i].cb(events[i].sock, events[i].event, events[i].status,
                         events[i].user_arg);
        }
    }
    return rc;
}
