/**
 * @file fuzz_context.c
 * @brief Fuzzes driver lifecycle and argument validation:
 *        w5500_init / w5500_init_static / w5500_context_size / deinit,
 *        including the canary semantics of caller-provided storage.
 *
 * Input layout:
 *   [0]    flags: bit0 = burst path, bit1 = static mem_size high bit,
 *                 bit2 = which handle the script operates on
 *                 (1 => malloc handle, else static handle when available)
 *   [1]    static mem_size low byte (0..255; high bit from flags => 0..511)
 *   [..]   opcode script (<= FUZZ_MAX_OPS, bounds checked):
 *            0 SET_TIMEOUT 4 bytes (little-endian ms)
 *            1 CHECK       (0 args)
 *            2 SW_RESET    (0 args)
 *            3 SOCKET open/close round-trip (0 args)
 *            4 NEGATIVE probes: NULL handle/arguments (0 args)
 *            5 LINK probe  (0 args)
 *
 * Properties:
 *   1. init_static with mem_size < context_size() returns ERR_NOMEM,
 *      leaves *out_handle untouched and never writes into the caller's
 *      buffer (0xA5 canary intact).
 *   2. On success the handle aliases the buffer; bytes beyond
 *      context_size() keep their canary through init AND deinit (deinit
 *      only zeroes the context itself, and never free()s static storage).
 *   3. Broken/NULL platform tables or NULL out_handle are rejected with
 *      ERR_PARAM before any allocation or buffer write.
 *   4. A malloc-backed handle is fully functional: the script's driver
 *      calls behave exactly as on a static handle.
 *
 * Note: a second deinit() on the same handle is documented use-after-free
 * and is deliberately NOT fuzzed here.
 */
#include "fuzz_common.h"

#define FUZZ_CTX_BUF_SIZE 512u
#define CANARY_BYTE       0xA5u

#define NEED(n)                                                              \
    do {                                                                     \
        if (p + (size_t)(n) > size) {                                        \
            goto done;                                                       \
        }                                                                    \
    } while (0)

static union {
    max_align_t align;
    uint8_t bytes[FUZZ_CTX_BUF_SIZE];
} g_static_buf;

static void noop_cb(w5500_socket_t sock, w5500_event_t event,
                    w5500_result_t status, void *user_arg)
{
    (void)sock;
    (void)event;
    (void)status;
    (void)user_arg;
}

static void fill_canary(void)
{
    memset(g_static_buf.bytes, CANARY_BYTE, FUZZ_CTX_BUF_SIZE);
}

