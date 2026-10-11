/**
 * @file fuzz_send_recv.c
 * @brief Fuzzes the socket data path: send/sendto/recv/recvfrom/rx_available/
 *        tx_free/get_peer plus explicit NULL/zero-length negative probes.
 *
 * Input layout:
 *   [0]    flags: bit0 = proto (1 => UDP, else TCP), bit1 = burst path,
 *                 bit2 = cr_stuck (applied AFTER setup), bit3 = send_stall
 *                 (applied AFTER setup)
 *   [1..4] setup peer IP for the TCP remote_connect
 *   [..]   opcode script (<= FUZZ_MAX_OPS, bounds checked):
 *            0 SEND     len, payload
 *            1 SENDTO   ip0..ip3, port_hi, port_lo, len, payload
 *            2 RECV     len
 *            3 INJECT   len(<=32), payload
 *            4 RXAVAIL  (0 args)
 *            5 TXFREE   (0 args)
 *            6 GET_PEER (0 args)
 *            7 NEGATIVE probes: NULL buffers, zero lengths (0 args)
 *
 * Properties:
 *   1. *sent <= len on success; W5500_ERR_TIMEOUT means the SEND command
 *      never completed, so *sent must be 0; protocol/state mismatches are
 *      W5500_ERR_STATE without touching *sent.
 *   2. *recvd <= len and <= injected bytes; RX_RSR always equals the
 *      tracked pending count (including the bytes the driver consumed
 *      right before a stuck RECV command timed out).
 *   3. TX/RX queries stay inside the 2 KB default buffer envelope.
 *   4. GET_PEER on an established TCP socket returns the peer that the
 *      setup installed.
 */
#include "fuzz_common.h"

#define NEED(n)                                                              \
    do {                                                                     \
        if (p + (size_t)(n) > size) {                                        \
            goto done;                                                       \
        }                                                                    \
    } while (0)

static w5500_socket_t g_sock;
static unsigned g_id;
static unsigned g_events_connected;

