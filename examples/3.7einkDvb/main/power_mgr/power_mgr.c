#include "power_mgr.h"

#include <stdio.h>
#include "axp2101.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen_rotation.h"
#include "screen_ui.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "power";

#define POWER_TASK_STACK     4096
#define POWER_TASK_PRIORITY  4
#define POWER_ACTIVE_POLL_MS 250
#define POWER_VBUS_CACHE_US  (2 * 1000 * 1000)
#define POWER_BUSY_POLL_MS   50

#if CONFIG_ESP_TASK_WDT_EN
#if CONFIG_ESP_TASK_WDT_PANIC
#define POWER_TWDT_PANIC     true
#else
#define POWER_TWDT_PANIC     false
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
#define POWER_TWDT_IDLE_MASK 1u
#else
#define POWER_TWDT_IDLE_MASK 0u
#endif
#endif

static volatile bool s_low_power;
static volatile power_mgr_state_t s_state = POWER_STATE_ACTIVE;
static volatile uint32_t s_last_activity_ms;
static power_mgr_wake_cb_t s_wake_cb;
static TaskHandle_t s_task;

/* Wrap safe millisecond clock; 32-bit keeps the read atomic across tasks. */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void power_mgr_note_activity(void)
{
    s_last_activity_ms = now_ms();
}

bool power_mgr_low_power(void)
{
    return s_low_power;
}

power_mgr_state_t power_mgr_state(void)
{
    return s_state;
}

const char *power_mgr_state_name(void)
{
    switch (s_state) {
    case POWER_STATE_LOW_POWER: return "low";
    case POWER_STATE_OFF: return "off";
    default: return "active";
    }
}

void power_mgr_set_wake_cb(power_mgr_wake_cb_t cb)
{
    s_wake_cb = cb;
}

/* Advertise the low power state on the bistable panel; it stays readable
 * while the MCU sleeps and after a PMIC shutdown. Caller holds the lock. */
static void show_low_power_page(void)
{
    char battery[24];
    const int soc = axp2101_battery_percent();
    if (soc >= 0) {
        snprintf(battery, sizeof(battery), "Battery %d%%", soc);
    } else {
        snprintf(battery, sizeof(battery), "AP is closed");
    }
    esp_err_t err = screen_ui_show_message("LOW POWER", "WiFi AP off",
                                           "Press POWER key", battery);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "low power page: %s", esp_err_to_name(err));
    }
}

static void power_off(void)
{
    s_state = POWER_STATE_OFF;
    esp_err_t err = axp2101_power_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power off failed: %s", esp_err_to_name(err));
        s_state = POWER_STATE_LOW_POWER;
        return;
    }
    /* The PMIC has cut the rails; nothing runs after this point. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Light sleep freezes the idle tasks that feed the task watchdog, so the TWDT
 * is switched off while the device sleeps and re-armed with the project
 * settings afterwards. */
#if POWER_USE_LIGHT_SLEEP
static void twdt_low_power(bool entering)
{
#if CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_config_t cfg = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = entering ? 0u : POWER_TWDT_IDLE_MASK,
        .trigger_panic = POWER_TWDT_PANIC,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "task wdt %s: %s", entering ? "off" : "on", esp_err_to_name(err));
    }
#else
    (void)entering;
#endif
}
#endif

/* Sleep until the power key is pressed or the shutdown deadline expires.
 * The rotation lock is held across each sleep so no panel or flash job can
 * start while the MCU is suspended, and released while blocking normally. */
