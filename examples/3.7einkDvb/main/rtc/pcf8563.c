/*
 * PCF8563：掉电保时的日历时钟。首次上电（VL 置位或读数不合理）用编译日期
 * 初始化，之后由 SNTP 校时写回；开机再把 RTC 时间灌进系统时钟。
 */

#include "pcf8563.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "board.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "rtc";

#define RTC_I2C_HZ           400000
#define RTC_I2C_TIMEOUT_MS   50

/* 2024-01-01T00:00:00Z，早于它的系统时间视为还没校过时。 */
#define RTC_EPOCH_MIN        1704067200LL
/* 2101-01-01T00:00:00Z，PCF8563 的年份上限是 2099。 */
#define RTC_EPOCH_MAX        4133980800LL

#define REG_CTRL1            0x00
#define REG_CTRL2            0x01
#define REG_SECONDS          0x02
#define REG_MINUTES          0x03
#define REG_HOURS            0x04
#define REG_DAYS             0x05
#define REG_WEEKDAYS         0x06
#define REG_MONTHS           0x07
#define REG_YEARS            0x08

#define CTRL1_STOP           (1u << 5)
#define SECONDS_VL           (1u << 7)

static i2c_master_dev_handle_t s_dev;
static bool s_inited;

static const char *const k_wday[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};

static uint8_t bin2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t bcd2bin(uint8_t v)
{
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}

static int weekday_of(int year, int month, int day)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        year--;
    }
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

static void compile_time(pcf8563_time_t *out)
{
    char mon[4] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";

    memset(out, 0, sizeof(*out));
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);

    const char *found = strstr(months, mon);
    out->year = (uint16_t)year;
    out->month = found ? (uint8_t)((found - months) / 3 + 1) : 1;
    out->day = (uint8_t)day;
    out->hour = (uint8_t)hour;
    out->minute = (uint8_t)min;
    out->second = (uint8_t)sec;
    out->weekday = (uint8_t)weekday_of(out->year, out->month, out->day);
}

static esp_err_t rtc_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[9];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    board_i2c_lock();
    esp_err_t err = i2c_master_transmit(s_dev, buf, len + 1, RTC_I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return err;
}

static esp_err_t rtc_write_u8(uint8_t reg, uint8_t val)
{
    return rtc_write(reg, &val, 1);
}

static esp_err_t rtc_read(uint8_t reg, uint8_t *data, size_t len)
{
    board_i2c_lock();
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, data, len, RTC_I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return err;
}

