#ifndef IMU_APP_H
#define IMU_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "msa321.h"

typedef struct {
    bool present;
    bool int_ready;      /* interrupt table compiled in and sensor found */
    bool wake_armed;     /* IMU may be used as a deep sleep wake source now */
    float x_g;
    float y_g;
    float z_g;
    msa321_orientation_t orientation;
    uint32_t flip_events;
    uint32_t tap_events;
} imu_app_status_t;

/* Starts the low rate sampling task. Call after the I2C bus, the panel
 * rotation lock and the web server exist. ESP_ERR_NOT_FOUND means the sensor
 * did not answer: callers log it and keep running without the IMU. */
esp_err_t imu_app_start(void);

/* Thread-safe copy for /api/imu. */
void imu_app_status(imu_app_status_t *out);
const char *imu_app_orientation_name(msa321_orientation_t orientation);

/* True when GPIO4 may be armed as a deep sleep motion wake source. Safe to
 * call before the sampling task exists (maintenance wake). */
bool imu_app_wake_supported(void);

/* Called by power_mgr_deep_sleep(): clears a latched event and programs the
 * any-motion interrupt. False means the IMU cannot wake the chip and the timer
 * / power key sources are used alone. */
bool imu_app_prepare_wake(void);
void imu_app_clear_wake(void);

#endif /* IMU_APP_H */