static void low_power_loop(void)
{
    const int64_t entered_us = esp_timer_get_time();
    int64_t last_vbus_us = 0;
    bool vbus = false;
    unsigned i2c_errors = 0;

#if POWER_USE_LIGHT_SLEEP
    twdt_low_power(true);
#endif
    while (s_low_power) {
        screen_rotation_lock();
        if (web_server_screen_busy() || screen_rotation_busy()) {
            screen_rotation_unlock();
            vTaskDelay(pdMS_TO_TICKS(POWER_BUSY_POLL_MS));
            continue;
        }

        /* Reading the status registers acknowledges the latched short press,
         * so nIRQ releases and the next press can be seen again. */
        bool pressed = false;
        esp_err_t err = axp2101_take_pwron_short_press(&pressed);
        const int64_t now = esp_timer_get_time();
        if (pressed) {
            screen_rotation_unlock();
            ESP_LOGI(TAG, "POWER key: wake network");
            break;
        }

        if (POWER_AUTO_OFF_MS > 0 && now - entered_us >= (int64_t)POWER_AUTO_OFF_MS * 1000) {
            if (now - last_vbus_us >= POWER_VBUS_CACHE_US) {
                vbus = axp2101_is_usb_connected();
                last_vbus_us = now;
            }
            if (!vbus) {
                screen_rotation_unlock();
                ESP_LOGW(TAG, "low power %u min: power off to protect the battery",
                         (unsigned)(POWER_AUTO_OFF_MS / 60000));
                power_off();
                continue;
            }
        }

        esp_err_t sleep_err = ESP_OK;
#if POWER_USE_LIGHT_SLEEP
        (void)esp_sleep_enable_timer_wakeup((uint64_t)POWER_SLEEP_POLL_MS * 1000);
        sleep_err = esp_light_sleep_start();
#else
        vTaskDelay(pdMS_TO_TICKS(POWER_SLEEP_POLL_MS));
#endif
        screen_rotation_unlock();

        if (err != ESP_OK && (++i2c_errors % 20U) == 1U) {
            ESP_LOGW(TAG, "power key read failed: %s", esp_err_to_name(err));
        }
        if (sleep_err != ESP_OK) {
            ESP_LOGW(TAG, "light sleep refused: %s", esp_err_to_name(sleep_err));
            vTaskDelay(pdMS_TO_TICKS(POWER_SLEEP_POLL_MS));
        }
    }
#if POWER_USE_LIGHT_SLEEP
    twdt_low_power(false);
#endif
}

static void enter_low_power(void)
{
    ESP_LOGI(TAG, "no network activity for %u s: closing AP, entering low power",
             (unsigned)(POWER_IDLE_TIMEOUT_MS / 1000));
    s_state = POWER_STATE_LOW_POWER;
    s_low_power = true;

    esp_err_t err = wifi_mgr_suspend();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "suspending radio: %s", esp_err_to_name(err));
    }

    /* The address strip marks the awake state, so it goes away first. Then the
     * next stored picture replaces the one on the glass: one full six-ink
     * refresh per idle entry, and the low power state is "a picture with no
     * strip". Without pictures the firmware shows the low power notice. */
    screen_rotation_lock();
    screen_rotation_set_overlay(NULL);
    if (screen_rotation_hidden()) {
        /* Deliberately blanked: nothing to draw, keep it that way. */
    } else if (screen_rotation_has_images()) {
        esp_err_t swap = screen_rotation_show_next(false);
        if (swap != ESP_OK) ESP_LOGW(TAG, "idle picture: %s", esp_err_to_name(swap));
    } else {
        show_low_power_page();
    }
    screen_rotation_unlock();

    low_power_loop();

    s_low_power = false;
    s_state = POWER_STATE_ACTIVE;
    power_mgr_note_activity();

    /* Resume first: after it returns the address is final (the STA connect
     * waits inside), so the wake page carries the right strip in one redraw. */
    err = wifi_mgr_resume();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "resuming network: %s", esp_err_to_name(err));
    }
    if (s_wake_cb) {
        s_wake_cb();
    }
}

static void power_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "idle timeout %u s, auto power off after %u min in low power%s",
             (unsigned)(POWER_IDLE_TIMEOUT_MS / 1000),
             (unsigned)(POWER_AUTO_OFF_MS / 60000),
             POWER_AUTO_OFF_MS > 0 ? "" : " (disabled)");
    power_mgr_note_activity();

    for (;;) {
        const uint32_t idle_ms = (uint32_t)(now_ms() - s_last_activity_ms);
        const bool busy = web_server_screen_busy() || screen_rotation_busy();

        /* An attached client keeps the AP up no matter how idle the browser
         * is; the countdown starts again from the disconnect. */
        if (busy || wifi_mgr_ap_clients() > 0) {
            power_mgr_note_activity();
        } else if (idle_ms >= POWER_IDLE_TIMEOUT_MS) {
            enter_low_power();
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(POWER_ACTIVE_POLL_MS));
    }
}

esp_err_t power_mgr_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_last_activity_ms = now_ms();
    if (xTaskCreate(power_task, "power", POWER_TASK_STACK, NULL,
                    POWER_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
