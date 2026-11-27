#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WEATHER_LABEL_MAX        16
#define WEATHER_DAY_COUNT        4
#define WEATHER_ERROR_MAX        24
#define WEATHER_FETCH_TIMEOUT_MS 8000

/* Persisted under NVS namespace "weather". Coordinates are stored as fixed
 * point (degrees x 10000) so they survive NVS without float blobs and print
 * back exactly. */
typedef struct {
    bool enabled;                             /* default true */
    int32_t lat_e4;                           /* -900000..900000 */
    int32_t lon_e4;                           /* -1800000..1800000 */
    char label[WEATHER_LABEL_MAX + 1];        /* printable ASCII, may be empty */
} weather_config_t;

/* One forecast day. weekday is three uppercase letters; day 0 is "TOD" when
 * the renderer formats it. Struct layout is part of the NVS blob, bump
 * WEATHER_CACHE_VERSION when it changes. */
typedef struct {
    char weekday[4];
    int8_t max_c;
    int8_t min_c;
    int16_t code;                             /* WMO weather code */
    uint8_t valid;
} weather_day_t;

typedef struct {
    bool has_data;
    int16_t temp_x10;                         /* current temperature x 10 */
    int16_t code;
    int8_t today_max;
    int8_t today_min;
    uint8_t day_count;                        /* 0..WEATHER_DAY_COUNT */
    weather_day_t days[WEATHER_DAY_COUNT];
    uint32_t observed_at;                     /* Unix seconds, 0 = no clock */
    char last_error[WEATHER_ERROR_MAX + 1];   /* last failed fetch, "" = ok */
    uint32_t last_error_at;
} weather_cache_t;

/* Load config and cache from NVS once. Safe to call repeatedly; every getter
 * also lazily loads, so calling it early is only an optimisation. */
esp_err_t weather_init(void);

esp_err_t weather_config_get(weather_config_t *out);
/* Validates ranges and the label, writes NVS, then updates the RAM copy. */
esp_err_t weather_config_set(const weather_config_t *cfg);
bool weather_label_ok(const char *text);

/* HTTPS fetch from Open-Meteo and parse. Updates the cache and NVS only on
 * success; on failure the old cache is kept and the error text is recorded.
 * Returns within timeout_ms (plus a small scheduling margin). */
esp_err_t weather_fetch(uint32_t timeout_ms);

void weather_cache_get(weather_cache_t *out);
/* Seconds since observed_at, or -1 when unknown / clock unset. */
int weather_age_seconds(const weather_cache_t *cache);

const char *weather_code_text(int code);
/* "39.9042" / "-0.5000" from degrees x 10000. */
void weather_coord_text(int32_t e4, char *dst, size_t size);

/* Builds the three standby-page lines: "18C CLEAR", "H 22 L 11 OBS 14:32"
 * and "TOD 22/11 THU 20/11 ...". Without data: main "WEATHER --" and both
 * detail and forecast empty. */
void weather_format_lines(const weather_cache_t *cache,
                          char *main_line, size_t main_size,
                          char *detail_line, size_t detail_size,
                          char *forecast_line, size_t forecast_size);

/* Push the cached result into content_page_set_weather_lines(). */
void weather_publish_page(void);

#endif /* WEATHER_H */