static void evt_cb(w5500_socket_t sock, w5500_event_t event,
                   w5500_result_t status, void *user_arg)
{
    (void)user_arg;
    fuzz_check_rc(status);
    if (sock == NULL || sock->id >= W5500_SOCKET_COUNT) {
        abort();
    }
    if (event == W5500_EVENT_CONNECTED) {
        g_events_connected++;
    }
    if (event == W5500_EVENT_DISCONNECTED || event == W5500_EVENT_ERROR) {
        g_sock = NULL; /* the driver released our socket */
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    w5500_handle_t h;
    fuzz_lock_snap_t snap;
    w5500_proto_t proto;
    uint32_t pending = 0u;
    size_t p = 5u;
    unsigned ops = 0u;
    uint8_t ip[4];

    if (size < 5u) {
        return 0;
    }

    proto = ((data[0] & 1u) != 0u) ? W5500_PROTO_UDP : W5500_PROTO_TCP;
    ip[0] = data[1];
    ip[1] = data[2];
    ip[2] = data[3];
    ip[3] = data[4];

    h = fuzz_open_handle((data[0] >> 1) & 1u);
    g_sock = NULL;
    g_events_connected = 0u;

    if (w5500_socket_open(h, proto, 0x1F90u, evt_cb, NULL, &g_sock) !=
        W5500_OK) {
        abort(); /* clean chip at setup */
    }
    g_id = g_sock->id;

    if (proto == W5500_PROTO_TCP) {
        w5500_result_t rc;
        mock_w5500_remote_connect((uint8_t)g_id, ip, 12345u);
        rc = w5500_poll(h);
        fuzz_check_rc(rc);
        if (g_events_connected == 0u ||
            h->sockets[g_id].state != W5500_SSR_ESTABLISHED) {
            abort(); /* remote_connect + poll must surface CONNECTED */
        }
    }

    /* Knobs only after setup so the clean open/connect path is exercised. */
    g_mock.cr_stuck = (uint8_t)((data[0] >> 2) & 1u);
    g_mock.send_stall = (uint8_t)((data[0] >> 3) & 1u);
    snap = fuzz_lock_snap();

    while (p < size && ops < FUZZ_MAX_OPS) {
        uint8_t op = data[p++];
        ops++;

        switch (op & 0x07u) {
        case 0u: { /* SEND */
            uint16_t sent = 0xFFFFu;
            uint16_t len;
            w5500_result_t rc;

            NEED(1);
            len = data[p++] % 65u;
            if (len > (uint16_t)(size - p)) {
                len = (uint16_t)(size - p);
            }
            if (g_sock == NULL) {
                rc = w5500_socket_send(g_sock, data + p, len, &sent);
                fuzz_check_rc(rc);
                if (rc != W5500_ERR_PARAM) {
                    abort();
                }
                p += len;
                break;
            }
            if (proto == W5500_PROTO_UDP) {
                rc = w5500_socket_send(g_sock, data + p, len, &sent);
                fuzz_check_rc(rc);
                if (rc != W5500_ERR_STATE) {
                    abort();
                }
                p += len;
                break;
            }
            rc = w5500_socket_send(g_sock, data + p, len, &sent);
            fuzz_check_rc(rc);
            if (rc == W5500_OK) {
                if (sent > len) {
                    abort();
                }
            } else if (rc == W5500_ERR_TIMEOUT) {
                if (sent != 0u) {
                    abort(); /* SEND never completed: nothing accepted */
                }
            } else if (rc == W5500_ERR_PARAM) {
                if (len != 0u) {
                    abort(); /* only a zero length can produce PARAM here */
                }
            } else if (rc != W5500_ERR_STATE) {
                abort();
            }
            p += len;
            break;
        }
        case 1u: { /* SENDTO */
            uint16_t sent = 0xFFFFu;
            uint16_t len;
            uint8_t dip[4];
            uint16_t dport;
            w5500_result_t rc;

            NEED(7);
            dip[0] = data[p];
            dip[1] = data[p + 1u];
            dip[2] = data[p + 2u];
            dip[3] = data[p + 3u];
            dport = (uint16_t)(((uint16_t)data[p + 4u] << 8) | data[p + 5u]);
            p += 6u;
            len = data[p++] % 65u;
            if (len > (uint16_t)(size - p)) {
                len = (uint16_t)(size - p);
            }
            if (g_sock == NULL) {
                rc = w5500_socket_sendto(g_sock, dip, dport, data + p, len,
                                         &sent);
                fuzz_check_rc(rc);
                if (rc != W5500_ERR_PARAM) {
                    abort();
                }
                p += len;
                break;
            }
            if (proto != W5500_PROTO_UDP) {
                rc = w5500_socket_sendto(g_sock, dip, dport, data + p, len,
                                         &sent);
                fuzz_check_rc(rc);
                if (rc != W5500_ERR_STATE) {
                    abort();
                }
                p += len;
                break;
            }
            rc = w5500_socket_sendto(g_sock, dip, dport, data + p, len, &sent);
            fuzz_check_rc(rc);
            if (rc == W5500_OK) {
                if (sent > len) {
                    abort();
                }
            } else if (rc == W5500_ERR_TIMEOUT) {
                if (sent != 0u) {
                    abort();
                }
            } else if (rc == W5500_ERR_PARAM) {
                if (len != 0u) {
                    abort();
                }
            } else if (rc != W5500_ERR_STATE) {
                abort();
            }
            p += len;
            break;
        }
        case 2u: { /* RECV */
            uint16_t recvd = 0xFFFFu;
            uint8_t buf[64];
            uint16_t len;
            w5500_result_t rc;

            NEED(1);
            len = data[p++] % 65u;
            if (g_sock == NULL || len == 0u) {
                rc = w5500_socket_recv(g_sock, buf, len, &recvd);
                fuzz_check_rc(rc);
                if (rc != W5500_ERR_PARAM) {
                    abort();
                }
                break;
            }
            rc = w5500_socket_recv(g_sock, buf, len, &recvd);
            fuzz_check_rc(rc);
            if (rc == W5500_OK) {
                if (recvd > len || recvd > pending) {
                    abort();
                }
                pending -= recvd;
            } else if (rc == W5500_ERR_TIMEOUT) {
                /* Sn_RX_RD was advanced before the stuck RECV command
                   failed, so the chip consumed exactly min(len, pending). */
                uint16_t consumed = (len < pending) ? len : (uint16_t)pending;
                pending -= consumed;
            } else {
                abort();
            }
            break;
        }
        case 3u: { /* INJECT */
            uint16_t len;
            NEED(1);
            len = data[p++] % 33u;
            if (len > (uint16_t)(size - p)) {
                len = (uint16_t)(size - p);
            }
            mock_w5500_inject_rx((uint8_t)g_id, data + p, len);
            pending += len;
            p += len;
            break;
        }
        case 4u: { /* RXAVAIL */
            uint16_t avail = 0u;
            if (g_sock == NULL) {
                fuzz_check_rc(w5500_socket_rx_available(g_sock, &avail));
                break;
            }
            if (w5500_socket_rx_available(g_sock, &avail) != W5500_OK) {
                abort();
            }
            if (avail != pending) {
                abort(); /* RSR must mirror the inject/recv bookkeeping */
            }
            break;
        }
        case 5u: { /* TXFREE */
            uint16_t free_bytes = 0u;
            fuzz_check_rc(w5500_socket_tx_free(g_sock, &free_bytes));
            if (g_sock != NULL && free_bytes > 2048u) {
                abort(); /* default 2 KB TX buffer envelope */
            }
            break;
        }
        case 6u: { /* GET_PEER */
            uint8_t peer[4];
            uint16_t port = 0u;
            w5500_result_t rc = w5500_socket_get_peer(g_sock, peer, &port);
            fuzz_check_rc(rc);
            if (rc == W5500_OK && proto == W5500_PROTO_TCP &&
                g_sock != NULL) {
                if (memcmp(peer, ip, 4) != 0 || port != 12345u) {
                    abort();
                }
            }
            break;
        }
        case 7u: { /* NEGATIVE probes */
            uint16_t tmp = 0xFFFFu;
            uint8_t buf[4] = { 0u, 0u, 0u, 0u };
            /* send() checks the protocol/state before its buffer arguments,
               so a UDP socket reports ERR_STATE where TCP reports PARAM. */
            w5500_result_t send_expect = (proto == W5500_PROTO_TCP)
                                             ? W5500_ERR_PARAM
                                             : W5500_ERR_STATE;

            if (g_sock == NULL) {
                break;
            }
            if (w5500_socket_send(g_sock, NULL, 4u, &tmp) != send_expect) {
                abort();
            }
            if (w5500_socket_send(g_sock, buf, 4u, NULL) != send_expect) {
                abort();
            }
            if (w5500_socket_recv(g_sock, NULL, 4u, &tmp) != W5500_ERR_PARAM) {
                abort();
            }
            if (w5500_socket_recv(g_sock, buf, 0u, &tmp) != W5500_ERR_PARAM) {
                abort();
            }
            if (w5500_socket_rx_available(g_sock, NULL) != W5500_ERR_PARAM) {
                abort();
            }
            if (w5500_socket_tx_free(g_sock, NULL) != W5500_ERR_PARAM) {
                abort();
            }
            if (w5500_socket_sendto(g_sock, NULL, 1u, buf, 1u, &tmp) !=
                W5500_ERR_PARAM) {
                abort();
            }
            break;
        }
        default:
            abort();
        }
    }

done:
    fuzz_check_locks(snap);
    w5500_deinit(h);
    return 0;
}
