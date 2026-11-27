#include "cJSON.h"
#include "content_page.h"
#include "lunar.h"
#include "web_server.h"
#include "pin_map.h"
#include "power_mgr.h"
#include "screen_rotation.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "axp2101.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "imu_app.h"
#include "screen_driver.h"
#include "screen_ui.h"
#include "tf_card.h"
#include "wifi_mgr.h"
#include "lwip/sockets.h"

static const char *TAG = "web";

typedef enum { JOB_IDLE, JOB_REFRESH, JOB_RECEIVING, JOB_ERROR } job_phase_t;
typedef enum {
    JOB_FRAME, JOB_CLEAR_SCREEN, JOB_TF_IMPORT, JOB_TF_EXPORT, JOB_PANEL_PROBE,
    JOB_CONTENT
} job_kind_t;
#define FRAME_FORMAT "otp6-v1"
#define FRAME_CHUNK_BYTES 2048
#define SLOT_COUNT 3
#define THUMB_WIDTH 60
#define THUMB_HEIGHT 90
#define THUMB_BYTES (THUMB_WIDTH * THUMB_HEIGHT * 4)
#define TF_LIST_MAX 64
#define TF_NAME_MAX 64
#define TF_CHUNK_BYTES 2048
#define TF_JSON_BYTES 8192

static portMUX_TYPE s_job_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile job_phase_t s_job_phase = JOB_IDLE;
static esp_err_t s_job_err = ESP_OK;
static TaskHandle_t s_job_task;
static uint32_t s_job_id;
static job_kind_t s_job_kind;
static screen_upload_report_t s_upload_report;
static const char *s_job_step = "idle";
static size_t s_job_bytes;
typedef struct {
    httpd_req_t *req;
    size_t bytes;
    TickType_t started;
    uint32_t hash, expected;
    uint32_t id, base_generation, rotation_epoch;
    int slot;                              /* -1 = the next slot in the cycle */
} frame_rx_t;
static frame_rx_t s_pending;
typedef struct {
    char name[TF_NAME_MAX + 1];
    int slot;
} tf_pending_t;
static tf_pending_t s_tf_pending;
/* Last partial-window probe request/result, published under s_job_lock. */
static screen_probe_req_t s_probe_req;
static screen_probe_result_t s_probe_result;
static bool s_probe_ran;
static uint32_t s_probe_at_ms;

static void log_heap_stats(const char *event, uint32_t id)
{
    const uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t min8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    const uint32_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap %s id=%u free8=%u min8=%u largest8=%u internal=%u internal_largest=%u",
             event, (unsigned)id, (unsigned)free8, (unsigned)min8, (unsigned)largest8,
             (unsigned)free_internal, (unsigned)largest_internal);
}


static esp_err_t send_json(httpd_req_t *req, const char *json);
static void frame_job_task(void *arg);
static const char *job_kind_name(job_kind_t kind)
{
    switch (kind) {
    case JOB_CLEAR_SCREEN: return "clear-screen";
    case JOB_TF_IMPORT: return "tf-import";
    case JOB_TF_EXPORT: return "tf-export";
    case JOB_PANEL_PROBE: return "panel-probe";
    case JOB_CONTENT: return "content";
    default: return "frame";
    }
}
static const char *job_sequence(job_kind_t kind)
{
    switch (kind) {
    case JOB_CLEAR_SCREEN: return "otp6-clear-v1";
    case JOB_TF_IMPORT: return "tf-import-v1";
    case JOB_TF_EXPORT: return "tf-export-v1";
    case JOB_PANEL_PROBE: return "panel-probe-v1";
    case JOB_CONTENT: return "content-page-v1";
    default: return FRAME_FORMAT;
    }
}
static const char *job_phase_name(job_phase_t phase)
{
    switch (phase) {
    case JOB_REFRESH: return "refresh";
    case JOB_RECEIVING: return "receiving";
    case JOB_ERROR: return "error";
    default: return "idle";
    }
}

bool web_server_screen_busy(void){return s_job_phase == JOB_RECEIVING || s_job_phase == JOB_REFRESH;}
static bool begin_job(job_kind_t kind)
{
    portENTER_CRITICAL(&s_job_lock);
    bool available = !screen_rotation_busy() && (s_job_phase == JOB_IDLE || s_job_phase == JOB_ERROR);
    if (available) {
        if (++s_job_id == 0) ++s_job_id;
        s_job_kind = kind;
        s_job_err = ESP_OK;
        s_job_phase = JOB_RECEIVING;
        s_job_step = "queued";
        s_job_bytes = 0;
        memset(&s_upload_report, 0, sizeof(s_upload_report));
    }
    portEXIT_CRITICAL(&s_job_lock);
    return available;
}
static void finish_job(esp_err_t err, const screen_upload_report_t *report)
{
    portENTER_CRITICAL(&s_job_lock);
    s_upload_report = *report;
    s_job_err = err;
    s_job_phase = err == ESP_OK ? JOB_IDLE : JOB_ERROR;
    s_job_step = err == ESP_OK ? "done" : "error";
    portEXIT_CRITICAL(&s_job_lock);
}
static void set_job_progress(size_t bytes, const char *step)
{
    portENTER_CRITICAL(&s_job_lock);
    s_job_bytes = bytes;
    s_job_step = step;
    portEXIT_CRITICAL(&s_job_lock);
}
static void set_job_phase(job_phase_t phase, const char *step)
{
    portENTER_CRITICAL(&s_job_lock);
    s_job_phase = phase;
    s_job_step = step;
    portEXIT_CRITICAL(&s_job_lock);
}

/* Append one contract to both job polling and the asynchronous OTP result.
 * Zero hashes mean no complete successful source transmission was recorded. */
static esp_err_t send_report_json(httpd_req_t *req, char *json, size_t capacity, int used,
                                  const screen_upload_report_t *report, const char *sequence)
{
    if (used < 0 || (size_t)used >= capacity) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "job JSON overflow");
        return ESP_FAIL;
    }
    const int written = snprintf(json + used, capacity - (size_t)used,
        "\"sequence\":\"%s\",\"plane_fnv\":[\"%08x\"],\"completed_mask\":%u}",
        sequence, (unsigned)report->frame_fnv, report->completed ? 1U : 0U);
    if (written < 0 || (size_t)written >= capacity - (size_t)used) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "job JSON overflow");
        return ESP_FAIL;
    }
    return send_json(req, json);
}
static esp_err_t send_job_json(httpd_req_t *req)
{
    char json[512];
    portENTER_CRITICAL(&s_job_lock);
    const uint32_t id = s_job_id;
    const job_phase_t phase = s_job_phase;
    const esp_err_t err = s_job_err;
    const job_kind_t kind = s_job_kind;
    const char *step = s_job_step;
    const size_t bytes = s_job_bytes;
    const screen_upload_report_t report = s_upload_report;
    portEXIT_CRITICAL(&s_job_lock);
    const int used = snprintf(json, sizeof(json),
        "{\"ok\":true,\"id\":%u,\"phase\":\"%s\",\"err\":%d,\"kind\":\"%s\","
        "\"step\":\"%s\",\"bytes\":%u,\"total_ms\":%u,\"refresh_ms\":%u,"
        "\"display_generation\":%u,\"current_display_generation\":%u,",
        (unsigned)id, job_phase_name(phase), (int)err, job_kind_name(kind), step,
        (unsigned)bytes, (unsigned)report.total_ms, (unsigned)report.refresh_ms,
        (unsigned)report.display_generation, (unsigned)screen_driver_generation());
    return send_report_json(req, json, sizeof(json), used, &report, job_sequence(kind));
}

