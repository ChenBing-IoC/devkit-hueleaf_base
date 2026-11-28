/*
 * MSA321：与 MSA311 同寄存器。4g / 125Hz，读三轴加速度。
 */

#include "msa321.h"

#include <stddef.h>
#include "board.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

#define IMU_I2C_HZ           400000
#define IMU_I2C_TIMEOUT_MS   50
#define IMU_PARTID           0x13
#define IMU_LSB_PER_G        512.0f   /* 12bit ±4g */

#define REG_SOFTRESET        0x00
#define REG_PARTID           0x01
#define REG_ACC_X_L          0x02
#define REG_MOTION_INT       0x09   /* RO interrupt status */
#define REG_TAP_ACTIVE_STATUS 0x0B  /* RO which axis started the event */
#define REG_RANGE            0x0F
#define REG_ODR              0x10
#define REG_PWR              0x11
#define REG_INT_SET_0        0x16   /* R/W per-interrupt enable */
#define REG_INT_MAP_0        0x19   /* R/W route interrupts to INT1 */
#define REG_INT_CONFIG       0x20   /* R/W INT1 output type and level */
#define REG_INT_LATCH        0x21   /* R/W latch mode + RESET_INT */
#define REG_ACTIVE_DUR       0x27   /* R/W ACTIVE_DUR[1:0] */
#define REG_ACTIVE_TH        0x28   /* R/W ACTIVE_TH[7:0] */

/* MSA321 register map, pages 20/23/24/25. */
#define MOTION_INT_ACTIVE    0x04   /* 0x09 bit2 ACTIVE_INT, read only */
#define INT_SET_ACTIVE_XYZ   0x07   /* 0x16 ACTIVE_INT_EN_Z/Y/X */
#define INT_MAP_INT1_ACTIVE  0x04   /* 0x19 bit2 INT1_ACTIVE */
/* 0x20 carries only INT1_OD (bit1, 0 = push-pull) and INT1_LVL (bit0,
 * 0 = active low); the part has a single interrupt pin, no INT2 fields. */
#define INT_CONFIG_ACTIVE_HIGH 0x01 /* 0x20 push-pull, active high */
#define INT_LATCH_RESET_INT  0x80   /* 0x21 bit7, write 1 clears every latch */
#define INT_LATCH_MODE       0x07   /* 0x21 LATCH_INT 0111 = latched */

#define RANGE_4G             0x01
#define ODR_125HZ            0x07
#define PWR_NORMAL           0x12
#define SOFTRESET_CMD        0x24

static i2c_master_dev_handle_t s_dev;
static bool s_inited;
static bool s_present;

static esp_err_t imu_write_u8(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    board_i2c_lock();
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), IMU_I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return err;
}

static esp_err_t imu_read(uint8_t reg, uint8_t *data, size_t len)
{
    board_i2c_lock();
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, data, len, IMU_I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return err;
}

static int16_t unpack12(uint8_t lo, uint8_t hi)
{
    return (int16_t)(((int16_t)((hi << 8) | lo)) >> 4);
}