static void check_canary(size_t from)
{
    size_t k;
    for (k = from; k < FUZZ_CTX_BUF_SIZE; k++) {
        if (g_static_buf.bytes[k] != CANARY_BYTE) {
            abort(); /* init/deinit wrote past context_size() */
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    w5500_plat_if_t plat;
    w5500_handle_t h_static = NULL;
    w5500_handle_t h_malloc = NULL;
    w5500_handle_t h_active;
    fuzz_lock_snap_t snap;
    size_t ctx_size;
    size_t mem_size;
    int static_ok;
    int use_malloc;
    size_t p = 2u;
    unsigned ops = 0u;
    int sentinel_int = 0;
    w5500_handle_t untouched = (w5500_handle_t)&sentinel_int;

    if (size < 2u) {
        return 0;
    }

    mock_w5500_reset();
    if (data[0] & 1u) {
        mock_w5500_plat_burst(&plat);
    } else {
        mock_w5500_plat(&plat);
    }
    snap = fuzz_lock_snap();

    ctx_size = w5500_context_size();
    if (ctx_size == 0u || ctx_size > FUZZ_CTX_BUF_SIZE) {
        abort(); /* context outgrew the fuzz buffer: raise the constant */
    }
    mem_size = (size_t)data[1] +
               ((data[0] & 2u) != 0u ? 256u : 0u);

    /* --- failing init variants: nothing may be written ------------- */
    fill_canary();

    h_static = untouched;
    if (w5500_init_static(&h_static, NULL, FUZZ_CTX_BUF_SIZE, &plat) !=
            W5500_ERR_PARAM ||
        w5500_init_static(&h_static, g_static_buf.bytes, FUZZ_CTX_BUF_SIZE,
                          NULL) != W5500_ERR_PARAM) {
        abort();
    }
    if (h_static != untouched) {
        abort(); /* failure must leave *out_handle untouched */
    }

    {
        w5500_plat_if_t bad = plat;
        bad.spi_select = NULL;
        h_static = untouched;
        if (w5500_init_static(&h_static, g_static_buf.bytes, FUZZ_CTX_BUF_SIZE,
                              &bad) != W5500_ERR_PARAM) {
            abort();
        }
        if (h_static != untouched) {
            abort();
        }
        if (w5500_init(&h_malloc, &bad) != W5500_ERR_PARAM ||
            w5500_init(&h_malloc, NULL) != W5500_ERR_PARAM ||
            w5500_init(NULL, &plat) != W5500_ERR_PARAM) {
            abort();
        }
        if (h_malloc != NULL) {
            abort(); /* rejected inits must not hand out a handle */
        }
    }
    if (w5500_init_static(NULL, g_static_buf.bytes, FUZZ_CTX_BUF_SIZE,
                          &plat) != W5500_ERR_PARAM) {
        abort(); /* NULL out_handle */
    }
    check_canary(0u);
    h_static = NULL;

    /* --- ERR_NOMEM vs success, depending on the fuzzed size -------- */
    fill_canary();
    if (mem_size < ctx_size) {
        h_static = untouched;
        if (w5500_init_static(&h_static, g_static_buf.bytes, mem_size,
                              &plat) != W5500_ERR_NOMEM) {
            abort();
        }
        if (h_static != untouched) {
            abort();
        }
        check_canary(0u); /* undersized init must not touch the buffer */
        static_ok = 0;
        h_static = NULL; /* nothing to deinit */
    } else {
        h_static = untouched;
        if (w5500_init_static(&h_static, g_static_buf.bytes, mem_size,
                              &plat) != W5500_OK ||
            h_static != (w5500_handle_t)(void *)g_static_buf.bytes) {
            abort(); /* handle must alias the caller's storage */
        }
        check_canary(ctx_size); /* bytes beyond the context stay canary */
        if (w5500_set_timeout(h_static, FUZZ_TIMEOUT_MS) != W5500_OK) {
            abort();
        }
        static_ok = 1;
    }

    /* --- malloc variant -------------------------------------------- */
    if (w5500_init(&h_malloc, &plat) != W5500_OK || h_malloc == NULL) {
        abort();
    }
    if (w5500_set_timeout(h_malloc, FUZZ_TIMEOUT_MS) != W5500_OK) {
        abort();
    }

    use_malloc = ((data[0] >> 2) & 1u) != 0u;
    if (use_malloc || static_ok == 0) {
        h_active = h_malloc;
    } else {
        h_active = h_static;
    }

    /* --- script ----------------------------------------------------- */
    while (p < size && ops < FUZZ_MAX_OPS) {
        uint8_t op = data[p++];
        ops++;

        switch (op % 6u) {
        case 0u: { /* SET_TIMEOUT */
            uint32_t v;
            uint32_t prev;
            w5500_result_t rc;

            NEED(4);
            v = (uint32_t)data[p] | ((uint32_t)data[p + 1u] << 8) |
                ((uint32_t)data[p + 2u] << 16) | ((uint32_t)data[p + 3u] << 24);
            p += 4u;
            prev = h_active->timeout_ms;
            rc = w5500_set_timeout(h_active, v);
            fuzz_check_rc(rc);
            if (v == 0u) {
                if (rc != W5500_ERR_PARAM || h_active->timeout_ms != prev) {
                    abort();
                }
            } else if (rc != W5500_OK || h_active->timeout_ms != v) {
                abort();
            }
            break;
        }
        case 1u: { /* CHECK */
            uint8_t ver = 0u;
            if (w5500_check(h_active) != W5500_OK ||
                w5500_get_hw_version(h_active, &ver) != W5500_OK ||
                ver != W5500_HW_VERSION) {
                abort();
            }
            break;
        }
        case 2u: { /* SW_RESET */
            if (w5500_sw_reset(h_active) != W5500_OK ||
                g_mock.common[W5500_REG_PHYCFGR & 0x3Fu] != 0x01u) {
                abort();
            }
            break;
        }
        case 3u: { /* SOCKET open/close round-trip */
            w5500_socket_t s = NULL;
            if (w5500_socket_open(h_active, W5500_PROTO_TCP, 0x1F90u, noop_cb,
                                  NULL, &s) != W5500_OK ||
                s == NULL || s->id >= W5500_SOCKET_COUNT) {
                abort();
            }
            if (w5500_socket_close(s) != W5500_OK) {
                abort();
            }
            if (fuzz_used_slots(h_active) != 0u) {
                abort(); /* close must free the slot */
            }
            break;
        }
        case 4u: { /* NEGATIVE probes on the active handle */
            uint8_t mac[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
            uint8_t ver = 0u;

            if (w5500_set_timeout(h_active, 0u) != W5500_ERR_PARAM ||
                w5500_set_timeout(NULL, 1u) != W5500_ERR_PARAM ||
                w5500_set_mac(NULL, mac) != W5500_ERR_PARAM ||
                w5500_get_hw_version(NULL, &ver) != W5500_ERR_PARAM ||
                w5500_get_hw_version(h_active, NULL) != W5500_ERR_PARAM ||
                w5500_get_link_status(h_active, NULL) != W5500_ERR_PARAM ||
                w5500_check(NULL) != W5500_ERR_PARAM ||
                w5500_sw_reset(NULL) != W5500_ERR_PARAM ||
                w5500_init_static(NULL, NULL, 0u, NULL) != W5500_ERR_PARAM) {
                abort();
            }
            w5500_deinit(NULL); /* must be a no-op, never crash */
            break;
        }
        case 5u: { /* LINK default: mock powers up with link up */
            bool link = false;
            if (w5500_get_link_status(h_active, &link) != W5500_OK ||
                !link) {
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

    if (h_static != NULL) {
        w5500_deinit(h_static); /* frees nothing, zeroes the context only */
        check_canary(ctx_size);
    }
    w5500_deinit(h_malloc);
    return 0;
}
