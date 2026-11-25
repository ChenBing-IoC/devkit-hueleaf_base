#ifndef POWER_MGR_H
#define POWER_MGR_H

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* Provisioning-only SoftAP is suspended after this much idle time, then the
 * CPU drops into light sleep. Any BOOT/POWER/RTC edge wakes it and reopens AP. */
#define POWER_AP_IDLE_TIMEOUT_MS (3 * 60 * 1000)

esp_err_t power_mgr_init(void);

/* The rotate-button task parks itself while the device is suspended so its
 * 10 ms polling cannot keep waking the chip out of light sleep. */
void power_mgr_register_button_task(TaskHandle_t task);
bool power_mgr_is_suspended(void);

#endif /* POWER_MGR_H */