esp_err_t pcf8563_set_time(const pcf8563_time_t *time)
{
    uint8_t raw[7];

    if (!s_inited || time == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(rtc_write_u8(REG_CTRL1, CTRL1_STOP), TAG, "stop");

    raw[0] = bin2bcd(time->second) & 0x7F;
    raw[1] = bin2bcd(time->minute) & 0x7F;
    raw[2] = bin2bcd(time->hour) & 0x3F;
    raw[3] = bin2bcd(time->day) & 0x3F;
    raw[4] = (uint8_t)(time->weekday & 0x07);
    raw[5] = bin2bcd(time->month) & 0x1F;
    raw[6] = bin2bcd((uint8_t)(time->year % 100));
    ESP_RETURN_ON_ERROR(rtc_write(REG_SECONDS, raw, sizeof(raw)), TAG, "set time");
    return rtc_write_u8(REG_CTRL1, 0x00);
}

esp_err_t pcf8563_get_time(pcf8563_time_t *time)
{
    uint8_t raw[7];

    if (!s_inited || time == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(rtc_read(REG_SECONDS, raw, sizeof(raw)), TAG, "get time");
    time->second = bcd2bin(raw[0] & 0x7F);
    time->minute = bcd2bin(raw[1] & 0x7F);
    time->hour = bcd2bin(raw[2] & 0x3F);
    time->day = bcd2bin(raw[3] & 0x3F);
    time->weekday = (uint8_t)(raw[4] & 0x07);
    time->month = bcd2bin(raw[5] & 0x1F);
    time->year = (uint16_t)(2000 + bcd2bin(raw[6]));
    return ESP_OK;
}

/* 公历日期转 Unix 天数（Howard Hinnant 算法）：不读本地时区，RTC 与 SNTP
 * 统一按 UTC 解释。 */
static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153u * (month + (month > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int64_t epoch_of(const pcf8563_time_t *t)
{
    return days_from_civil(t->year, t->month, t->day) * 86400 +
           (int64_t)t->hour * 3600 + (int64_t)t->minute * 60 + t->second;
}

esp_err_t pcf8563_restore_system_time(void)
{
    pcf8563_time_t t;
    struct timeval now;

    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(pcf8563_get_time(&t), TAG, "restore read");
    const int64_t rtc = epoch_of(&t);
    if (rtc < RTC_EPOCH_MIN || rtc >= RTC_EPOCH_MAX || t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 59) {
        ESP_LOGW(TAG, "RTC %04u-%02u-%02u %02u:%02u:%02u implausible, wait for SNTP",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
        return ESP_ERR_INVALID_STATE;
    }
    gettimeofday(&now, NULL);
    /* 只允许向前跳：SNTP 已经校准过的会话里不要把系统时钟拨回去。 */
    if (rtc <= (int64_t)now.tv_sec) {
        return ESP_OK;
    }
    struct timeval set = { .tv_sec = (time_t)rtc, .tv_usec = 0 };
    if (settimeofday(&set, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "system clock %04u-%02u-%02u %02u:%02u:%02u from RTC",
             t.year, t.month, t.day, t.hour, t.minute, t.second);
    return ESP_OK;
}

esp_err_t pcf8563_save_system_time(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    const time_t now = time(NULL);
    if ((int64_t)now < RTC_EPOCH_MIN || (int64_t)now >= RTC_EPOCH_MAX) {
        /* 还没校过时（系统时间停在 1970）：不要把无效值写进 RTC。 */
        return ESP_ERR_INVALID_STATE;
    }
    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL) {
        return ESP_FAIL;
    }
    pcf8563_time_t t = {
        .year = (uint16_t)(utc.tm_year + 1900),
        .month = (uint8_t)(utc.tm_mon + 1),
        .day = (uint8_t)utc.tm_mday,
        .weekday = (uint8_t)utc.tm_wday,
        .hour = (uint8_t)utc.tm_hour,
        .minute = (uint8_t)utc.tm_min,
        .second = (uint8_t)utc.tm_sec,
    };
    return pcf8563_set_time(&t);
}

esp_err_t pcf8563_init(void)
{
    pcf8563_time_t now = {0};
    pcf8563_time_t rd = {0};

    if (s_inited) {
        return ESP_OK;
    }
    if (board_i2c_bus() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t devcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_PCF8563,
        .scl_speed_hz = RTC_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &devcfg, &s_dev),
                        TAG, "add i2c");

    ESP_RETURN_ON_ERROR(rtc_write_u8(REG_CTRL2, 0x00), TAG, "ctrl2");
    s_inited = true;

    /* RTC 由自己的纽扣电池供电，跨深睡和掉电都在走：只有在从未设置过
     * （VL 置位）或读数明显不合理时才用编译时间兜底，否则保留原值。 */
    uint8_t raw[7] = {0};
    bool seeded = false;
    const esp_err_t read_err = rtc_read(REG_SECONDS, raw, sizeof(raw));
    const bool vl = read_err == ESP_OK && (raw[0] & SECONDS_VL) != 0;
    const bool plausible = !vl &&
        bcd2bin(raw[6]) >= 24 && bcd2bin(raw[6]) <= 99 &&
        bcd2bin(raw[5] & 0x1F) >= 1 && bcd2bin(raw[5] & 0x1F) <= 12 &&
        bcd2bin(raw[3] & 0x3F) >= 1 && bcd2bin(raw[3] & 0x3F) <= 31;
    if (!plausible) {
        compile_time(&now);
        ESP_LOGW(TAG, "RTC %s, seed from build time", vl ? "VL set" : "out of range");
        ESP_RETURN_ON_ERROR(pcf8563_set_time(&now), TAG, "set now");
        seeded = true;
    }
    ESP_RETURN_ON_ERROR(pcf8563_get_time(&rd), TAG, "readback");
    ESP_LOGI(TAG, "PCF8563 %s %04u-%02u-%02u %s %02u:%02u:%02u",
             seeded ? "set" : "kept", rd.year, rd.month, rd.day,
             rd.weekday < 7 ? k_wday[rd.weekday] : "?",
             rd.hour, rd.minute, rd.second);
    return ESP_OK;
}
