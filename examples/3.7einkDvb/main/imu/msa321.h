#ifndef MSA321_H
#define MSA321_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

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

/* Any-motion interrupt on the INT1 pin. The register table is only compiled in
 * when it has been verified against the MSA311/MSA321 register manual (see
 * MSA321_INT_REGS_VERIFIED in msa321.c); until then these calls return
 * ESP_ERR_NOT_SUPPORTED and the device keeps polling only. */
esp_err_t msa321_int_config_motion(void);
esp_err_t msa321_int_clear(void);
bool msa321_int_ready(void);

#endif /* MSA321_H */
