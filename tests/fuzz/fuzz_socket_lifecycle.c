/**
 * @file fuzz_socket_lifecycle.c
 * @brief Drives open/close/listen/connect/disconnect/poll as a fuzz-chosen
 *        opcode script against the chip emulator.
 *
 * Input layout:
 *   [0]   flags: bit0 = cr_stuck knob, bit1 = connect_ok knob
 *   [1..] opcode script (<= FUZZ_MAX_OPS operations, bounds checked):
 *           0 OPEN      3 args: proto_byte, port_hi, port_lo
 *           1 CLOSE     1 arg : slot index
 *           2 LISTEN    1 arg : slot index
 *           3 CONNECT   7 args: idx, ip0..ip3, port_hi, port_lo
 *           4 DISCONNECT 1 arg: slot index
 *           5 POLL      0 args
 *           6 SETBUF    1 arg : buffer size byte
 *           7 POKE_IR   2 args: idx, Sn_IR bits
 *
 * Properties:
 *   1. Opening when all four slots are claimed returns W5500_ERR_NOMEM
 *      (checked before any chip interaction, hence knob-independent);
 *      with the cr_stuck knob an OPEN of a free slot returns exactly
 *      W5500_ERR_TIMEOUT and must not publish a handle.
 *   2. The harness's slot mirror stays in exact agreement with the
 *      driver's in_use flags after every operation — a poll that releases
 *      a slot without delivering the matching event (the bug fixed in
 *      43dbc8b would silently drop it) trips this immediately.
 *   3. Lock balance, valid result codes, no leaks (LSan on deinit).
 */
#include "fuzz_common.h"

/* Harness-side mirror of the driver's socket slots, indexed by hardware id. */
static w5500_socket_t g_slots[W5500_SOCKET_COUNT];

static void evt_cb(w5500_socket_t sock, w5500_event_t event,
                   w5500_result_t status, void *user_arg)
{
    (void)user_arg;
    fuzz_check_rc(status);
    if (sock == NULL || sock->owner == NULL) {
        abort();
    }
    if (sock->id >= W5500_SOCKET_COUNT) {
        abort();
    }
    if (event == W5500_EVENT_DISCONNECTED || event == W5500_EVENT_ERROR) {
        /* The driver releases the slot before/at dispatch. */
        g_slots[sock->id] = NULL;
    } else if (g_slots[sock->id] == NULL) {
        /* An event for a slot the driver considers free. */
        abort();
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    w5500_handle_t h;
    fuzz_lock_snap_t snap;
    size_t p = 1u;
    unsigned ops = 0u;

    if (size < 1u) {
        return 0;
    }

    h = fuzz_open_handle(0);
    g_mock.cr_stuck = (uint8_t)(data[0] & 0x01u);
    g_mock.connect_ok = (uint8_t)((data[0] >> 1) & 0x01u);
    memset(g_slots, 0, sizeof(g_slots));
    snap = fuzz_lock_snap();
    check_consistency(h);

    while (p < size && ops < FUZZ_MAX_OPS) {
        uint8_t op = data[p++];
        unsigned n_args;
        uint8_t a[7];
        unsigned i;

        switch (op & 0x07u) {
        case 0u: n_args = 3u; break; /* OPEN */
        case 3u: n_args = 7u; break; /* CONNECT */
        case 7u: n_args = 2u; break; /* POKE_IR */
        case 5u: n_args = 0u; break; /* POLL */
        default: n_args = 1u; break; /* CLOSE/LISTEN/DISCONNECT/SETBUF */
        }
        if (p + n_args > size) {
            break; /* truncated argument tail */
        }
        for (i = 0; i < n_args; i++) {
            a[i] = data[p + i];
        }
        p += n_args;
        ops++;

        switch (op & 0x07u) {
        case 0u: { /* OPEN */
            w5500_socket_t out = NULL;
            w5500_proto_t proto = ((a[0] & 1u) != 0u) ? W5500_PROTO_UDP
                                                       : W5500_PROTO_TCP;
            uint16_t port = (uint16_t)(((uint16_t)a[1] << 8) | a[2]);
            w5500_result_t rc;
            w5500_result_t expect;

            if (fuzz_used_slots(h) >= W5500_SOCKET_COUNT) {
                /* Checked before any chip interaction: knob-independent. */
                expect = W5500_ERR_NOMEM;
            } else if (g_mock.cr_stuck != 0u) {
                /* OPEN never self-clears Sn_CR: bounded timeout, and the
                   driver releases the slot again on this path. */
                expect = W5500_ERR_TIMEOUT;
            } else {
                expect = W5500_OK;
            }
            rc = w5500_socket_open(h, proto, port, evt_cb, NULL, &out);
            fuzz_check_rc(rc);
            if (rc != expect) {
                abort();
            }
            if (rc == W5500_OK) {
                if (out == NULL || out->id >= W5500_SOCKET_COUNT) {
                    abort();
                }
                if (g_slots[out->id] != NULL) {
                    abort(); /* driver handed out a claimed slot */
                }
                g_slots[out->id] = out;
            } else if (out != NULL) {
                abort(); /* failure must not publish a handle */
            }
            break;
        }
        case 1u: { /* CLOSE */
            w5500_socket_t s = g_slots[a[0] & 0x03u];
            w5500_result_t rc = w5500_socket_close(s);
            fuzz_check_rc(rc);
            if (rc == W5500_OK) {
                g_slots[a[0] & 0x03u] = NULL;
            }
            break;
        }
        case 2u: { /* LISTEN */
            w5500_result_t rc = w5500_socket_listen(g_slots[a[0] & 0x03u]);
            fuzz_check_rc(rc);
            break;
        }
        case 3u: { /* CONNECT */
            uint8_t ip[4];
            uint16_t port;
            ip[0] = a[1];
            ip[1] = a[2];
            ip[2] = a[3];
            ip[3] = a[4];
            port = (uint16_t)(((uint16_t)a[5] << 8) | a[6]);
            fuzz_check_rc(w5500_socket_connect(g_slots[a[0] & 0x03u], ip,
                                               port));
            break;
        }
        case 4u: { /* DISCONNECT */
            fuzz_check_rc(w5500_socket_disconnect(g_slots[a[0] & 0x03u]));
            break;
        }
        case 5u: /* POLL */
            fuzz_check_rc(w5500_poll(h));
            break;
        case 6u: { /* SETBUF */
            w5500_result_t rc =
                w5500_set_socket_buffer_size(h, a[0]); /* raw byte, incl. 0 */
            fuzz_check_rc(rc);
            if (rc == W5500_OK) {
                memset(g_slots, 0, sizeof(g_slots)); /* slots invalidated */
            }
            break;
        }
        case 7u: { /* POKE_IR: feed Sn_IR behind the driver's back */
            g_mock.sreg[a[0] & 0x03u][W5500_Sn_IR] |= a[1];
            break;
        }
        default:
            abort();
        }

        check_consistency(h);
    }

    fuzz_check_locks(snap);
    w5500_deinit(h);
    return 0;
}
