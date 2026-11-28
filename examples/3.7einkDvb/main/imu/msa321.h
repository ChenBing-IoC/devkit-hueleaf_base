#ifndef MSA321_H
#define MSA321_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Any-motion (active) threshold and duration, meant to be tuned in the field.
 * On the 4 g range one threshold LSB is 7.81 mg, so MSA321_ACTIVE_TH 0x14 is
 * 156 mg. The detector compares acceleration over a fixed 1 ms window and
 * fires when the slope stays above the threshold for MSA321_ACTIVE_DUR + 1
 * consecutive windows (0x01 = 2 ms).
 *
 * Desk vibration waking the device: raise the threshold to 0x20..0x30.
 * Picking the device up does not wake it: lower the threshold towards 0x0C. */
#ifndef MSA321_ACTIVE_TH
#define MSA321_ACTIVE_TH 0x14
#endif
#ifndef MSA321_ACTIVE_DUR
#define MSA321_ACTIVE_DUR 0x01
#endif

typedef struct {
    int16_t raw_x;
    int16_t raw_y;
    int16_t raw_z;
    float x_g;
    float y_g;
    float z_g;
} msa321_sample_t;

/* Sensor-frame orientation derived from the gravity vector. The chip is not
 * remapped to the panel, so the names describe the sensor axes. */
typedef enum {
    MSA321_ORIENT_UNKNOWN = 0,
    MSA321_ORIENT_PORTRAIT,
    MSA321_ORIENT_LANDSCAPE,
    MSA321_ORIENT_FACEUP,
    MSA321_ORIENT_FACEDOWN,
} msa321_orientation_t;

esp_err_t msa321_init(void);
esp_err_t msa321_read(msa321_sample_t *sample);
/* True after init and a successful PARTID read. */
bool msa321_present(void);

/* Any-motion (active) interrupt on INT1, used to wake the chip from deep
 * sleep: three axes enabled, mapped to INT1, push-pull active high, latched.
 * Threshold and duration come from MSA321_ACTIVE_TH / MSA321_ACTIVE_DUR above.
 * Every call returns ESP_ERR_INVALID_STATE when the sensor did not answer, so
 * the power manager can fall back to the timer and the power key. */
esp_err_t msa321_int_config_motion(void);
/* Drops a latched active event with RESET_INT and keeps the latch armed. */
esp_err_t msa321_int_clear(void);
bool msa321_int_ready(void);
/* Raw 0x0B Tap_Active_Status: bit3 ACTIVE_SIGN (1 = positive), bit2..0
 * ACTIVE_FIRST_X/Y/Z. Only used to log which axis started an event. */
esp_err_t msa321_active_status(uint8_t *raw);

#endif /* MSA321_H */
