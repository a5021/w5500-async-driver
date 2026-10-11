/**
 * @file fuzz_common.h
 * @brief Fixture helpers shared by the W5500 fuzz harnesses.
 *
 * Every harness iteration must start from a pristine chip model: the mock is
 * a file-scope global (g_mock), so fuzzing without a reset would leak state
 * across inputs and break reproducibility.
 */
#ifndef W5500_FUZZ_COMMON_H
#define W5500_FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_internal.h"
#include "mock_w5500.h"

/**
 * Fixed, tiny driver timeout. The mock's delay_ms() never sleeps (it only
 * bumps elapsed_ms), but a bounded wait keeps per-input runtime at a few SPI
 * transactions even with the cr_stuck bus-hang knob set. The timeout itself
 * is deliberately not fuzzed: timeout_ms drives the wait-loop trip count and
 * an input-selected value could stall the fuzzer for billions of iterations.
 */
#define FUZZ_TIMEOUT_MS 4u

/** Upper bound of the opcode-script length per fuzz input. */
#define FUZZ_MAX_OPS 32u

/** Every driver call must return one of the documented status codes. */
static inline void fuzz_check_rc(w5500_result_t rc)
{
    switch (rc) {
    case W5500_OK:
    case W5500_ERR_PARAM:
    case W5500_ERR_TIMEOUT:
    case W5500_ERR_SPI:
    case W5500_ERR_STATE:
    case W5500_ERR_NOMEM:
    case W5500_ERR_CHIP:
    case W5500_ERR_BUSY:
        return;
    default:
        abort();
    }
}

/** Snapshot of the mock's lock counters. */
typedef struct {
    uint32_t lock;
    uint32_t unlock;
} fuzz_lock_snap_t;

static inline fuzz_lock_snap_t fuzz_lock_snap(void)
{
    fuzz_lock_snap_t s;
    s.lock = g_mock.lock_count;
    s.unlock = g_mock.unlock_count;
    return s;
}

/**
 * Lock-balance invariant: every public entry point must release exactly what
 * it takes. An imbalance would deadlock the optional RTOS-mutex build.
 */
static inline void fuzz_check_locks(fuzz_lock_snap_t s)
{
    if ((g_mock.lock_count - s.lock) != (g_mock.unlock_count - s.unlock)) {
        abort();
    }
}

/**
 * Fresh driver handle bound to the mock: resets the chip model, wires the
 * platform hooks (byte-loop or burst), installs the small fixed timeout and
 * forces behaviour knobs to their pristine values. mock_w5500_reset()
 * deliberately preserves knobs across resets, so a knob left set by a
 * previous fuzz input would otherwise leak into this input's setup phase.
 * Harnesses set the knobs they want AFTER this call.
 */
static inline w5500_handle_t fuzz_open_handle(int burst)
{
    w5500_plat_if_t plat;
    w5500_handle_t h = NULL;

    mock_w5500_reset();
    g_mock.cr_stuck = 0u;
    g_mock.connect_ok = 1u;
    g_mock.send_stall = 0u;
    if (burst != 0) {
        mock_w5500_plat_burst(&plat);
    } else {
        mock_w5500_plat(&plat);
    }
    if (w5500_init(&h, &plat) != W5500_OK || h == NULL) {
        abort();
    }
    if (w5500_set_timeout(h, FUZZ_TIMEOUT_MS) != W5500_OK) {
        abort();
    }
    return h;
}

/** Number of hardware socket slots currently claimed. */
static inline unsigned fuzz_used_slots(w5500_handle_t h)
{
    unsigned i;
    unsigned n = 0u;

    for (i = 0; i < W5500_SOCKET_COUNT; i++) {
        if (h->sockets[i].in_use != 0u) {
            n++;
        }
    }
    return n;
}

#endif /* W5500_FUZZ_COMMON_H */
