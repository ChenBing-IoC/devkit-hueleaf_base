/*
 * MSA321：与 MSA311 同寄存器。4g / 125Hz，读三轴加速度。
 */

#include "msa321.h"

#include <stddef.h>
#include "board.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
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
#define REG_RANGE            0x0F
#define REG_ODR              0x10
#define REG_PWR              0x11

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

/* The MSA311/MSA321 register manual is not part of this tree and this build
 * environment has no network access. The interrupt register addresses are
 * deliberately absent instead of guessed: a wrong address would program an
 * unrelated control register on real hardware.
 *
 * To enable motion wake, fill msa321_int_config_motion() and
 * msa321_int_clear() from the register manual (any-motion threshold and
 * duration, interrupt enable, INT1 mapping, push-pull / active high / latched
 * configuration, status clear) and set the switch below to 1. imu_app and the
 * power manager pick it up without further changes. */
#ifndef MSA321_INT_REGS_VERIFIED
#define MSA321_INT_REGS_VERIFIED 0
#endif

bool msa321_int_ready(void)
{
    return msa321_present() && MSA321_INT_REGS_VERIFIED;
}

esp_err_t msa321_int_config_motion(void)
{
    if (!msa321_present()) {
        return ESP_ERR_INVALID_STATE;
    }
#if MSA321_INT_REGS_VERIFIED
    /* Program any-motion threshold/duration, enable it, map it to INT1 and set
     * push-pull, active high, latched. Return ESP_OK only after read-back. */
    return ESP_ERR_NOT_SUPPORTED;
#else
    static bool warned;
    if (!warned) {
        warned = true;
        ESP_LOGW(TAG, "motion interrupt unavailable: register table not verified");
    }
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t msa321_int_clear(void)
{
    if (!msa321_int_ready()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Read the interrupt status register until the any-motion bit clears. */
    return ESP_ERR_NOT_SUPPORTED;
}
