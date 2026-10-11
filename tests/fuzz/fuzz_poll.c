/**
 * @file fuzz_poll.c
 * @brief Fuzzes w5500_poll() event dispatch: fuzzed Sn_IR / Sn_SR pokes,
 *        RX injections, remote close, and API calls interleaved with polls.
 *
 * Input layout:
 *   [0]   flags: bits0-3 = proto for socket i, bits4-5 = sockets to open
 *                (0..3), bit6 = burst path, bit7 = cr_stuck (applied after
 *                setup so the opens themselves stay clean)
 *   [1]   setup: bit i => socket i also gets mock_w5500_remote_connect()
 *   [2..] opcode script (<= FUZZ_MAX_OPS, bounds checked):
 *           0 POKE_IR   2 args: idx, bits
 *           1 POKE_SR   2 args: idx, raw Sn_SR value
 *           2 INJECT    2+len args: idx, len(<=32), payload
 *           3 REMOTE_DISC 1 arg: idx
 *           4 POLL      0 args
 *           5 API_CLOSE 1 arg: idx
 *           6 API_DISCONNECT 1 arg: idx
 *           7 SETBUF    1 arg: buffer size byte
 *
 * Properties:
 *   1. Reentrancy: the callback re-enters the driver (rx_available,
 *      tx_free, send attempt) — callbacks run with the lock released, so
 *      this must not deadlock or corrupt state; lock balance is checked.
 *   2. A slot mirror updated from callbacks must agree with in_use after
 *      every poll: a released slot whose event was silently dropped (the
 *      pre-43dbc8b behaviour) leaves the mirror stale and trips this.
 *   3. The poll layer never dispatches an event for a slot it did not
 *      claim at poll start.
 */
#include "fuzz_common.h"

static w5500_socket_t g_slots[W5500_SOCKET_COUNT];
static unsigned g_events;

static void evt_cb(w5500_socket_t sock, w5500_event_t event,
                   w5500_result_t status, void *user_arg)
{
    uint8_t scratch[16];
    uint16_t n = 0u;

    (void)user_arg;
    fuzz_check_rc(status);
    if (sock == NULL || sock->owner == NULL ||
        sock->id >= W5500_SOCKET_COUNT) {
        abort();
    }
    if ((unsigned)event > (unsigned)W5500_EVENT_ERROR) {
        abort();
    }
    /* The mirror is only cleared inside this callback; an entry-time NULL
       means poll dispatched for a slot that was free at poll start. */
    if (g_slots[sock->id] == NULL) {
        abort();
    }
    g_events++;

    /* Re-enter the driver from callback context (documented as safe).
       On release events the slot is already freed, so these legitimately
       return W5500_ERR_PARAM — any other documented code is fine too. */
    fuzz_check_rc(w5500_socket_rx_available(sock, &n));
    fuzz_check_rc(w5500_socket_tx_free(sock, &n));
    fuzz_check_rc(w5500_socket_recv(sock, scratch, sizeof(scratch), &n));

    if (event == W5500_EVENT_DISCONNECTED || event == W5500_EVENT_ERROR) {
        g_slots[sock->id] = NULL;
    }
}

static void check_consistency(w5500_handle_t h)
{
    unsigned i;

    for (i = 0; i < W5500_SOCKET_COUNT; i++) {
        int mirror = (g_slots[i] != NULL);
        int driver = (h->sockets[i].in_use != 0);
        if (mirror != driver) {
            abort();
        }
    }
}