extern const uint8_t index_html_start[]        asm("_binary_index_html_start");
extern const uint8_t index_html_end[]          asm("_binary_index_html_end");
extern const uint8_t pipeline_js_start[]       asm("_binary_eink_pipeline_js_start");
extern const uint8_t pipeline_js_end[]         asm("_binary_eink_pipeline_js_end");
extern const uint8_t vue_js_start[]            asm("_binary_vue_global_prod_js_start");
extern const uint8_t vue_js_end[]              asm("_binary_vue_global_prod_js_end");

static esp_err_t send_embedded(httpd_req_t *req, const uint8_t *start, const uint8_t *end,
                               const char *type, const char *cache)
{
    const size_t len = (size_t)(end - start);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", cache ? cache : "no-cache");
    /* A failed response must propagate so IDF can retire the bad session. */
    return httpd_resp_send(req, (const char *)start, len);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    /* Every API reply is a sign of life: it postpones the low power countdown. */
    power_mgr_note_activity();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP response failed uri=%s fd=%d err=%s", req->uri,
                 httpd_req_to_sockfd(req), esp_err_to_name(err));
    }
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    power_mgr_note_activity();
    return send_embedded(req, index_html_start, index_html_end, "text/html; charset=utf-8", "no-cache");
}

static esp_err_t vue_get_handler(httpd_req_t *req)
{
    return send_embedded(req, vue_js_start, vue_js_end,
                         "application/javascript; charset=utf-8", "public, max-age=86400");
}

static esp_err_t pipeline_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)pipeline_js_start,
                                        pipeline_js_end - pipeline_js_start);
    if (err != ESP_OK) return err;
    char config[64];
    int n = snprintf(config, sizeof(config), ";EinkPipeline.setPanelRotation(%u);", screen_rotation_get());
    if (n < 0 || (size_t)n >= sizeof(config)) return ESP_FAIL;
    err = httpd_resp_send_chunk(req, config, n);
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void escape_ssid(const char *src, char *dst)
{
    static const char hex[] = "0123456789abcdef";
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        if (c < 32 || c == 34 || c == 92) {
            *dst++ = 92; *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
            *dst++ = hex[c >> 4]; *dst++ = hex[c & 15];
        } else *dst++ = (char)c;
    }
    *dst = 0;
}

static size_t append_charge_json(char *json, size_t capacity, size_t used);

/* Lets the page distinguish a light-sleep wake (uptime keeps running) from a
 * power cycle or panic (uptime restarts, reset_reason names the cause). */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
    }
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ip[16] = { 0 };
    char ssid[33] = { 0 };
    char escaped_ssid[193];
    char url[48] = { 0 };
    char json[1024];
    wifi_mgr_get_ip(ip, sizeof(ip));
    wifi_mgr_get_ssid(ssid, sizeof(ssid));
    wifi_mgr_service_url(url, sizeof(url));
    escape_ssid(ssid, escaped_ssid);
    int n = snprintf(json, sizeof(json),
        "{\"ok\":true,\"format\":\"otp6-v1\",\"w\":480,\"h\":720,\"frame\":172800,"
        "\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"url\":\"%s\"",
        wifi_mgr_is_sta_up() ? "sta" : "ap", ip, escaped_ssid, url);
    if (n < 0 || (size_t)n >= sizeof(json)) return ESP_FAIL;
    size_t used = append_charge_json(json, sizeof(json), (size_t)n);
    if (used + 160 >= sizeof(json)) return ESP_FAIL;
    int tail = snprintf(json + used, sizeof(json) - used,
        ",\"images\":%u,\"slots\":%d,\"current\":%d,"
        "\"job\":\"%s\",\"job_id\":%u,\"job_err\":%d,\"rotation\":%u,\"rotation_epoch\":%u,"
        "\"rotation_busy\":%s,\"uptime_ms\":%llu,\"reset_reason\":\"%s\"}",
        screen_rotation_image_count(), SLOT_COUNT, screen_rotation_current(),
        job_phase_name(s_job_phase), (unsigned)s_job_id, (int)s_job_err, screen_rotation_get(),
        (unsigned)screen_rotation_epoch(), screen_rotation_busy() ? "true" : "false",
        (unsigned long long)(esp_timer_get_time() / 1000),
        reset_reason_name(esp_reset_reason()));
    if (tail < 0 || (size_t)tail >= sizeof(json) - used) return ESP_FAIL;
    return send_json(req, json);
}




static size_t append_charge_json(char *json, size_t capacity, size_t used)
{
    axp2101_chg_pwr_t pwr = axp2101_get_charge_power();
    bool enabled = false, present = false;
    int soc = -1;
    bool valid = axp2101_gauge_status(&enabled, &present, &soc) == ESP_OK;
    int n = snprintf(json + used, capacity - used,
        ",\"charge\":\"%s\",\"name\":\"%s\",\"capacity_mah\":%u,\"target_ma\":%u,\"icc_ma\":%u,\"vbus_lim_ma\":%u,\"gauge_read_ok\":%s,\"gauge_enabled\":%s,\"battery_present\":%s,\"soc\":%d,\"learning_completion_available\":false",
        axp2101_charge_power_id(pwr), axp2101_charge_power_name(pwr),
        (unsigned)axp2101_capacity_mah(), (unsigned)axp2101_charge_target_ma(pwr),
        (unsigned)axp2101_charge_icc_ma(pwr), (unsigned)axp2101_charge_vbus_lim_ma(pwr),
        valid ? "true" : "false", enabled ? "true" : "false", present ? "true" : "false", soc);
    if (n < 0 || (size_t)n >= capacity - used) return capacity;
    return used + (size_t)n;
}

static void send_charge_json(httpd_req_t *req)
{
    char json[384] = "{\"ok\":true";
    size_t used = append_charge_json(json, sizeof(json), strlen(json));
    if (used + 2 >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "charge response overflow");
        return;
    }
    json[used++] = '}';
    json[used] = 0;
    (void)send_json(req, json);
}

static esp_err_t charge_get_handler(httpd_req_t *req)
{
    send_charge_json(req);
    return ESP_OK;
}

static esp_err_t charge_post_handler(httpd_req_t *req)
{
    char body[128];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    size_t used = 0;
    while (used < req->content_len) {
        int n = httpd_req_recv(req, body + used, req->content_len - used);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete charge request");
            return ESP_FAIL;
        }
        used += (size_t)n;
    }
    body[used] = 0;
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "power");
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(root, "capacity_mah");
    int pwr = -1;
    if (cJSON_IsString(mode)) {
        if (!strcmp(mode->valuestring, "slow") || !strcmp(mode->valuestring, "1w") || !strcmp(mode->valuestring, "1")) pwr = 0;
        if (!strcmp(mode->valuestring, "balanced") || !strcmp(mode->valuestring, "2.5w") || !strcmp(mode->valuestring, "2.5")) pwr = 1;
        if (!strcmp(mode->valuestring, "fast") || !strcmp(mode->valuestring, "5w") || !strcmp(mode->valuestring, "5")) pwr = 2;
    }
    double capacity = cap && cJSON_IsNumber(cap) ? cap->valuedouble : axp2101_capacity_mah();
    bool valid = cJSON_IsObject(root) && pwr >= 0 && (!cap || cJSON_IsNumber(cap)) &&
                 capacity >= 500 && capacity <= 10000 && capacity == (uint16_t)capacity;
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "power or capacity_mah (integer 500..10000)");
        return ESP_FAIL;
    }
    esp_err_t err = axp2101_set_charge_config((axp2101_chg_pwr_t)pwr, (uint16_t)capacity);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    send_charge_json(req);
    return ESP_OK;
}

/* Sleep policy and the boot-time wake facts; the page shows the same values so
 * a degraded (timer-only) wake is visible instead of guessed. */
