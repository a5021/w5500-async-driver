/**
 * @file w5500_port_rtos.h
 * @brief FreeRTOS synchronization hooks for the W5500 platform interface.
 *
 * Provides a global, plain (non-recursive) mutex bound to the platform
 * lock/unlock callbacks. Intended for builds with a single W5500 driver
 * instance; for multi-chip systems provide your own lock closures.
 */
#ifndef W5500_PORT_RTOS_H
#define W5500_PORT_RTOS_H

#include "w5500.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the driver mutex (call once, before w5500_init()).
 * @return W5500_OK or W5500_ERR_NOMEM when allocation fails.
 */
w5500_result_t w5500_rtos_lock_init(void);

/** Delete the driver mutex (call after w5500_deinit()). */
void w5500_rtos_lock_deinit(void);

/** Mutex take hook: assign to w5500_plat_if_t.lock. */
void w5500_rtos_lock(void);

/** Mutex give hook: assign to w5500_plat_if_t.unlock. */
void w5500_rtos_unlock(void);

/**
 * @brief Convenience: set plat->lock/unlock to the FreeRTOS mutex hooks.
 * Call w5500_rtos_lock_init() first.
 */
void w5500_rtos_bind_plat(w5500_plat_if_t *plat);

#ifdef __cplusplus
}
#endif

#endif /* W5500_PORT_RTOS_H */
