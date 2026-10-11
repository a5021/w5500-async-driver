/**
 * @file w5500_port_rtos.c
 * @brief FreeRTOS mutex backing the W5500 platform lock/unlock hooks.
 */
#include "FreeRTOS.h"
#include "semphr.h"

#include "w5500_port_rtos.h"

static SemaphoreHandle_t s_w5500_mutex = NULL;

w5500_result_t w5500_rtos_lock_init(void)
{
    if (s_w5500_mutex != NULL) {
        return W5500_OK;
    }
    s_w5500_mutex = xSemaphoreCreateMutex();
    return (s_w5500_mutex != NULL) ? W5500_OK : W5500_ERR_NOMEM;
}

void w5500_rtos_lock_deinit(void)
{
    if (s_w5500_mutex != NULL) {
        vSemaphoreDelete(s_w5500_mutex);
        s_w5500_mutex = NULL;
    }
}

void w5500_rtos_lock(void)
{
    if (s_w5500_mutex != NULL) {
        (void)xSemaphoreTake(s_w5500_mutex, portMAX_DELAY);
    }
}

void w5500_rtos_unlock(void)
{
    if (s_w5500_mutex != NULL) {
        (void)xSemaphoreGive(s_w5500_mutex);
    }
}

void w5500_rtos_bind_plat(w5500_plat_if_t *plat)
{
    if (plat == NULL) {
        return;
    }
    plat->lock = w5500_rtos_lock;
    plat->unlock = w5500_rtos_unlock;
}