static void send_power_json(httpd_req_t *req)
{
    char json[384];
    const power_mgr_config_t cfg = power_mgr_config();
    const int soc = axp2101_battery_percent();
    const int n = snprintf(json, sizeof(json),
        "{\"ok\":true,\"mode\":\"%s\",\"interval_min\":%u,\"deep_ok\":%s,"
        "\"wake_pins\":{\"25\":%s,\"4\":%s,\"28\":%s,\"23\":%s},"
        "\"last_wake\":\"%s\",\"usb\":%s,\"batt\":%d,\"sleep_state\":\"%s\"}",
        power_mgr_mode_name(cfg.mode), (unsigned)cfg.interval_min,
        power_mgr_deep_ok() ? "true" : "false",
        power_mgr_wake_pin_valid(25) ? "true" : "false",
        power_mgr_wake_pin_valid(4) ? "true" : "false",
        power_mgr_wake_pin_valid(28) ? "true" : "false",
        power_mgr_wake_pin_valid(23) ? "true" : "false",
        power_mgr_last_wake_name(),
        axp2101_is_usb_connected() ? "true" : "false",
        soc, power_mgr_state_name());
    if (n < 0 || (size_t)n >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "power response overflow");
        return;
    }
    (void)send_json(req, json);
}

static esp_err_t power_get_handler(httpd_req_t *req)
{
    send_power_json(req);
    return ESP_OK;
}

static esp_err_t power_post_handler(httpd_req_t *req)
{
    char body[128];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    size_t used = 0;
    while (used < req->content_len) {
        int n = httpd_req_recv(req, body + used, req->content_len - used);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete power request");
            return ESP_FAIL;
        }
        used += (size_t)n;
    }
    body[used] = 0;

    const power_mgr_config_t cur = power_mgr_config();
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "interval_min");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    int pwr_mode = (int)cur.mode;
    if (mode) {
        pwr_mode = -1;
        if (cJSON_IsString(mode)) {
            if (!strcmp(mode->valuestring, "auto")) pwr_mode = POWER_MODE_AUTO;
            else if (!strcmp(mode->valuestring, "light")) pwr_mode = POWER_MODE_LIGHT;
            else if (!strcmp(mode->valuestring, "deep")) pwr_mode = POWER_MODE_DEEP;
        }
    }
    const double interval_min = interval && cJSON_IsNumber(interval)
                                    ? interval->valuedouble : (double)cur.interval_min;
    const bool valid = cJSON_IsObject(root) && pwr_mode >= 0 &&
        (!interval || cJSON_IsNumber(interval)) &&
        interval_min >= 0 && interval_min <= POWER_INTERVAL_MAX_MIN &&
        interval_min == (uint16_t)interval_min;
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "mode auto|light|deep, interval_min 0..10080");
        return ESP_FAIL;
    }
    esp_err_t err = power_mgr_set_config((power_mgr_mode_t)pwr_mode, (uint16_t)interval_min);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    send_power_json(req);
    return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    static char json[2048];
    (void)wifi_mgr_scan_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    char body[256];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi request size");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete wifi request");
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    body[received] = 0;
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *ssid_json = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass_json = cJSON_GetObjectItemCaseSensitive(root, "password");
    const bool valid = cJSON_IsObject(root) && cJSON_IsString(ssid_json) &&
        ssid_json->valuestring[0] && strlen(ssid_json->valuestring) <= 32 &&
        (!pass_json || (cJSON_IsString(pass_json) && strlen(pass_json->valuestring) <= 64));
    if (!valid) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid or password");
        return ESP_FAIL;
    }
    char ssid[33], pass[65] = {0};
    memcpy(ssid, ssid_json->valuestring, strlen(ssid_json->valuestring) + 1);
    if (pass_json) memcpy(pass, pass_json->valuestring, strlen(pass_json->valuestring) + 1);
    cJSON_Delete(root);

    esp_err_t err = wifi_mgr_connect(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    send_json(req, "{\"ok\":true,\"msg\":\"connecting\"}");
    return ESP_OK;
}

/* Validate each six-ink chunk before writing it to controller RAM. */
static esp_err_t receive_frame_chunk(void *context, uint8_t *buffer,
                                     size_t capacity, size_t *received)
{
    frame_rx_t *rx = context;
    *received = 0;
    if (rx->bytes >= SCREEN_FRAME_BYTES) return ESP_ERR_INVALID_SIZE;
    if (xTaskGetTickCount() - rx->started > pdMS_TO_TICKS(60000)) return ESP_ERR_TIMEOUT;
    size_t remaining = SCREEN_FRAME_BYTES - rx->bytes;
    if (capacity > remaining) capacity = remaining;
    int ret = HTTPD_SOCK_ERR_TIMEOUT;
    for (unsigned attempt = 0; attempt < 3 && ret == HTTPD_SOCK_ERR_TIMEOUT; ++attempt) {
        ret = httpd_req_recv(rx->req, (char *)buffer, capacity);
    }
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) return ESP_ERR_TIMEOUT;
    if (ret <= 0) return ESP_FAIL;
    for (int i = 0; i < ret; ++i) {
        uint8_t hi = buffer[i] >> 4, lo = buffer[i] & 15;
        bool valid = ((hi <= 3 || hi == 5 || hi == 6) && (lo <= 3 || lo == 5 || lo == 6));
        if (!valid) return ESP_ERR_INVALID_ARG;
        rx->hash = (rx->hash ^ buffer[i]) * 16777619U;
    }
    (void)screen_rotation_capture_write(rx->bytes, buffer, ret);
    rx->bytes += ret;
    portENTER_CRITICAL(&s_job_lock);
    s_job_bytes = rx->bytes;
    portEXIT_CRITICAL(&s_job_lock);
    if (rx->bytes == SCREEN_FRAME_BYTES) {
        ESP_LOGI(TAG, "frame check id=%u bytes=%u hash=%08x expected=%08x",
                 (unsigned)rx->id, (unsigned)rx->bytes, (unsigned)rx->hash,
                 (unsigned)rx->expected);
        if (rx->hash != rx->expected) return ESP_ERR_INVALID_CRC;
    }
    *received = ret;
    return ESP_OK;
}
static esp_err_t upload_progress(void *context, screen_upload_phase_t phase)
{
    frame_rx_t *rx = context;
    const bool receiving = phase == SCREEN_UPLOAD_RECEIVING;
    if (receiving) rx->started = xTaskGetTickCount();
    portENTER_CRITICAL(&s_job_lock);
    s_job_phase = receiving ? JOB_RECEIVING : JOB_REFRESH;
    s_job_step = receiving ? "receiving" : "refresh";
    portEXIT_CRITICAL(&s_job_lock);
    ESP_LOGI(TAG, "frame id=%u step=%s", (unsigned)rx->id, receiving ? "receiving" : "refresh");
    return ESP_OK;
}

/* Draws the awake IP strip over the bytes on their way to the controller. The
 * stored copy and the hash evidence keep the clean frame, so the low power
 * replay shows the picture alone. Runs with the rotation lock held. */
static void ip_strip_decorate(void *context, uint8_t *buffer, size_t offset, size_t length)
{
    (void)context;
    const char *text = screen_rotation_overlay();
    if (!text[0] || !length) return;
    const unsigned rotation = screen_rotation_get();
    for (size_t i = 0; i < length; ++i) {
        const size_t pos = offset + i;
        const int px = SCREEN_WIDTH - 1 - (int)(pos / (SCREEN_HEIGHT / 2));
        uint8_t hi = buffer[i] >> 4, lo = buffer[i] & 15, ink;
        int x, y;
        int py = (int)(pos % (SCREEN_HEIGHT / 2)) * 2;
        screen_rotation_source_xy(rotation, px, py, &x, &y);
        if (screen_ui_overlay_pixel(text, rotation, x, y, &ink)) hi = ink;
        screen_rotation_source_xy(rotation, px, py + 1, &x, &y);
        if (screen_ui_overlay_pixel(text, rotation, x, y, &ink)) lo = ink;
        buffer[i] = (uint8_t)((hi << 4) | lo);
    }
}

