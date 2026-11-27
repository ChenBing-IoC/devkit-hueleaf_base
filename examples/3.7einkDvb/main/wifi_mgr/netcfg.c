#include "netcfg.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pin_map.h"

static const char *TAG = "netcfg";

#define KEY_AP_PASS     "ap_pass"
#define KEY_TOKEN       "token"
#define AP_PASS_PREFIX  "hueleaf-"
#define TOKEN_MAX       64
#define AP_PASS_MIN     8
#define AP_PASS_MAX     63
#define BOOT_HOLD_MS    5000
#define BOOT_POLL_MS    50

/* wifi_mgr stores the STA credentials in its own namespace; the factory reset
 * erases them here because it runs before wifi_mgr_init() loads them. */
#define WIFI_NS         "wifi_cfg"
#define WIFI_KEY_SSID   "ssid"
#define WIFI_KEY_PASS   "pass"

static SemaphoreHandle_t s_lock;
static bool s_ready;
static char s_ap_pass[AP_PASS_MAX + 1];
static char s_token[TOKEN_MAX + 1];
static char s_suffix[5];
static char s_host[32];

static void lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

bool netcfg_text_ok(const char *text, size_t min_len, size_t max_len)
{
    const size_t len = text ? strlen(text) : 0;
    if (len < min_len || len > max_len) return false;
    for (const char *p = text; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

/* Last two bytes of the STA MAC, lowercase hex. This is the stable part of both
 * the AP password and the mDNS host name. */
static void derive_suffix(void)
{
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        /* Deterministic fallback: a random suffix would change the default AP
         * password on every boot. */
        snprintf(s_suffix, sizeof(s_suffix), "0000");
        ESP_LOGW(TAG, "MAC read failed, suffix 0000");
    } else {
        snprintf(s_suffix, sizeof(s_suffix), "%02x%02x", mac[4], mac[5]);
    }
    snprintf(s_host, sizeof(s_host), "hueleaf-%s.local", s_suffix);
}

static void derive_default_ap_pass(char *buf, size_t len)
{
    snprintf(buf, len, AP_PASS_PREFIX "%s", s_suffix);
}

static void load_settings(void)
{
    nvs_handle_t nvs;
    s_ap_pass[0] = 0;
    s_token[0] = 0;
    if (nvs_open(NETCFG_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_ap_pass);
    if (nvs_get_str(nvs, KEY_AP_PASS, s_ap_pass, &len) != ESP_OK ||
        !netcfg_text_ok(s_ap_pass, AP_PASS_MIN, AP_PASS_MAX)) {
        s_ap_pass[0] = 0;
    }
    len = sizeof(s_token);
    if (nvs_get_str(nvs, KEY_TOKEN, s_token, &len) != ESP_OK ||
        !netcfg_text_ok(s_token, 8, TOKEN_MAX)) {
        s_token[0] = 0;
    }
    nvs_close(nvs);
}

static void save_str(const char *key, const char *value)
{
    nvs_handle_t nvs;
    if (nvs_open(NETCFG_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed");
        return;
    }
    (void)nvs_set_str(nvs, key, value);
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static void erase_key(const char *ns, const char *key)
{
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READWRITE, &nvs) != ESP_OK) return;
    (void)nvs_erase_key(nvs, key);
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

esp_err_t netcfg_init(void)
{
    if (s_ready) return ESP_OK;

    /* The MAC suffix does not depend on NVS: derive it first so a broken NVS
     * still yields a valid default AP password instead of an open network. */
    derive_suffix();
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    load_settings();
    if (s_ap_pass[0] == 0) {
        /* First boot (or a factory reset): store the derived default so the
         * value survives a later MAC-independent change of policy. */
        derive_default_ap_pass(s_ap_pass, sizeof(s_ap_pass));
        save_str(KEY_AP_PASS, s_ap_pass);
        ESP_LOGI(TAG, "AP password initialised (***)");
    }
    s_ready = true;
    ESP_LOGI(TAG, "host=%s token=%s", s_host, s_token[0] ? "set" : "off");
    return ESP_OK;
}

esp_err_t netcfg_get_ap_password(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_ready) (void)netcfg_init();
    lock();
    if (s_ap_pass[0] == 0) {
        /* NVS unavailable: still answer with the MAC-derived default so the
         * SoftAP never silently falls back to an open network. */
        derive_default_ap_pass(s_ap_pass, sizeof(s_ap_pass));
    }
    snprintf(buf, len, "%s", s_ap_pass);
    unlock();
    return ESP_OK;
}

esp_err_t netcfg_set_ap_password(const char *password)
{
    if (!netcfg_text_ok(password, AP_PASS_MIN, AP_PASS_MAX)) return ESP_ERR_INVALID_ARG;
    lock();
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", password);
    unlock();
    save_str(KEY_AP_PASS, password);
    ESP_LOGI(TAG, "AP password updated (***)");
    return ESP_OK;
}

esp_err_t netcfg_reset_ap_password(void)
{
    char pass[AP_PASS_MAX + 1];
    derive_default_ap_pass(pass, sizeof(pass));
    lock();
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", pass);
    unlock();
    save_str(KEY_AP_PASS, pass);
    ESP_LOGI(TAG, "AP password reset to default (***)");
    return ESP_OK;
}

esp_err_t netcfg_get_api_token(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_ready) (void)netcfg_init();
    lock();
    snprintf(buf, len, "%s", s_token);
    unlock();
    return ESP_OK;
}

esp_err_t netcfg_set_api_token(const char *token)
{
    if (token && token[0] && !netcfg_text_ok(token, 8, TOKEN_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *value = token ? token : "";
    lock();
    snprintf(s_token, sizeof(s_token), "%s", value);
    unlock();
    save_str(KEY_TOKEN, value);
    ESP_LOGI(TAG, "API token %s", value[0] ? "set (***)" : "cleared");
    return ESP_OK;
}

bool netcfg_token_set(void)
{
    bool set;
    if (!s_ready) (void)netcfg_init();
    lock();
    set = s_token[0] != 0;
    unlock();
    return set;
}

esp_err_t netcfg_hostname(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_ready) (void)netcfg_init();
    lock();
    snprintf(buf, len, "%s", s_host);
    unlock();
    return ESP_OK;
}

esp_err_t netcfg_mac_suffix(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_ready) (void)netcfg_init();
    lock();
    snprintf(buf, len, "%s", s_suffix);
    unlock();
    return ESP_OK;
}

/* Five seconds of a held BOOT key at boot is unambiguous: a normal picture
 * press is a tap, and the button task ignores a key still held at power-on. */
bool netcfg_boot_recovery(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << EINK_PICTURE_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "BOOT pin config failed");
        return false;
    }
    if (gpio_get_level(EINK_PICTURE_BUTTON) != EINK_PICTURE_BUTTON_ACTIVE_LEVEL) {
        return false;
    }

    ESP_LOGW(TAG, "BOOT held at boot: keep holding %d ms for factory reset",
             BOOT_HOLD_MS);
    for (int waited = 0; waited < BOOT_HOLD_MS; waited += BOOT_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
        if (gpio_get_level(EINK_PICTURE_BUTTON) != EINK_PICTURE_BUTTON_ACTIVE_LEVEL) {
            ESP_LOGI(TAG, "BOOT released after %d ms, normal boot", waited);
            return false;
        }
    }

    ESP_LOGW(TAG, "factory reset: AP password, API token and STA credentials");
    (void)netcfg_reset_ap_password();
    erase_key(NETCFG_NS, KEY_TOKEN);
    erase_key(WIFI_NS, WIFI_KEY_SSID);
    erase_key(WIFI_NS, WIFI_KEY_PASS);
    lock();
    s_token[0] = 0;
    unlock();
    ESP_LOGW(TAG, "factory reset done; AP password is the default again");
    return true;
}
