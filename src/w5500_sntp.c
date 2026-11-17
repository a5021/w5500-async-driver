/**
 * @file w5500_sntp.c
 * @brief Minimal SNTP client over one driver UDP socket.
 */
#include <string.h>

#include "w5500_sntp.h"
#include "w5500_sockets.h"
#include "w5500_sntp_internal.h"

#define SNTP_SERVER_PORT   123u
#define SNTP_LOCAL_PORT    53536u
#define SNTP_RETRY_MS      2000u
#define SNTP_MAX_ATTEMPTS  3u

#define SNTP_MODE_CLIENT   3u
#define SNTP_MODE_SERVER   4u
#define SNTP_LI_VN_MODE    ((0u << 6) | (4u << 3) | SNTP_MODE_CLIENT) /* v4 */

#define SNTP_STATE_OFF     0u
#define SNTP_STATE_WAITING 1u

struct w5500_sntp {
    w5500_handle_t  chip;
    w5500_socket_t  sock;

    uint8_t         server[4];
    uint32_t        t_last;
    uint8_t         state;
    uint8_t         attempts;

    w5500_sntp_cb_t cb;
    void           *user;

    uint8_t         rx[W5500_SNTP_PKT_LEN];
    uint8_t         tx[W5500_SNTP_PKT_LEN];
};

int w5500_sntp_parse_response(const uint8_t *pkt, uint16_t len,
                              uint32_t *unix_time)
{
    uint32_t ntp;
    uint8_t mode;

    if (pkt == NULL || unix_time == NULL || len < W5500_SNTP_PKT_LEN) {
        return -1;
    }
    mode = (uint8_t)(pkt[0] & 0x07u);
    if ((pkt[0] & 0xC0u) == 0xC0u) {
        return -1; /* LI=3: clock unsynchronised */
    }
    if (mode != SNTP_MODE_SERVER) {
        return -1;
    }
    if (pkt[1] == 0u) {
        return -1; /* kiss-of-death / unsynchronised server */
    }
    ntp = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
          ((uint32_t)pkt[42] << 8) | (uint32_t)pkt[43];
    if (ntp <= W5500_SNTP_UNIX_DELTA) {
        return -1; /* no usable transmit timestamp */
    }
    *unix_time = ntp - W5500_SNTP_UNIX_DELTA;
    return 0;
}

static void sntp_noop_cb(w5500_socket_t sock, w5500_event_t event,
                         w5500_result_t status, void *user_arg)
{
    (void)sock;
    (void)event;
    (void)status;
    (void)user_arg;
}

static void sntp_finish(struct w5500_sntp *s, uint32_t unix_time,
                        w5500_result_t status)
{
    s->state = SNTP_STATE_OFF;
    if (s->cb != NULL) {
        s->cb(s, unix_time, status, s->user);
    }
}

static w5500_result_t sntp_transmit(struct w5500_sntp *s)
{
    uint16_t sent = 0u;
    w5500_result_t rc;

    s->tx[0] = SNTP_LI_VN_MODE;
    rc = w5500_socket_sendto(s->sock, s->server, SNTP_SERVER_PORT, s->tx,
                             (uint16_t)sizeof(s->tx), &sent);
    if (rc == W5500_ERR_BUSY) {
        return W5500_OK; /* retried from poll() on the next tick */
    }
    return rc;
}

size_t w5500_sntp_size(void)
{
    return sizeof(struct w5500_sntp);
}

w5500_result_t w5500_sntp_init(w5500_sntp_t **out_sntp,
                               void *mem,
                               size_t mem_size,
                               w5500_handle_t chip,
                               const uint8_t server[4],
                               w5500_sntp_cb_t cb,
                               void *user_arg)
{
    struct w5500_sntp *s;

    if (out_sntp == NULL || mem == NULL || chip == NULL || server == NULL ||
        cb == NULL) {
        return W5500_ERR_PARAM;
    }
    if (mem_size < sizeof(*s)) {
        return W5500_ERR_NOMEM;
    }
    s = (struct w5500_sntp *)mem;
    memset(s, 0, sizeof(*s));
    s->chip = chip;
    memcpy(s->server, server, 4u);
    s->cb = cb;
    s->user = user_arg;
    s->state = SNTP_STATE_OFF;
    *out_sntp = s;
    return W5500_OK;
}

w5500_result_t w5500_sntp_set_server(w5500_sntp_t *sntp,
                                     const uint8_t server[4])
{
    if (sntp == NULL || server == NULL) {
        return W5500_ERR_PARAM;
    }
    memcpy(sntp->server, server, 4u);
    return W5500_OK;
}

w5500_result_t w5500_sntp_start(w5500_sntp_t *sntp, uint32_t now_ms)
{
    w5500_result_t rc;

    if (sntp == NULL) {
        return W5500_ERR_PARAM;
    }
    if (sntp->sock == NULL) {
        rc = w5500_socket_open(sntp->chip, W5500_PROTO_UDP, SNTP_LOCAL_PORT,
                               sntp_noop_cb, sntp, &sntp->sock);
        if (rc != W5500_OK) {
            sntp->sock = NULL;
            return rc;
        }
    }
    memset(sntp->tx, 0, sizeof(sntp->tx));
    sntp->state = SNTP_STATE_WAITING;
    sntp->attempts = 1u;
    sntp->t_last = now_ms;
    return sntp_transmit(sntp);
}

w5500_result_t w5500_sntp_poll(w5500_sntp_t *sntp, uint32_t now_ms)
{
    uint8_t packets = 0u;

    if (sntp == NULL) {
        return W5500_ERR_PARAM;
    }
    if (sntp->state != SNTP_STATE_WAITING) {
        return W5500_ERR_STATE;
    }

    while (packets < 4u) {
        uint16_t n = 0u;
        uint32_t unix_time = 0u;
        w5500_result_t rc = w5500_socket_recv(sntp->sock, sntp->rx,
                                              (uint16_t)sizeof(sntp->rx), &n);

        if (rc != W5500_OK) {
            return rc;
        }
        if (n == 0u) {
            break;
        }
        if (w5500_sntp_parse_response(sntp->rx, n, &unix_time) == 0) {
            sntp_finish(sntp, unix_time, W5500_OK);
            return W5500_OK;
        }
        packets++;
    }

    if ((int32_t)(now_ms - sntp->t_last) >= (int32_t)SNTP_RETRY_MS) {
        if (sntp->attempts >= SNTP_MAX_ATTEMPTS) {
            sntp_finish(sntp, 0u, W5500_ERR_TIMEOUT);
            return W5500_OK;
        }
        (void)sntp_transmit(sntp);
        sntp->t_last = now_ms;
        sntp->attempts++;
    }
    return W5500_OK;
}

void w5500_sntp_stop(w5500_sntp_t *sntp)
{
    if (sntp == NULL) {
        return;
    }
    if (sntp->sock != NULL) {
        (void)w5500_socket_close(sntp->sock);
        sntp->sock = NULL;
    }
    sntp->state = SNTP_STATE_OFF;
}