/* ---- TF content library -------------------------------------------------- */

static bool tf_import_name_ok(const char *name)
{
    const size_t len = name ? strlen(name) : 0;
    if (len == 0 || len > TF_NAME_MAX) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    /* Only the export side restricts the alphabet; an import may use any name
     * already on the card as long as it cannot escape /sdcard/eink. */
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static bool tf_export_name_ok(const char *name)
{
    const size_t len = name ? strlen(name) : 0;
    if (len == 0 || len > TF_NAME_MAX) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (size_t i = 0; i < len; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

static bool tf_path(char *dst, size_t capacity, const char *name)
{
    const size_t dir = strlen(TF_CARD_EINK_DIR);
    const size_t len = name ? strlen(name) : 0;
    if (!name || len == 0 || dir + 1 + len + 1 > capacity) return false;
    memcpy(dst, TF_CARD_EINK_DIR, dir);
    dst[dir] = '/';
    memcpy(dst + dir + 1, name, len + 1);
    return true;
}

static esp_err_t read_json_body(httpd_req_t *req, char *body, size_t capacity)
{
    if (req->content_len == 0 || req->content_len >= capacity) return ESP_ERR_INVALID_SIZE;
    size_t used = 0;
    while (used < req->content_len) {
        const int n = httpd_req_recv(req, body + used, req->content_len - used);
        if (n <= 0) return ESP_FAIL;
        used += (size_t)n;
    }
    body[used] = 0;
    return ESP_OK;
}

static int append_lit(char *dst, size_t capacity, int used, const char *lit)
{
    if (used < 0) return -1;
    const size_t n = strlen(lit);
    if ((size_t)used + n + 1 > capacity) return -1;
    memcpy(dst + used, lit, n + 1);
    return used + (int)n;
}

static int append_json_name(char *dst, size_t capacity, int used, const char *src)
{
    if (used < 0 || (size_t)used + 2 > capacity) return -1;
    dst[used++] = '"';
    for (const char *p = src; *p; ++p) {
        char out[2] = { *p, 0 };
        if (*p == '"' || *p == '\\') {
            out[0] = '\\';
            out[1] = *p;
        } else if ((unsigned char)*p < 0x20) {
            out[0] = '_';
            out[1] = 0;
        }
        const size_t n = strlen(out);
        if ((size_t)used + n + 2 > capacity) return -1;
        memcpy(dst + used, out, n);
        used += (int)n;
    }
    dst[used++] = '"';
    dst[used] = 0;
    return used;
}

typedef struct {
    char name[TF_NAME_MAX + 1];
    unsigned size;
    bool valid;
} tf_entry_t;

static tf_entry_t s_tf_entries[TF_LIST_MAX];
static char s_tf_json[TF_JSON_BYTES];
static uint8_t s_tf_chunk[TF_CHUNK_BYTES];

static int tf_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const tf_entry_t *)a)->name, ((const tf_entry_t *)b)->name);
}

static esp_err_t tf_get_handler(httpd_req_t *req)
{
    power_mgr_note_activity();
    if (web_server_screen_busy() || screen_rotation_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    if (tf_card_ensure_mounted() != ESP_OK) {
        return send_json(req, "{\"ok\":true,\"present\":false,\"mounted\":false,\"files\":[]}");
    }

    unsigned count = 0;
    (void)mkdir(TF_CARD_EINK_DIR, 0777);
    DIR *dir = opendir(TF_CARD_EINK_DIR);
    if (dir != NULL) {
        struct dirent *ent;
        while (count < TF_LIST_MAX && (ent = readdir(dir)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            const size_t len = strlen(ent->d_name);
            if (len == 0 || len > TF_NAME_MAX) continue;
            char path[sizeof(TF_CARD_EINK_DIR) + TF_NAME_MAX + 2];
            if (!tf_path(path, sizeof(path), ent->d_name)) continue;
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            tf_entry_t *e = &s_tf_entries[count];
            memcpy(e->name, ent->d_name, len + 1);
            e->size = (unsigned)st.st_size;
            e->valid = st.st_size == SCREEN_FRAME_BYTES;
            ++count;
        }
        closedir(dir);
    }
    qsort(s_tf_entries, count, sizeof(s_tf_entries[0]), tf_entry_cmp);

    int used = append_lit(s_tf_json, sizeof(s_tf_json), 0,
        "{\"ok\":true,\"present\":true,\"mounted\":true,\"files\":[");
    for (unsigned i = 0; i < count && used >= 0; ++i) {
        char size_text[16];
        const int n = snprintf(size_text, sizeof(size_text), "%u", s_tf_entries[i].size);
        if (n < 0 || (size_t)n >= sizeof(size_text)) { used = -1; break; }
        if (i) used = append_lit(s_tf_json, sizeof(s_tf_json), used, ",");
        used = append_lit(s_tf_json, sizeof(s_tf_json), used, "{\"name\":");
        used = append_json_name(s_tf_json, sizeof(s_tf_json), used, s_tf_entries[i].name);
        used = append_lit(s_tf_json, sizeof(s_tf_json), used, ",\"size\":");
        used = append_lit(s_tf_json, sizeof(s_tf_json), used, size_text);
        used = append_lit(s_tf_json, sizeof(s_tf_json), used, ",\"valid\":");
        used = append_lit(s_tf_json, sizeof(s_tf_json), used,
                          s_tf_entries[i].valid ? "true" : "false");
        used = append_lit(s_tf_json, sizeof(s_tf_json), used, "}");
    }
    if (used >= 0) used = append_lit(s_tf_json, sizeof(s_tf_json), used, "]}");
    if (used < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "tf list overflow");
        return ESP_FAIL;
    }
    return send_json(req, s_tf_json);
}

static esp_err_t tf_import_post_handler(httpd_req_t *req)
{
    power_mgr_note_activity();
    char body[192];
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(root, "file");
    const cJSON *slot = cJSON_GetObjectItemCaseSensitive(root, "slot");
    const bool valid = cJSON_IsObject(root) && cJSON_IsString(file) &&
        tf_import_name_ok(file->valuestring) && cJSON_IsNumber(slot) &&
        slot->valueint >= 0 && slot->valueint < SLOT_COUNT &&
        (double)slot->valueint == slot->valuedouble;
    char name[TF_NAME_MAX + 1];
    int target = 0;
    if (valid) {
        memcpy(name, file->valuestring, strlen(file->valuestring) + 1);
        target = slot->valueint;
    }
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file and slot 0..2 required");
        return ESP_FAIL;
    }
    if (web_server_screen_busy() || screen_rotation_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    if (tf_card_ensure_mounted() != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"no card\"}");
    }

    char path[sizeof(TF_CARD_EINK_DIR) + TF_NAME_MAX + 2];
    struct stat st;
    if (!tf_path(path, sizeof(path), name) || stat(path, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_size != SCREEN_FRAME_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a 172800-byte frame");
        return ESP_FAIL;
    }
    if (!begin_job(JOB_TF_IMPORT)) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    memcpy(s_tf_pending.name, name, strlen(name) + 1);
    s_tf_pending.slot = target;
    xTaskNotifyGive(s_job_task);
    httpd_resp_set_status(req, "202 Accepted");
    return send_job_json(req);
}

static esp_err_t tf_export_post_handler(httpd_req_t *req)
{
    power_mgr_note_activity();
    char body[192];
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *slot = cJSON_GetObjectItemCaseSensitive(root, "slot");
    const cJSON *namej = cJSON_GetObjectItemCaseSensitive(root, "name");
    const bool valid = cJSON_IsObject(root) && cJSON_IsNumber(slot) &&
        slot->valueint >= 0 && slot->valueint < SLOT_COUNT &&
        (double)slot->valueint == slot->valuedouble &&
        cJSON_IsString(namej) && tf_export_name_ok(namej->valuestring);
    char name[TF_NAME_MAX + 1];
    int target = 0;
    if (valid) {
        memcpy(name, namej->valuestring, strlen(namej->valuestring) + 1);
        target = slot->valueint;
    }
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot 0..2 and safe name required");
        return ESP_FAIL;
    }
    if (web_server_screen_busy() || screen_rotation_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    if (tf_card_ensure_mounted() != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"no card\"}");
    }
    screen_rotation_lock();
    const bool have = screen_rotation_slot_info(target, NULL);
    screen_rotation_unlock();
    if (!have) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot empty");
        return ESP_FAIL;
    }
    if (!begin_job(JOB_TF_EXPORT)) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    memcpy(s_tf_pending.name, name, strlen(name) + 1);
    s_tf_pending.slot = target;
    xTaskNotifyGive(s_job_task);
    httpd_resp_set_status(req, "202 Accepted");
    return send_job_json(req);
}

