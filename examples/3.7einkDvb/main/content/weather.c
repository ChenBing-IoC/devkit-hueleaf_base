#include "weather.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "content_page.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "weather";

#define WEATHER_NVS_NS      "weather"
#define WEATHER_KEY_ENABLED "enabled"
#define WEATHER_KEY_LAT     "lat"
#define WEATHER_KEY_LON     "lon"
#define WEATHER_KEY_LABEL   "label"
#define WEATHER_KEY_C_OK    "c_ok"
#define WEATHER_KEY_C_VER   "c_ver"
#define WEATHER_KEY_C_TEMP  "c_t"
#define WEATHER_KEY_C_CODE  "c_code"
#define WEATHER_KEY_C_TMAX  "c_max"
#define WEATHER_KEY_C_MIN   "c_min"
#define WEATHER_KEY_C_DAYS  "c_days"
#define WEATHER_KEY_C_ND    "c_nd"
#define WEATHER_KEY_C_OBS   "c_obs"
#define WEATHER_KEY_ERR     "err"
#define WEATHER_KEY_ERR_AT  "err_at"

#define WEATHER_CACHE_VERSION 1
#define WEATHER_BODY_BYTES    4096
#define WEATHER_TASK_STACK    8192
#define WEATHER_URL_BYTES     384
#define WEATHER_DEFAULT_LAT_E4 399042   /* 39.9042, Beijing */
#define WEATHER_DEFAULT_LON_E4 1164074  /* 116.4074 */
/* A timestamp below this cannot come from the RTC or SNTP; it is recorded as
 * "unknown" (0) instead of a meaningless 1970 value. */
#define WEATHER_TIME_VALID_MIN 1600000000U

/* Fixed endpoint. The coordinates are the only device data in the request. */
#define WEATHER_URL_FMT \
    "https://api.open-meteo.com/v1/forecast?" \
    "latitude=%.23s&longitude=%.23s&current=temperature_2m,weather_code" \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min" \
    "&forecast_days=4&timezone=Asia%%2FShanghai" \
    "&temperature_unit=celsius&wind_speed_unit=ms"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static weather_config_t s_cfg;
static weather_cache_t s_cache;
static bool s_loaded;

static const char *const k_weekday[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};

/* ---- Config and cache storage -------------------------------------------- */

static uint32_t clock_now(void)
{
    const time_t now = time(NULL);
    if (now <= 0) return 0;
    return (uint32_t)now >= WEATHER_TIME_VALID_MIN ? (uint32_t)now : 0;
}