esp_err_t msa321_read(msa321_sample_t *sample)
{
    uint8_t raw[6];

    if (!s_inited || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(imu_read(REG_ACC_X_L, raw, sizeof(raw)), TAG, "acc");
    sample->raw_x = unpack12(raw[0], raw[1]);
    sample->raw_y = unpack12(raw[2], raw[3]);
    sample->raw_z = unpack12(raw[4], raw[5]);
    sample->x_g = (float)sample->raw_x / IMU_LSB_PER_G;
    sample->y_g = (float)sample->raw_y / IMU_LSB_PER_G;
    sample->z_g = (float)sample->raw_z / IMU_LSB_PER_G;
    return ESP_OK;
}

esp_err_t msa321_init(void)
{
    uint8_t partid = 0;

    if (s_inited) {
        return ESP_OK;
    }
    if (board_i2c_bus() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t devcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_MSA321,
        .scl_speed_hz = IMU_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &devcfg, &s_dev),
                        TAG, "add i2c");

    ESP_RETURN_ON_ERROR(imu_write_u8(REG_SOFTRESET, SOFTRESET_CMD), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(imu_read(REG_PARTID, &partid, 1), TAG, "partid");
    if (partid != IMU_PARTID) {
        ESP_LOGW(TAG, "PARTID=0x%02X, expected 0x%02X", partid, IMU_PARTID);
    } else {
        ESP_LOGI(TAG, "MSA321 PARTID=0x%02X", partid);
    }
    /* Something answered at 0x62: keep the sensor usable even when the part
     * ID differs (clone or future revision), the gestures only need data. */
    s_present = true;

    ESP_RETURN_ON_ERROR(imu_write_u8(REG_RANGE, RANGE_4G), TAG, "range");
    ESP_RETURN_ON_ERROR(imu_write_u8(REG_ODR, ODR_125HZ), TAG, "odr");
    ESP_RETURN_ON_ERROR(imu_write_u8(REG_PWR, PWR_NORMAL), TAG, "pwr");
    vTaskDelay(pdMS_TO_TICKS(5));

    s_inited = true;
    ESP_LOGI(TAG, "MSA321 ready  4g 125Hz");
    return ESP_OK;
}

bool msa321_present(void)
{
    return s_inited && s_present;
}

bool msa321_int_ready(void)
{
    return msa321_present();
}

esp_err_t msa321_active_status(uint8_t *raw)
{
    if (!msa321_present() || raw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return imu_read(REG_TAP_ACTIVE_STATUS, raw, 1);
}

/* Set bits in a register without touching the rest: 0x16 and 0x19 also carry
 * the orientation and tap enables, which must survive a motion re-arm. */
static esp_err_t imu_set_bits(uint8_t reg, uint8_t bits)
{
    uint8_t val = 0;
    ESP_RETURN_ON_ERROR(imu_read(reg, &val, 1), TAG, "read 0x%02X", reg);
    return imu_write_u8(reg, (uint8_t)(val | bits));
}

esp_err_t msa321_int_config_motion(void)
{
    uint8_t cfg = 0, latch = 0, set0 = 0, map0 = 0, th = 0, dur = 0;

    if (!msa321_present()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Order matters only for 0x21: it is written last so the latch cannot hold
     * an event that the threshold write itself created. */
    ESP_RETURN_ON_ERROR(imu_write_u8(REG_INT_CONFIG, INT_CONFIG_ACTIVE_HIGH),
                        TAG, "int config");
    ESP_RETURN_ON_ERROR(imu_write_u8(REG_ACTIVE_TH, MSA321_ACTIVE_TH),
                        TAG, "active th");
    ESP_RETURN_ON_ERROR(imu_write_u8(REG_ACTIVE_DUR, MSA321_ACTIVE_DUR),
                        TAG, "active dur");
    ESP_RETURN_ON_ERROR(imu_set_bits(REG_INT_SET_0, INT_SET_ACTIVE_XYZ),
                        TAG, "int set");
    ESP_RETURN_ON_ERROR(imu_set_bits(REG_INT_MAP_0, INT_MAP_INT1_ACTIVE),
                        TAG, "int map");
    /* Latched so INT1 stays high for the whole sleep; RESET_INT stays 0. */
    ESP_RETURN_ON_ERROR(imu_write_u8(REG_INT_LATCH, INT_LATCH_MODE),
                        TAG, "int latch");

    ESP_RETURN_ON_ERROR(imu_read(REG_INT_CONFIG, &cfg, 1), TAG, "rb cfg");
    ESP_RETURN_ON_ERROR(imu_read(REG_INT_LATCH, &latch, 1), TAG, "rb latch");
    ESP_RETURN_ON_ERROR(imu_read(REG_INT_SET_0, &set0, 1), TAG, "rb set");
    ESP_RETURN_ON_ERROR(imu_read(REG_INT_MAP_0, &map0, 1), TAG, "rb map");
    ESP_RETURN_ON_ERROR(imu_read(REG_ACTIVE_TH, &th, 1), TAG, "rb th");
    ESP_RETURN_ON_ERROR(imu_read(REG_ACTIVE_DUR, &dur, 1), TAG, "rb dur");

    ESP_LOGI(TAG, "int regs 0x20=%02X 0x21=%02X 0x16=%02X 0x19=%02X 0x28=%02X 0x27=%02X",
             cfg, latch, set0, map0, th, dur);
    /* Only the meaningful bits are compared: reserved bits may read back
     * differently on a clone, but a dropped write must not arm the wake. */
    if ((cfg & 0x03) != INT_CONFIG_ACTIVE_HIGH ||
        (latch & 0x0F) != INT_LATCH_MODE ||
        (set0 & INT_SET_ACTIVE_XYZ) != INT_SET_ACTIVE_XYZ ||
        (map0 & INT_MAP_INT1_ACTIVE) != INT_MAP_INT1_ACTIVE ||
        th != MSA321_ACTIVE_TH ||
        dur != (uint8_t)(MSA321_ACTIVE_DUR & 0x03)) {
        ESP_LOGE(TAG, "motion interrupt read-back mismatch");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "motion wake: %d mg over %d ms, INT1 latched high",
             (int)(MSA321_ACTIVE_TH * 781 / 100), (int)(MSA321_ACTIVE_DUR + 1));
    return ESP_OK;
}

esp_err_t msa321_int_clear(void)
{
    uint8_t status = 0;
    uint8_t cfg = 0;
    uint8_t latch = 0;

    if (!msa321_int_ready()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 0x21 = 0x87: RESET_INT (write 1 clears all latched interrupts) plus
     * LATCH_INT 0111, so the configuration survives the clear and the next
     * event latches again. Three retries 2 ms apart cover a sample landing
     * between the write and the status read. */
    for (int attempt = 0; attempt <= 3; attempt++) {
        ESP_RETURN_ON_ERROR(imu_write_u8(REG_INT_LATCH,
                                         (uint8_t)(INT_LATCH_RESET_INT | INT_LATCH_MODE)),
                            TAG, "int reset");
        /* The map documents RESET_INT as a level, not as self-clearing. Leaving
         * it at 1 would hold every latch in reset and INT1 would never assert,
         * so release it when the read-back shows it is still set. */
        ESP_RETURN_ON_ERROR(imu_read(REG_INT_LATCH, &latch, 1), TAG, "rb latch");
        if ((latch & INT_LATCH_RESET_INT) != 0) {
            ESP_RETURN_ON_ERROR(imu_write_u8(REG_INT_LATCH,
                                             (uint8_t)(latch & INT_LATCH_MODE)),
                                TAG, "reset release");
        }
        ESP_RETURN_ON_ERROR(imu_read(REG_MOTION_INT, &status, 1), TAG, "status");
        ESP_RETURN_ON_ERROR(imu_read(REG_INT_CONFIG, &cfg, 1), TAG, "cfg");
        if ((status & MOTION_INT_ACTIVE) == 0) {
            ESP_LOGI(TAG, "active interrupt cleared, 0x09=%02X 0x20=%02X", status, cfg);
            return ESP_OK;
        }
        /* Real 2 ms: the 100 Hz tick would round pdMS_TO_TICKS(2) down to 0
         * and the retry would race the status register. */
        esp_rom_delay_us(2000);
    }

    ESP_LOGE(TAG, "active interrupt still latched: 0x09=%02X 0x20=%02X", status, cfg);
    return ESP_FAIL;
}