/* Both runners are called by the job task with the rotation lock held, so a
 * 23 s refresh and a flash/FAT read never interleave. */
static esp_err_t tf_job_import(const tf_pending_t *p)
{
    if (!tf_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    char path[sizeof(TF_CARD_EINK_DIR) + TF_NAME_MAX + 2];
    if (!tf_path(path, sizeof(path), p->name)) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return ESP_ERR_NOT_FOUND;

    const uint8_t blank[3] = { SCREEN_COLOR_WHITE, SCREEN_COLOR_WHITE, SCREEN_COLOR_WHITE };
    esp_err_t err = screen_rotation_capture_begin(SCREEN_CACHE_OTP6, blank, p->slot);
    if (err != ESP_OK) {
        fclose(f);
        return err;
    }

    size_t total = 0;
    while (total < SCREEN_FRAME_BYTES) {
        size_t want = SCREEN_FRAME_BYTES - total;
        if (want > sizeof(s_tf_chunk)) want = sizeof(s_tf_chunk);
        const size_t got = fread(s_tf_chunk, 1, want, f);
        if (got == 0) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        bool inks_ok = true;
        for (size_t i = 0; i < got; ++i) {
            const uint8_t hi = s_tf_chunk[i] >> 4, lo = s_tf_chunk[i] & 15;
            if (!((hi <= 3 || hi == 5 || hi == 6) && (lo <= 3 || lo == 5 || lo == 6))) {
                inks_ok = false;
                break;
            }
        }
        if (!inks_ok) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = screen_rotation_capture_write(total, s_tf_chunk, got);
        if (err != ESP_OK) break;
        total += got;
        set_job_progress(total, "import");
    }
    fclose(f);
    if (err != ESP_OK || total != SCREEN_FRAME_BYTES) {
        screen_rotation_capture_end(false, 0);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }

    /* The slot header is written first. capture_end marks the slot current even
     * though nothing was drawn yet, so drop the on-glass bookkeeping before the
     * replay; otherwise show_image() would think the frame is already there. */
    screen_rotation_capture_end(true, screen_driver_generation());
    screen_rotation_note_external();
    set_job_phase(JOB_REFRESH, "refresh");
    ESP_LOGI(TAG, "tf import %s -> slot %d", p->name, p->slot);
    return screen_rotation_show_image();
}

static esp_err_t tf_job_export(const tf_pending_t *p)
{
    if (!tf_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!screen_rotation_slot_info(p->slot, NULL)) return ESP_ERR_INVALID_STATE;
    (void)mkdir(TF_CARD_EINK_DIR, 0777);
    char path[sizeof(TF_CARD_EINK_DIR) + TF_NAME_MAX + 2];
    if (!tf_path(path, sizeof(path), p->name)) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "wb");
    if (f == NULL) return ESP_FAIL;

    esp_err_t err = ESP_OK;
    size_t total = 0;
    while (total < SCREEN_FRAME_BYTES) {
        size_t want = SCREEN_FRAME_BYTES - total;
        if (want > sizeof(s_tf_chunk)) want = sizeof(s_tf_chunk);
        err = screen_rotation_read_frame(p->slot, total, s_tf_chunk, want);
        if (err != ESP_OK) break;
        if (fwrite(s_tf_chunk, 1, want, f) != want) {
            err = ESP_FAIL;
            break;
        }
        total += want;
        set_job_progress(total, "export");
    }
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK && total != SCREEN_FRAME_BYTES) err = ESP_ERR_INVALID_SIZE;
    ESP_LOGI(TAG, "tf export slot %d -> %s (%u bytes, %s)",
             p->slot, p->name, (unsigned)total, esp_err_to_name(err));
    return err;
}