bool weather_label_ok(const char *text)
{
    if (text == NULL) return false;
    const size_t length = strlen(text);
    if (length > WEATHER_LABEL_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

static esp_err_t cache_load(weather_cache_t *cache)
{
    memset(cache, 0, sizeof(*cache));
    nvs_handle_t nvs;
    if (nvs_open(WEATHER_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return ESP_ERR_NOT_FOUND;

    uint8_t version = 0, ok = 0;
    if (nvs_get_u8(nvs, WEATHER_KEY_C_VER, &version) == ESP_OK &&
        version == WEATHER_CACHE_VERSION &&
        nvs_get_u8(nvs, WEATHER_KEY_C_OK, &ok) == ESP_OK && ok == 1) {
        int16_t i16 = 0;
        int8_t i8 = 0;
        if (nvs_get_i16(nvs, WEATHER_KEY_C_TEMP, &i16) == ESP_OK) cache->temp_x10 = i16;
        if (nvs_get_i16(nvs, WEATHER_KEY_C_CODE, &i16) == ESP_OK) cache->code = i16;
        if (nvs_get_i8(nvs, WEATHER_KEY_C_TMAX, &i8) == ESP_OK) cache->today_max = i8;
        if (nvs_get_i8(nvs, WEATHER_KEY_C_MIN, &i8) == ESP_OK) cache->today_min = i8;
        size_t days = sizeof(cache->days);
        uint8_t count = 0;
        if (nvs_get_blob(nvs, WEATHER_KEY_C_DAYS, cache->days, &days) == ESP_OK &&
            days == sizeof(cache->days) &&
            nvs_get_u8(nvs, WEATHER_KEY_C_ND, &count) == ESP_OK) {
            cache->day_count = count <= WEATHER_DAY_COUNT ? count : WEATHER_DAY_COUNT;
            cache->has_data = true;
        }
        uint32_t at = 0;
        if (nvs_get_u32(nvs, WEATHER_KEY_C_OBS, &at) == ESP_OK) cache->observed_at = at;
    }

    size_t length = sizeof(cache->last_error);
    if (nvs_get_str(nvs, WEATHER_KEY_ERR, cache->last_error, &length) != ESP_OK) {
        cache->last_error[0] = '\0';
    } else {
        cache->last_error[WEATHER_ERROR_MAX] = '\0';
    }
    uint32_t err_at = 0;
    if (nvs_get_u32(nvs, WEATHER_KEY_ERR_AT, &err_at) == ESP_OK) cache->last_error_at = err_at;
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t weather_init(void)
{
    bool loaded = false;
    portENTER_CRITICAL(&s_lock);
    loaded = s_loaded;
    portEXIT_CRITICAL(&s_lock);
    if (loaded) return ESP_OK;

    weather_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    cfg.lat_e4 = WEATHER_DEFAULT_LAT_E4;
    cfg.lon_e4 = WEATHER_DEFAULT_LON_E4;

    nvs_handle_t nvs;
    if (nvs_open(WEATHER_NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t enabled = 1;
        if (nvs_get_u8(nvs, WEATHER_KEY_ENABLED, &enabled) == ESP_OK) {
            cfg.enabled = enabled != 0;
        }
        int32_t value = 0;
        if (nvs_get_i32(nvs, WEATHER_KEY_LAT, &value) == ESP_OK &&
            value >= -900000 && value <= 900000) {
            cfg.lat_e4 = value;
        }
        if (nvs_get_i32(nvs, WEATHER_KEY_LON, &value) == ESP_OK &&
            value >= -1800000 && value <= 1800000) {
            cfg.lon_e4 = value;
        }
        size_t length = sizeof(cfg.label);
        if (nvs_get_str(nvs, WEATHER_KEY_LABEL, cfg.label, &length) != ESP_OK) {
            cfg.label[0] = '\0';
        } else {
            cfg.label[WEATHER_LABEL_MAX] = '\0';
            if (!weather_label_ok(cfg.label)) cfg.label[0] = '\0';
        }
        nvs_close(nvs);
    }

    weather_cache_t cache;
    (void)cache_load(&cache);

    portENTER_CRITICAL(&s_lock);
    if (!s_loaded) {
        s_cfg = cfg;
        s_cache = cache;
        s_loaded = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t weather_config_get(weather_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    (void)weather_init();
    portENTER_CRITICAL(&s_lock);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t weather_config_set(const weather_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->lat_e4 < -900000 || cfg->lat_e4 > 900000) return ESP_ERR_INVALID_ARG;
    if (cfg->lon_e4 < -1800000 || cfg->lon_e4 > 1800000) return ESP_ERR_INVALID_ARG;
    if (!weather_label_ok(cfg->label)) return ESP_ERR_INVALID_ARG;
    (void)weather_init();

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WEATHER_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, WEATHER_KEY_ENABLED, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, WEATHER_KEY_LAT, cfg->lat_e4);
    if (err == ESP_OK) err = nvs_set_i32(nvs, WEATHER_KEY_LON, cfg->lon_e4);
    if (err == ESP_OK) err = nvs_set_str(nvs, WEATHER_KEY_LABEL, cfg->label);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config save: %s", esp_err_to_name(err));
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    s_cfg = *cfg;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "config lat=%d lon=%d label=%s enabled=%d",
             (int)cfg->lat_e4, (int)cfg->lon_e4, cfg->label, (int)cfg->enabled);
    return ESP_OK;
}

void weather_cache_get(weather_cache_t *out)
{
    if (out == NULL) return;
    (void)weather_init();
    portENTER_CRITICAL(&s_lock);
    *out = s_cache;
    portEXIT_CRITICAL(&s_lock);
}

int weather_age_seconds(const weather_cache_t *cache)
{
    if (cache == NULL || !cache->has_data || cache->observed_at == 0) return -1;
    const uint32_t now = clock_now();
    if (now == 0) return -1;
    if (now < cache->observed_at) return 0;
    return (int)(now - cache->observed_at);
}

/* ---- Result persistence -------------------------------------------------- */

static void persist_error(const char *text, uint32_t at)
{
    nvs_handle_t nvs;
    if (nvs_open(WEATHER_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        esp_err_t err = nvs_set_str(nvs, WEATHER_KEY_ERR, text);
        if (err == ESP_OK) err = nvs_set_u32(nvs, WEATHER_KEY_ERR_AT, at);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
        if (err != ESP_OK) ESP_LOGW(TAG, "error save: %s", esp_err_to_name(err));
    }
}

/* Records a short failure label; the cache itself is left untouched. */
static esp_err_t set_error(const char *text)
{
    const uint32_t at = clock_now();
    (void)weather_init();
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_cache.last_error, text, sizeof(s_cache.last_error));
    s_cache.last_error_at = at;
    portEXIT_CRITICAL(&s_lock);
    persist_error(text, at);
    ESP_LOGW(TAG, "fetch failed: %s", text);
    return ESP_FAIL;
}

static esp_err_t set_error_code(const char *text, int code)
{
    char label[WEATHER_ERROR_MAX + 1];
    char number[16];
    snprintf(number, sizeof(number), "%d", code);
    strlcpy(label, text, sizeof(label));
    strlcat(label, " ", sizeof(label));
    strlcat(label, number, sizeof(label));
    return set_error(label);
}

static void persist_cache(const weather_cache_t *cache)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WEATHER_NVS_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, WEATHER_KEY_C_VER, WEATHER_CACHE_VERSION);
        if (err == ESP_OK) err = nvs_set_u8(nvs, WEATHER_KEY_C_OK, 1);
        if (err == ESP_OK) err = nvs_set_i16(nvs, WEATHER_KEY_C_TEMP, cache->temp_x10);
        if (err == ESP_OK) err = nvs_set_i16(nvs, WEATHER_KEY_C_CODE, cache->code);
        if (err == ESP_OK) err = nvs_set_i8(nvs, WEATHER_KEY_C_TMAX, cache->today_max);
        if (err == ESP_OK) err = nvs_set_i8(nvs, WEATHER_KEY_C_MIN, cache->today_min);
        if (err == ESP_OK) err = nvs_set_blob(nvs, WEATHER_KEY_C_DAYS, cache->days,
                                              sizeof(cache->days));
        if (err == ESP_OK) err = nvs_set_u8(nvs, WEATHER_KEY_C_ND, cache->day_count);
        if (err == ESP_OK) err = nvs_set_u32(nvs, WEATHER_KEY_C_OBS, cache->observed_at);
        if (err == ESP_OK) err = nvs_set_str(nvs, WEATHER_KEY_ERR, "");
        if (err == ESP_OK) err = nvs_set_u32(nvs, WEATHER_KEY_ERR_AT, 0);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "cache save: %s", esp_err_to_name(err));

    weather_cache_t result = *cache;
    result.has_data = true;
    result.last_error[0] = '\0';
    result.last_error_at = 0;
    portENTER_CRITICAL(&s_lock);
    s_cache = result;
    portEXIT_CRITICAL(&s_lock);
}

/* ---- Parsing ------------------------------------------------------------- */

static bool json_number(const cJSON *object, const char *key, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = item->valuedouble;
    return true;
}

static int8_t round_temp(double value)
{
    int rounded = (int)(value >= 0 ? value + 0.5 : value - 0.5);
    if (rounded < -60) rounded = -60;
    if (rounded > 60) rounded = 60;
    return (int8_t)rounded;
}

static void weekday_from_date(const char *date, char *dst, size_t size)
{
    int year = 0, month = 0, day = 0;
    if (date == NULL || sscanf(date, "%4d-%2d-%2d", &year, &month, &day) != 3) {
        strlcpy(dst, "---", size);
        return;
    }
    struct tm value;
    memset(&value, 0, sizeof(value));
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = 12; /* noon avoids a DST edge shifting the weekday */
    if (mktime(&value) == (time_t)-1) {
        strlcpy(dst, "---", size);
        return;
    }
    strlcpy(dst, k_weekday[((value.tm_wday % 7) + 7) % 7], size);
}

static esp_err_t parse_body(const char *body, size_t length, weather_cache_t *out)
{
    cJSON *root = cJSON_ParseWithLength(body, length);
    if (root == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ESP_ERR_INVALID_ARG;
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    double temperature = 0, code = 0;
    if (cJSON_IsObject(current) &&
        json_number(current, "temperature_2m", &temperature) &&
        json_number(current, "weather_code", &code)) {
        const cJSON *times = cJSON_GetObjectItemCaseSensitive(daily, "time");
        const cJSON *codes = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
        const cJSON *maxs = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
        const cJSON *mins = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
        if (cJSON_IsArray(times) && cJSON_IsArray(codes) &&
            cJSON_IsArray(maxs) && cJSON_IsArray(mins)) {
            const int count = cJSON_GetArraySize(times);
            int day = 0;
            for (int i = 0; i < count && day < WEATHER_DAY_COUNT; ++i) {
                const cJSON *t = cJSON_GetArrayItem(times, i);
                const cJSON *wc = cJSON_GetArrayItem(codes, i);
                const cJSON *mx = cJSON_GetArrayItem(maxs, i);
                const cJSON *mn = cJSON_GetArrayItem(mins, i);
                if (!cJSON_IsString(t) || !cJSON_IsNumber(wc) ||
                    !cJSON_IsNumber(mx) || !cJSON_IsNumber(mn)) {
                    continue;
                }
                weather_day_t *entry = &out->days[day];
                weekday_from_date(t->valuestring, entry->weekday, sizeof(entry->weekday));
                entry->code = (int16_t)wc->valueint;
                entry->max_c = round_temp(mx->valuedouble);
                entry->min_c = round_temp(mn->valuedouble);
                entry->valid = 1;
                ++day;
            }
            out->day_count = (uint8_t)day;
            int temp_x10 = (int)(temperature * 10 +
                                 (temperature >= 0 ? 0.5 : -0.5));
            if (temp_x10 < -600) temp_x10 = -600;
            if (temp_x10 > 600) temp_x10 = 600;
            const int current_rounded = (temp_x10 >= 0)
                                            ? (temp_x10 + 5) / 10
                                            : (temp_x10 - 5) / 10;
            out->temp_x10 = (int16_t)temp_x10;
            out->code = (int16_t)code;
            if (day > 0) {
                out->today_max = out->days[0].max_c;
                out->today_min = out->days[0].min_c;
            } else {
                out->today_max = (int8_t)current_rounded;
                out->today_min = (int8_t)current_rounded;
            }
            out->observed_at = clock_now();
            out->has_data = true;
            err = ESP_OK;
        }
    }
    cJSON_Delete(root);
    return err;
}

/* ---- HTTPS fetch --------------------------------------------------------- */

typedef struct {
    char *body;
    size_t used;
    size_t capacity;
    bool overflow;
} http_sink_t;

static esp_err_t http_sink_event(esp_http_client_event_t *event)
{
    http_sink_t *sink = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || sink == NULL ||
        event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }
    if (sink->used + (size_t)event->data_len >= sink->capacity) {
        sink->overflow = true;
        return ESP_FAIL; /* aborts esp_http_client_perform() */
    }
    memcpy(sink->body + sink->used, event->data, (size_t)event->data_len);
    sink->used += (size_t)event->data_len;
    sink->body[sink->used] = '\0';
    return ESP_OK;
}

void weather_coord_text(int32_t e4, char *dst, size_t size)
{
    if (dst == NULL || size == 0) return;
    const int32_t magnitude = e4 < 0 ? -e4 : e4;
    char whole[16];
    char fraction[16];
    snprintf(whole, sizeof(whole), "%d", (int)(magnitude / 10000));
    snprintf(fraction, sizeof(fraction), "%04d", (int)(magnitude % 10000));
    dst[0] = '\0';
    if (e4 < 0) strlcat(dst, "-", size);
    strlcat(dst, whole, size);
    strlcat(dst, ".", size);
    strlcat(dst, fraction, size);
}

static void build_url(const weather_config_t *cfg, char *dst, size_t size)
{
    char lat[24];
    char lon[24];
    weather_coord_text(cfg->lat_e4, lat, sizeof(lat));
    weather_coord_text(cfg->lon_e4, lon, sizeof(lon));
    snprintf(dst, size, WEATHER_URL_FMT, lat, lon);
}

typedef struct {
    weather_config_t cfg;
    uint32_t timeout_ms;
    SemaphoreHandle_t done;
    esp_err_t result;
} fetch_job_t;

static esp_err_t fetch_once(const fetch_job_t *job)
{
    char url[WEATHER_URL_BYTES];
    build_url(&job->cfg, url, sizeof(url));

    char *body = malloc(WEATHER_BODY_BYTES);
    if (body == NULL) return set_error("no memory");
    body[0] = '\0';
    http_sink_t sink = {
        .body = body,
        .used = 0,
        .capacity = WEATHER_BODY_BYTES,
        .overflow = false,
    };
    const esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_sink_event,
        .user_data = &sink,
        .timeout_ms = (int)job->timeout_ms,
        .disable_auto_redirect = false,
        .max_redirection_count = 3,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(body);
        return set_error("no memory");
    }
    const esp_err_t perform_err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (perform_err != ESP_OK) {
        ESP_LOGW(TAG, "perform %s status=%d", esp_err_to_name(perform_err), status);
        free(body);
        return set_error(sink.overflow ? "body too big" : "net error");
    }
    if (status != 200) {
        free(body);
        return set_error_code("http", status);
    }
    if (sink.overflow) {
        free(body);
        return set_error("body too big");
    }
    /* Length only: the raw JSON is never printed. */
    ESP_LOGI(TAG, "response %u bytes", (unsigned)sink.used);

    weather_cache_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    const esp_err_t parse_err = parse_body(sink.body, sink.used, &parsed);
    free(body);
    if (parse_err != ESP_OK) return set_error("parse");
    persist_cache(&parsed);
    ESP_LOGI(TAG, "cached temp=%d code=%d days=%u obs=%u",
             (int)parsed.temp_x10 / 10, (int)parsed.code,
             (unsigned)parsed.day_count, (unsigned)parsed.observed_at);
    return ESP_OK;
}

static void fetch_task(void *argument)
{
    fetch_job_t *job = argument;
    job->result = fetch_once(job);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

esp_err_t weather_fetch(uint32_t timeout_ms)
{
    (void)weather_init();
    weather_config_t cfg;
    weather_config_get(&cfg);
    if (!cfg.enabled) return ESP_ERR_INVALID_STATE;
    if (timeout_ms == 0) timeout_ms = WEATHER_FETCH_TIMEOUT_MS;

    fetch_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) return ESP_ERR_NO_MEM;
    job->cfg = cfg;
    job->timeout_ms = timeout_ms;
    job->done = xSemaphoreCreateBinary();
    if (job->done == NULL) {
        free(job);
        return ESP_ERR_NO_MEM;
    }
    /* The TLS handshake needs more stack than the maintenance (app_main) and
     * job tasks can spare, so the request runs in its own short-lived task. */
    if (xTaskCreate(fetch_task, "weather", WEATHER_TASK_STACK, job, 5, NULL) != pdPASS) {
        vSemaphoreDelete(job->done);
        free(job);
        return set_error("no memory");
    }

    const TickType_t wait = pdMS_TO_TICKS(timeout_ms + 500);
    if (xSemaphoreTake(job->done, wait) != pdTRUE) {
        /* The client is configured with the same budget, so this only happens
         * when the network stack itself is wedged. The worker still owns job;
         * leave it allocated instead of freeing memory it may write to. */
        ESP_LOGW(TAG, "fetch overran %u ms", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = job->result;
    vSemaphoreDelete(job->done);
    free(job);
    return result;
}

/* ---- Formatting for content_page ----------------------------------------- */

const char *weather_code_text(int code)
{
    switch (code) {
    case 0: return "CLEAR";
    case 1: return "PARTLY";
    case 2: return "CLOUDY";
    case 3: return "OVERCAST";
    case 45: case 48: return "FOG";
    case 51: case 53: case 55: case 56: case 57: return "DRIZZLE";
    case 61: case 63: case 65: return "RAIN";
    case 66: case 67: return "SLEET";
    case 71: case 73: case 75: case 77: return "SNOW";
    case 80: case 81: case 82: return "SHOWERS";
    case 85: case 86: return "SNOW";
    case 95: case 96: case 99: return "STORM";
    default: return "N/A";
    }
}

static void append_int(char *dst, size_t size, int value)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    strlcat(dst, text, size);
}

static void observed_text(const weather_cache_t *cache, char *dst, size_t size)
{
    if (cache->observed_at == 0) {
        strlcpy(dst, "OBS --", size);
        return;
    }
    const time_t at = (time_t)cache->observed_at;
    struct tm local;
    if (localtime_r(&at, &local) == NULL) {
        strlcpy(dst, "OBS --", size);
        return;
    }
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
    dst[0] = '\0';
    strlcat(dst, "OBS ", size);
    strlcat(dst, clock, size);
}

void weather_format_lines(const weather_cache_t *cache,
                          char *main_line, size_t main_size,
                          char *detail_line, size_t detail_size,
                          char *forecast_line, size_t forecast_size)
{
    if (main_line == NULL || main_size == 0) return;
    if (detail_line != NULL && detail_size > 0) detail_line[0] = '\0';
    if (forecast_line != NULL && forecast_size > 0) forecast_line[0] = '\0';
    if (cache == NULL || !cache->has_data) {
        strlcpy(main_line, "WEATHER --", main_size);
        return;
    }

    const int temperature = (cache->temp_x10 >= 0)
                                ? (cache->temp_x10 + 5) / 10
                                : (cache->temp_x10 - 5) / 10;
    strlcpy(main_line, "", main_size);
    append_int(main_line, main_size, temperature);
    strlcat(main_line, "C ", main_size);
    strlcat(main_line, weather_code_text(cache->code), main_size);

    if (detail_line != NULL && detail_size > 0) {
        strlcpy(detail_line, "H ", detail_size);
        append_int(detail_line, detail_size, cache->today_max);
        strlcat(detail_line, " L ", detail_size);
        append_int(detail_line, detail_size, cache->today_min);
        strlcat(detail_line, " ", detail_size);
        char observed[12];
        observed_text(cache, observed, sizeof(observed));
        strlcat(detail_line, observed, detail_size);
    }

    if (forecast_line != NULL && forecast_size > 0) {
        for (int i = 0; i < cache->day_count && i < WEATHER_DAY_COUNT; ++i) {
            if (!cache->days[i].valid) continue;
            if (forecast_line[0] != '\0') strlcat(forecast_line, " ", forecast_size);
            strlcat(forecast_line, i == 0 ? "TOD" : cache->days[i].weekday, forecast_size);
            strlcat(forecast_line, " ", forecast_size);
            append_int(forecast_line, forecast_size, cache->days[i].max_c);
            strlcat(forecast_line, "/", forecast_size);
            append_int(forecast_line, forecast_size, cache->days[i].min_c);
        }
    }
}

void weather_publish_page(void)
{
    weather_cache_t cache;
    weather_cache_get(&cache);
    char main_line[24];
    char detail_line[48];
    char forecast_line[80];
    weather_format_lines(&cache, main_line, sizeof(main_line),
                         detail_line, sizeof(detail_line),
                         forecast_line, sizeof(forecast_line));
    content_page_set_weather_lines(main_line, detail_line, forecast_line);
}
