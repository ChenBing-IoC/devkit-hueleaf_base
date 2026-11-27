#ifndef PCF8563_H
#define PCF8563_H

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} pcf8563_time_t;

esp_err_t pcf8563_init(void);
esp_err_t pcf8563_set_time(const pcf8563_time_t *time);
esp_err_t pcf8563_get_time(pcf8563_time_t *time);

/* 开机把 RTC 时间同步到系统时钟。RTC 年份落在 2024..2100 之外、或读数不比
 * 当前系统时间新时不做任何修改，保证时钟只向前跳。 */
esp_err_t pcf8563_restore_system_time(void);
/* 把系统时钟写回 RTC（SNTP 成功后、进入深睡前调用）。系统时间还停在 2024 年
 * 之前（没有校过时）时跳过，避免用无效时间覆盖掉 RTC 的有效值。 */
esp_err_t pcf8563_save_system_time(void);

#endif /* PCF8563_H */