static void frame_job_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const job_kind_t kind = s_job_kind;
        if (kind == JOB_PANEL_PROBE) {
            screen_probe_req_t probe_req;
            portENTER_CRITICAL(&s_job_lock);
            probe_req = s_probe_req;
            portEXIT_CRITICAL(&s_job_lock);
            screen_probe_result_t probe_result;
            screen_upload_report_t probe_report = {0};
            const TickType_t probe_started = xTaskGetTickCount();
            screen_rotation_lock();
            const esp_err_t probe_err = screen_driver_partial_probe(&probe_req, &probe_result);
            const bool touched = probe_err != ESP_ERR_INVALID_ARG &&
                                 probe_err != ESP_ERR_NOT_SUPPORTED &&
                                 probe_err != ESP_ERR_INVALID_STATE;
            if (touched) {
                /* The probe already repainted a white full frame. Put the
                 * stored picture back when one exists; otherwise keep it blank. */
                screen_rotation_note_external();
                if (screen_rotation_has_images()) {
                    const esp_err_t replay = screen_rotation_show_image();
                    if (replay != ESP_OK) {
                        ESP_LOGW(TAG, "probe replay: %s", esp_err_to_name(replay));
                    }
                } else {
                    screen_rotation_note_blank();
                }
            }
            screen_rotation_unlock();
            portENTER_CRITICAL(&s_job_lock);
            s_probe_result = probe_result;
            s_probe_ran = true;
            s_probe_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
            portEXIT_CRITICAL(&s_job_lock);
            probe_report.total_ms = (uint32_t)((xTaskGetTickCount() - probe_started) * portTICK_PERIOD_MS);
            probe_report.refresh_ms = probe_result.drf_ms;
            probe_report.completed = probe_err == ESP_OK;
            probe_report.display_generation = screen_driver_generation();
            finish_job(probe_err, &probe_report);
            ESP_LOGI(TAG, "job panel-probe %s", esp_err_to_name(probe_err));
            continue;
        }
        if (kind == JOB_CONTENT) {
            screen_upload_report_t content_report = {0};
            const TickType_t content_started = xTaskGetTickCount();
            screen_rotation_lock();
            const bool drawn = content_page_redraw(true);
            screen_rotation_unlock();
            content_report.total_ms = (uint32_t)((xTaskGetTickCount() - content_started) *
                                                 portTICK_PERIOD_MS);
            content_report.refresh_ms = content_report.total_ms;
            content_report.completed = drawn;
            content_report.display_generation = screen_driver_generation();
            finish_job(drawn ? ESP_OK : ESP_FAIL, &content_report);
            ESP_LOGI(TAG, "job content %s (%u ms)",
                     esp_err_to_name(drawn ? ESP_OK : ESP_FAIL),
                     (unsigned)content_report.total_ms);
            continue;
        }
        if (kind == JOB_TF_IMPORT || kind == JOB_TF_EXPORT) {
            const tf_pending_t tf = s_tf_pending;
            screen_upload_report_t tf_report = {0};
            const TickType_t tf_started = xTaskGetTickCount();
            screen_rotation_lock();
            const esp_err_t tf_err = kind == JOB_TF_IMPORT ? tf_job_import(&tf) : tf_job_export(&tf);
            screen_rotation_unlock();
            tf_report.total_ms = (uint32_t)((xTaskGetTickCount() - tf_started) * portTICK_PERIOD_MS);
            if (tf_err == ESP_OK) {
                tf_report.completed = true;
                tf_report.display_generation = screen_driver_generation();
            }
            finish_job(tf_err, &tf_report);
            ESP_LOGI(TAG, "job %s %s", job_kind_name(kind), esp_err_to_name(tf_err));
            continue;
        }
        frame_rx_t rx = s_pending;
        screen_upload_report_t report = {0};
        esp_err_t err;
        screen_rotation_lock();
        if (kind == JOB_FRAME) {
            const uint8_t blank[3] = {1,1,1};
            (void)screen_rotation_capture_begin(SCREEN_CACHE_OTP6, blank, rx.slot);
            uint8_t *buffer = malloc(FRAME_CHUNK_BYTES);
            err = !screen_rotation_matches(rx.rotation_epoch) ? ESP_ERR_INVALID_STATE :
                !buffer ? ESP_ERR_NO_MEM : screen_driver_upload(rx.base_generation,
                buffer, FRAME_CHUNK_BYTES, receive_frame_chunk, &rx, upload_progress,
                ip_strip_decorate, NULL, &report);
            free(buffer);
            screen_rotation_capture_end(err == ESP_OK, report.display_generation);
            ESP_LOGI(TAG, "frame id=%u end %s bytes=%u",
                     (unsigned)rx.id, esp_err_to_name(err), (unsigned)rx.bytes);
            log_heap_stats("frame-end", rx.id);
            /* Publish the completed baseline before the browser receives its
             * receipt and can immediately verify or reuse that baseline. */
            finish_job(err, &report);
            if (err == ESP_OK) {
                char json[384];
                const int used = snprintf(json, sizeof(json),
                    "{\"ok\":true,\"id\":%u,\"phase\":\"idle\",\"err\":0,\"kind\":\"frame\","
                    "\"total_ms\":%u,\"refresh_ms\":%u,\"display_generation\":%u,",
                    (unsigned)rx.id, (unsigned)report.total_ms,
                    (unsigned)report.refresh_ms, (unsigned)report.display_generation);
                (void)send_report_json(rx.req, json, sizeof(json), used, &report, FRAME_FORMAT);
            } else {
                httpd_resp_send_err(rx.req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
                /* Discard any unread body before the async request releases its socket. */
                shutdown(httpd_req_to_sockfd(rx.req), SHUT_RDWR);
            }
            const esp_err_t complete = httpd_req_async_handler_complete(rx.req);
            if (complete != ESP_OK) ESP_LOGW(TAG, "async complete: %s", esp_err_to_name(complete));
        } else {
            screen_rotation_note_external();
            err = screen_driver_clear(&report);
            if (err == ESP_OK) screen_rotation_note_blank();
            finish_job(err, &report);
        }
        screen_rotation_unlock();
        ESP_LOGI(TAG, "job %s", err == ESP_OK ? "idle" : "error");
    }
}
static esp_err_t job_get_handler(httpd_req_t *req)
{
    return send_job_json(req);
}
static bool hex_header(httpd_req_t *req, const char *key, uint32_t *out)
{
    char value[9];
    if (httpd_req_get_hdr_value_str(req, key, value, sizeof(value)) != ESP_OK ||
        strlen(value) != 8 || strspn(value, "0123456789abcdefABCDEF") != 8) return false;
    *out = (uint32_t)strtoul(value, NULL, 16);
    return true;
}

static bool decimal_header(httpd_req_t *req, const char *key, uint32_t *out)
{
    char text[11];
    if (httpd_req_get_hdr_value_str(req, key, text, sizeof(text)) != ESP_OK || !text[0] ||
        strspn(text, "0123456789") != strlen(text)) return false;
    uint64_t value = strtoull(text, NULL, 10);
    if (value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static esp_err_t frame_post_handler(httpd_req_t *req)
{
    char format[16], pass[8], quality[12];
    frame_rx_t rx = { .hash = 2166136261U, .slot = -1 };
    power_mgr_note_activity();
    if (httpd_req_get_hdr_value_str(req, "X-Eink-Format", format, sizeof(format)) != ESP_OK ||
        strcmp(format, FRAME_FORMAT) != 0 ||
        httpd_req_get_hdr_value_str(req, "X-Eink-Pass", pass, sizeof(pass)) != ESP_OK ||
        strcmp(pass, "0") != 0 ||
        httpd_req_get_hdr_value_str(req, "X-Eink-Quality", quality, sizeof(quality)) != ESP_OK ||
        (strcmp(quality, "fast") != 0 && strcmp(quality, "standard") != 0 && strcmp(quality, "best") != 0) ||
        !hex_header(req, "X-Eink-Checksum", &rx.expected)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "frame contract mismatch; reload page");
        return ESP_FAIL;
    }
    if (req->content_len != SCREEN_FRAME_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected 172800 bytes");
        return ESP_FAIL;
    }
    if (httpd_req_get_hdr_value_len(req, "X-Eink-Slot")) {
        uint32_t slot = 0;
        if (!decimal_header(req, "X-Eink-Slot", &slot) || slot >= 3) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Eink-Slot must be 0..2");
            return ESP_FAIL;
        }
        rx.slot = (int)slot;
    }
    if (httpd_req_get_hdr_value_len(req, "X-Eink-Base-Generation") &&
        (!decimal_header(req, "X-Eink-Base-Generation", &rx.base_generation) ||
         !rx.base_generation || rx.base_generation != screen_driver_generation())) {
        httpd_resp_set_status(req, "409 Conflict");
        send_json(req, "{\"ok\":false,\"error\":\"region baseline changed; send a full image\"}");
        return ESP_FAIL;
    }
    if (!decimal_header(req, "X-Eink-Rotation-Epoch", &rx.rotation_epoch) ||
        !screen_rotation_matches(rx.rotation_epoch) || !begin_job(JOB_FRAME)) {
        httpd_resp_set_status(req, "409 Conflict");
        send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
        return ESP_FAIL;
    }
    esp_err_t err = httpd_req_async_handler_begin(req, &rx.req);
    if (err != ESP_OK) {
        const screen_upload_report_t empty = {0};
        finish_job(err, &empty);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    rx.id = s_job_id;
    s_pending = rx;
    ESP_LOGI(TAG, "frame id=%u format=%s quality=%s bytes=%u",
             (unsigned)rx.id, format, quality, (unsigned)req->content_len);
    log_heap_stats("frame-start", rx.id);
    /* The worker owns first byte through POF and response; HTTP stays available. */
    xTaskNotifyGive(s_job_task);
    return ESP_OK;
}

static esp_err_t panel_get_handler(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true,\"controller\":\"EPD037A02\","
        "\"image_formats\":[\"otp6-v1\"],\"colors\":6,\"full_refresh\":true}");
}

static esp_err_t clear_post_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected empty body");
        return ESP_FAIL;
    }
    if (!begin_job(JOB_CLEAR_SCREEN)) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    portENTER_CRITICAL(&s_job_lock);
    s_job_phase = JOB_REFRESH;
    s_job_step = "clear";
    portEXIT_CRITICAL(&s_job_lock);
    xTaskNotifyGive(s_job_task);
    httpd_resp_set_status(req, "202 Accepted");
    return send_job_json(req);
}

static uint8_t s_thumb[THUMB_BYTES];
static SemaphoreHandle_t s_thumb_mutex;