static w5500_result_t do_poll(w5500_handle_t h)
{
    w5500_result_t rc = w5500_poll(h);
    fuzz_check_rc(rc);
    check_consistency(h);
    return rc;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    w5500_handle_t h;
    fuzz_lock_snap_t snap;
    unsigned n_open;
    unsigned i;
    size_t p = 2u;
    unsigned ops = 0u;

    if (size < 2u) {
        return 0;
    }

    h = fuzz_open_handle((data[0] >> 6) & 1u);
    memset(g_slots, 0, sizeof(g_slots));
    g_events = 0u;
    n_open = (data[0] >> 4) & 0x03u;

    for (i = 0; i < n_open; i++) {
        w5500_socket_t out = NULL;
        w5500_proto_t proto = ((data[0] >> i) & 1u) ? W5500_PROTO_UDP
                                                     : W5500_PROTO_TCP;
        uint8_t ip[4];

        if (w5500_socket_open(h, proto, (uint16_t)(5000u + i), evt_cb, NULL,
                              &out) != W5500_OK) {
            abort(); /* clean chip at setup: this must succeed */
        }
        g_slots[out->id] = out;

        ip[0] = 10u;
        ip[1] = 0u;
        ip[2] = 0u;
        ip[3] = (uint8_t)(i + 1u);
        if (((data[1] >> i) & 1u) != 0u) {
            mock_w5500_remote_connect(out->id, ip, (uint16_t)(6000u + i));
        }
    }
    g_mock.cr_stuck = (uint8_t)((data[0] >> 7) & 1u);
    snap = fuzz_lock_snap();
    check_consistency(h);

    while (p < size && ops < FUZZ_MAX_OPS) {
        uint8_t op = data[p++];
        unsigned idx;
        w5500_socket_t s;

        ops++;
        if ((op & 0x07u) != 4u && p >= size) {
            break; /* operation argument tail is truncated */
        }
        idx = (p < size) ? (unsigned)(data[p] % 4u) : 0u;
        s = g_slots[idx];

        switch (op & 0x07u) {
        case 0u: { /* POKE_IR */
            if (p + 2u > size) {
                goto done;
            }
            g_mock.sreg[data[p] % 4u][W5500_Sn_IR] |= data[p + 1u];
            p += 2u;
            break;
        }
        case 1u: { /* POKE_SR: arbitrary raw socket status */
            if (p + 2u > size) {
                goto done;
            }
            g_mock.sreg[data[p] % 4u][W5500_Sn_SR] = data[p + 1u];
            p += 2u;
            break;
        }
        case 2u: { /* INJECT */
            unsigned len;
            if (p + 2u > size) {
                goto done;
            }
            idx = data[p] % 4u;
            len = data[p + 1u] % 33u;
            p += 2u;
            if (p + len > size) {
                len = (size > p) ? (size - p) : 0u;
            }
            mock_w5500_inject_rx((uint8_t)idx, data + p, (uint16_t)len);
            p += len;
            break;
        }
        case 3u: /* REMOTE_DISC */
            p++;
            mock_w5500_remote_disconnect((uint8_t)idx);
            break;
        case 4u: /* POLL */
            (void)do_poll(h);
            break;
        case 5u: { /* API_CLOSE */
            w5500_result_t rc = w5500_socket_close(s);
            p++;
            fuzz_check_rc(rc);
            if (rc == W5500_OK) {
                g_slots[idx] = NULL;
            }
            break;
        }
        case 6u: /* API_DISCONNECT */
            p++;
            fuzz_check_rc(w5500_socket_disconnect(s));
            break;
        case 7u: { /* SETBUF */
            w5500_result_t rc;
            p++;
            rc = w5500_set_socket_buffer_size(h, data[p - 1u]);
            fuzz_check_rc(rc);
            if (rc == W5500_OK) {
                memset(g_slots, 0, sizeof(g_slots));
            }
            break;
        }
        default:
            abort();
        }
        check_consistency(h);
    }

done:
    (void)do_poll(h); /* flush whatever the script left pending */
    /* Each poll dispatches at most W5500_SOCKET_COUNT * 4 events. */
    if (g_events > (FUZZ_MAX_OPS + 1u) * (W5500_SOCKET_COUNT * 4u)) {
        abort();
    }
    fuzz_check_locks(snap);
    w5500_deinit(h);
    return 0;
}