static esp_err_t slots_get_handler(httpd_req_t *req)
{
    bool valid[SLOT_COUNT];
    unsigned rotation[SLOT_COUNT];
    screen_rotation_lock();
    const int current = screen_rotation_current();
    for (int i = 0; i < SLOT_COUNT; ++i) {
        rotation[i] = 0;
        valid[i] = screen_rotation_slot_info(i, &rotation[i]);
    }
    screen_rotation_unlock();

    char json[192];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"count\":%d,\"current\":%d,\"slots\":[",
                     SLOT_COUNT, current);
    if (n < 0 || (size_t)n >= sizeof(json)) return ESP_FAIL;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        const int used = snprintf(json + n, sizeof(json) - (size_t)n,
            "%s{\"i\":%d,\"valid\":%s,\"rotation\":%u}", i ? "," : "",
            i, valid[i] ? "true" : "false", valid[i] ? rotation[i] : 0U);
        if (used < 0 || (size_t)used >= sizeof(json) - (size_t)n) return ESP_FAIL;
        n += used;
    }
    if ((size_t)n >= sizeof(json) - 3) return ESP_FAIL;
    json[n++] = ']';
    json[n++] = '}';
    json[n] = 0;
    return send_json(req, json);
}

/* Raw RGBA thumbnail (60x90) of one stored slot; the page paints it into a
 * canvas. Empty (removed or never written) slots answer 404. */
static esp_err_t slot_get_handler(httpd_req_t *req)
{
    char query[24], value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "i", value, sizeof(value)) != ESP_OK || !value[0] ||
        strspn(value, "0123456789") != strlen(value)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index ?i=0..2");
        return ESP_FAIL;
    }
    const uint32_t index = (uint32_t)strtoul(value, NULL, 10);
    if (index >= SLOT_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index ?i=0..2");
        return ESP_FAIL;
    }
    power_mgr_note_activity();
    if (screen_rotation_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    if (s_thumb_mutex) xSemaphoreTake(s_thumb_mutex, pdMS_TO_TICKS(4000));
    screen_rotation_lock();
    const esp_err_t err = screen_rotation_preview((int)index, s_thumb, THUMB_WIDTH, THUMB_HEIGHT);
    screen_rotation_unlock();
    if (err != ESP_OK) {
        if (s_thumb_mutex) xSemaphoreGive(s_thumb_mutex);
        httpd_resp_send_err(req, err == ESP_ERR_INVALID_STATE ? HTTPD_404_NOT_FOUND :
                            HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t sent = httpd_resp_send(req, (const char *)s_thumb, THUMB_BYTES);
    if (s_thumb_mutex) xSemaphoreGive(s_thumb_mutex);
    return sent;
}

/* Read-only accelerometer state: last sample, orientation, gesture counters
 * and whether GPIO4 would be armed as a deep sleep wake source. */
static esp_err_t imu_get_handler(httpd_req_t *req)
{
    imu_app_status_t st;
    imu_app_status(&st);
    char json[320];
    const int n = snprintf(json, sizeof(json),
        "{\"ok\":true,\"present\":%s,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,"
        "\"orientation\":\"%s\",\"events\":{\"flip\":%u,\"tap\":%u},"
        "\"int_ready\":%s,\"wake_armed\":%s}",
        st.present ? "true" : "false",
        (double)st.x_g, (double)st.y_g, (double)st.z_g,
        imu_app_orientation_name(st.orientation),
        (unsigned)st.flip_events, (unsigned)st.tap_events,
        st.int_ready ? "true" : "false",
        st.wake_armed ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "imu response overflow");
        return ESP_FAIL;
    }
    return send_json(req, json);
}

/* ---- Standby information page -------------------------------------------- */

static esp_err_t send_content_json(httpd_req_t *req)
{
    content_config_t cfg;
    (void)content_config_get(&cfg);
    content_status_t status;
    content_status_get(&status);

    char json[512];
    const int used = snprintf(json, sizeof(json),
        "{\"ok\":true,\"enabled\":%s,\"note\":[\"%.32s\",\"%.32s\"],"
        "\"countdown_date\":\"%.10s\",\"countdown_label\":\"%.16s\","
        "\"last_draw\":%u,\"last_fingerprint\":\"%08x\",\"refreshed\":%s}",
        cfg.enabled ? "true" : "false", cfg.note[0], cfg.note[1],
        cfg.countdown_date, cfg.countdown_label,
        (unsigned)status.drawn_at, (unsigned)status.fingerprint,
        status.refreshed ? "true" : "false");
    if (used < 0 || (size_t)used >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content JSON overflow");
        return ESP_FAIL;
    }
    return send_json(req, json);
}

static esp_err_t content_get_handler(httpd_req_t *req)
{
    return send_content_json(req);
}

static esp_err_t content_post_handler(httpd_req_t *req)
{
    power_mgr_note_activity();
    char body[384];
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    content_config_t cfg;
    (void)content_config_get(&cfg); /* defaults are usable if NVS is damaged */

    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    bool valid = cJSON_IsObject(root);

    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (enabled) {
        valid = valid && cJSON_IsBool(enabled);
        if (valid) cfg.enabled = cJSON_IsTrue(enabled);
    }

    const cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "note");
    if (notes) {
        if (!cJSON_IsArray(notes) || cJSON_GetArraySize(notes) > CONTENT_NOTE_LINES) {
            valid = false;
        } else {
            cfg.note[0][0] = '\0';
            cfg.note[1][0] = '\0';
            int index = 0;
            for (const cJSON *item = notes->child; item && valid; item = item->next) {
                if (!cJSON_IsString(item) ||
                    !content_config_note_ok(item->valuestring, CONTENT_NOTE_MAX)) {
                    valid = false;
                    break;
                }
                const size_t length = strlen(item->valuestring);
                memcpy(cfg.note[index], item->valuestring, length + 1);
                ++index;
            }
        }
    }

    const cJSON *date = cJSON_GetObjectItemCaseSensitive(root, "countdown_date");
    if (date) {
        valid = valid && cJSON_IsString(date) &&
                strlen(date->valuestring) <= CONTENT_DATE_MAX &&
                (date->valuestring[0] == '\0' ||
                 lunar_parse_date(date->valuestring, NULL, NULL, NULL));
        if (valid) strlcpy(cfg.countdown_date, date->valuestring, sizeof(cfg.countdown_date));
    }

    const cJSON *label = cJSON_GetObjectItemCaseSensitive(root, "countdown_label");
    if (label) {
        valid = valid && cJSON_IsString(label) &&
                content_config_note_ok(label->valuestring, CONTENT_LABEL_MAX);
        if (valid) strlcpy(cfg.countdown_label, label->valuestring,
                           sizeof(cfg.countdown_label));
    }
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "enabled, note[2] (32 ASCII), countdown_date, label (16 ASCII)");
        return ESP_FAIL;
    }
    const esp_err_t err = content_config_set(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return send_content_json(req);
}

/* A manual redraw always costs one full refresh. The job task owns the
 * rotation lock; HTTP only queues it and returns 202. */
static esp_err_t content_refresh_post_handler(httpd_req_t *req)
{
    power_mgr_note_activity();
    if (req->content_len != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected empty body");
        return ESP_FAIL;
    }
    if (web_server_screen_busy() || screen_rotation_busy() || !begin_job(JOB_CONTENT)) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    set_job_phase(JOB_REFRESH, "content");
    xTaskNotifyGive(s_job_task);
    httpd_resp_set_status(req, "202 Accepted");
    return send_job_json(req);
}

/* ---- Partial-window probe API (docs/panel_partial_probe.md) -------------- */

static const char *probe_mode_name(screen_probe_mode_t mode)
{
    return mode == SCREEN_PROBE_MODE_WRITE ? "write" : "refresh";
}

static esp_err_t probe_get_handler(httpd_req_t *req)
{
    screen_probe_result_t result;
    screen_probe_req_t probe;
    bool ran;
    uint32_t at_ms;
    portENTER_CRITICAL(&s_job_lock);
    result = s_probe_result;
    probe = s_probe_req;
    ran = s_probe_ran;
    at_ms = s_probe_at_ms;
    portEXIT_CRITICAL(&s_job_lock);

    char json[640];
    int n;
    if (!ran) {
        n = snprintf(json, sizeof(json), "{\"ok\":true,\"enabled\":%s,\"ran\":false}",
                     screen_driver_partial_probe_enabled() ? "true" : "false");
    } else {
        n = snprintf(json, sizeof(json),
            "{\"ok\":true,\"enabled\":%s,\"ran\":true,"
            "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"repeat\":%u,\"mode\":\"%s\","
            "\"write_ms\":%u,\"drf_ms\":%u,\"cleanup_ms\":%u,\"total_ms\":%u,"
            "\"bytes\":%u,\"repeats\":%u,\"busy_asserted\":%s,\"busy_failures\":%u,"
            "\"exit_partial\":%s,\"cleanup\":%s,\"rotation\":%u,"
            "\"gate\":[%u,%u],\"row\":[%u,%u],\"panel\":[%d,%d,%d,%d],"
            "\"err\":%d,\"err_name\":\"%s\",\"at_ms\":%u}",
            screen_driver_partial_probe_enabled() ? "true" : "false",
            probe.x, probe.y, probe.w, probe.h, (unsigned)probe.repeat,
            probe_mode_name(probe.mode),
            (unsigned)result.write_ms, (unsigned)result.drf_ms,
            (unsigned)result.cleanup_ms, (unsigned)result.total_ms,
            (unsigned)result.bytes, (unsigned)result.repeats_done,
            result.busy_asserted ? "true" : "false", (unsigned)result.busy_failures,
            result.exit_partial_ok ? "true" : "false", result.cleanup_ok ? "true" : "false",
            (unsigned)result.rotation, (unsigned)result.gate_start, (unsigned)result.gate_end,
            (unsigned)result.row_start, (unsigned)result.row_end,
            result.px0, result.py0, result.px1, result.py1,
            (int)result.err, esp_err_to_name(result.err), (unsigned)at_ms);
    }
    if (n < 0 || (size_t)n >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "probe JSON overflow");
        return ESP_FAIL;
    }
    return send_json(req, json);
}

static esp_err_t probe_post_handler(httpd_req_t *req)
{
    power_mgr_note_activity();
    char body[192];
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    bool valid = cJSON_IsObject(root);
    const cJSON *jx = valid ? cJSON_GetObjectItemCaseSensitive(root, "x") : NULL;
    const cJSON *jy = valid ? cJSON_GetObjectItemCaseSensitive(root, "y") : NULL;
    const cJSON *jw = valid ? cJSON_GetObjectItemCaseSensitive(root, "w") : NULL;
    const cJSON *jh = valid ? cJSON_GetObjectItemCaseSensitive(root, "h") : NULL;
    const cJSON *jr = valid ? cJSON_GetObjectItemCaseSensitive(root, "repeat") : NULL;
    const cJSON *jm = valid ? cJSON_GetObjectItemCaseSensitive(root, "mode") : NULL;

    int x = 0, y = 0, w = 0, h = 0, repeat = 1;
    screen_probe_mode_t mode = SCREEN_PROBE_MODE_REFRESH;
    const cJSON *ints[] = { jx, jy, jw, jh };
    int *values[] = { &x, &y, &w, &h };
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]) && valid; ++i) {
        valid = cJSON_IsNumber(ints[i]) &&
                (double)ints[i]->valueint == ints[i]->valuedouble;
        if (valid) *values[i] = ints[i]->valueint;
    }
    if (valid && jr) {
        valid = cJSON_IsNumber(jr) && (double)jr->valueint == jr->valuedouble;
        if (valid) repeat = jr->valueint;
    }
    if (valid && jm) {
        if (cJSON_IsString(jm) && !strcmp(jm->valuestring, "write")) {
            mode = SCREEN_PROBE_MODE_WRITE;
        } else if (cJSON_IsString(jm) && !strcmp(jm->valuestring, "refresh")) {
            mode = SCREEN_PROBE_MODE_REFRESH;
        } else {
            valid = false;
        }
    }
    int logical_w = 0, logical_h = 0;
    screen_rotation_size(screen_rotation_get(), &logical_w, &logical_h);
    valid = valid && w >= 8 && h >= 8 && repeat >= 1 && repeat <= 10 &&
            x >= 0 && y >= 0 && w <= logical_w && h <= logical_h &&
            x <= logical_w - w && y <= logical_h - h;
    const screen_probe_req_t probe = {
        .x = x, .y = y, .w = w, .h = h,
        .rotation = (uint16_t)screen_rotation_get(),
        .repeat = (uint8_t)repeat, .mode = mode,
    };
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "x,y,w,h inside rotation; w,h>=8; repeat 1..10; mode write|refresh");
        return ESP_FAIL;
    }
    if (!screen_driver_partial_probe_enabled()) {
        httpd_resp_set_status(req, "501 Not Implemented");
        return send_json(req, "{\"ok\":false,\"error\":\"partial probe compiled out; "
                              "build with SCREEN_PARTIAL_PROBE_ENABLE=1\"}");
    }
    if (web_server_screen_busy() || screen_rotation_busy() || !begin_job(JOB_PANEL_PROBE)) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    portENTER_CRITICAL(&s_job_lock);
    s_probe_req = probe;
    portEXIT_CRITICAL(&s_job_lock);
    set_job_phase(JOB_REFRESH, "probe");
    xTaskNotifyGive(s_job_task);
    httpd_resp_set_status(req, "202 Accepted");
    return send_job_json(req);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 28;
    config.stack_size = 12288;
    config.max_open_sockets = 7;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 8;
    config.lru_purge_enable = true;

    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    if (s_job_task == NULL &&
        xTaskCreate(frame_job_task, "eink_job", 4096, NULL, 5, &s_job_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");

    const httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/vue.global.prod.js", .method = HTTP_GET, .handler = vue_get_handler },
        { .uri = "/eink_pipeline.js", .method = HTTP_GET, .handler = pipeline_get_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler },
        { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_handler },
        { .uri = "/api/charge", .method = HTTP_GET, .handler = charge_get_handler },
        { .uri = "/api/charge", .method = HTTP_POST, .handler = charge_post_handler },
        { .uri = "/api/power", .method = HTTP_GET, .handler = power_get_handler },
        { .uri = "/api/power", .method = HTTP_POST, .handler = power_post_handler },
        { .uri = "/api/frame", .method = HTTP_POST, .handler = frame_post_handler },
        { .uri = "/api/job", .method = HTTP_GET, .handler = job_get_handler },
        { .uri = "/api/panel", .method = HTTP_GET, .handler = panel_get_handler },
        { .uri = "/api/panel/probe", .method = HTTP_GET, .handler = probe_get_handler },
        { .uri = "/api/panel/probe", .method = HTTP_POST, .handler = probe_post_handler },
        { .uri = "/api/clear", .method = HTTP_POST, .handler = clear_post_handler },
        { .uri = "/api/slots", .method = HTTP_GET, .handler = slots_get_handler },
        { .uri = "/api/slot", .method = HTTP_GET, .handler = slot_get_handler },
        { .uri = "/api/imu", .method = HTTP_GET, .handler = imu_get_handler },
        { .uri = "/api/content", .method = HTTP_GET, .handler = content_get_handler },
        { .uri = "/api/content", .method = HTTP_POST, .handler = content_post_handler },
        { .uri = "/api/content/refresh", .method = HTTP_POST, .handler = content_refresh_post_handler },
        { .uri = "/api/tf", .method = HTTP_GET, .handler = tf_get_handler },
        { .uri = "/api/tf/import", .method = HTTP_POST, .handler = tf_import_post_handler },
        { .uri = "/api/tf/export", .method = HTTP_POST, .handler = tf_export_post_handler },
    };
    if (s_thumb_mutex == NULL) s_thumb_mutex = xSemaphoreCreateMutex();
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &handlers[i]), TAG, "register URI");
    }

    ESP_LOGI(TAG, "HTTP ready on :80");
    return ESP_OK;
}
