#include "power_mgr.h"

#include <stdio.h>
#include "axp2101.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pcf8563.h"
#include "pin_map.h"
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
#define POWER_BATT_GUARD_US  (60 * 1000 * 1000)
#define POWER_NVS_NS         "pwrmgr"
#define POWER_NVS_INTERVAL   "interval_min"
#define POWER_NVS_MODE       "mode"
#define POWER_SNTP_SERVER    "pool.ntp.org"

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
static power_mgr_maintenance_cb_t s_maint_cb;
static TaskHandle_t s_task;

/* Boot-time facts, fixed before the first task runs. */
static power_mgr_config_t s_cfg = {
    .mode = POWER_MODE_AUTO,
    .interval_min = POWER_WAKE_INTERVAL_MIN,
};
static bool s_pin25, s_pin4, s_pin28, s_pin23;
static bool s_maintenance_boot;
static const char *s_last_wake = "poweron";

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
    case POWER_STATE_MAINTENANCE: return "maint";
    case POWER_STATE_OFF: return "off";
    default: return "active";
    }
}

const char *power_mgr_mode_name(power_mgr_mode_t mode)
{
    switch (mode) {
    case POWER_MODE_LIGHT: return "light";
    case POWER_MODE_DEEP: return "deep";
    default: return "auto";
    }
}

bool power_mgr_deep_ok(void)
{
    /* Only the PMIC nIRQ pin is wired to a user action; the other probed pins
     * are logged for reference but cannot wake anything in this firmware. */
    return s_pin25;
}

bool power_mgr_wake_pin_valid(int gpio)
{
    switch (gpio) {
    case 25: return s_pin25;
    case 4:  return s_pin4;
    case 28: return s_pin28;
    case 23: return s_pin23;
    default: return false;
    }
}

const char *power_mgr_last_wake_name(void)
{
    return s_last_wake;
}

power_mgr_config_t power_mgr_config(void)
{
    return s_cfg;
}

bool power_mgr_boot_maintenance(void)
{
    return s_maintenance_boot;
}

void power_mgr_promote_wake(void)
{
    s_maintenance_boot = false;
    s_last_wake = "timer+key";
    ESP_LOGI(TAG, "POWER key latched during sleep: interactive wake");
}

/* The LP GPIO set is chip specific and the PMIC nIRQ sits outside it on this
 * board, so the answer is asked once at boot and reported (log + /api/power)
 * instead of being assumed. */
static void probe_wake_pins(void)
{
    s_pin25 = esp_sleep_is_valid_wakeup_gpio(pmic_irq);
    s_pin4  = esp_sleep_is_valid_wakeup_gpio(imu_irq);
    s_pin28 = esp_sleep_is_valid_wakeup_gpio(EINK_PICTURE_BUTTON);
    s_pin23 = esp_sleep_is_valid_wakeup_gpio(rtc_irq);
    ESP_LOGI(TAG, "deep wake pins: %d=%s %d=%s %d=%s %d=%s%s",
             (int)pmic_irq, s_pin25 ? "yes" : "no",
             (int)imu_irq, s_pin4 ? "yes" : "no",
             (int)EINK_PICTURE_BUTTON, s_pin28 ? "yes" : "no",
             (int)rtc_irq, s_pin23 ? "yes" : "no",
             s_pin25 ? "" : " (power key: timer-only fallback, latched press promotes the wake)");
}

static void load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(POWER_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint16_t interval = 0;
    uint8_t mode = 0;
    if (nvs_get_u16(nvs, POWER_NVS_INTERVAL, &interval) == ESP_OK &&
        interval <= POWER_INTERVAL_MAX_MIN) {
        s_cfg.interval_min = interval;
    }
    if (nvs_get_u8(nvs, POWER_NVS_MODE, &mode) == ESP_OK && mode <= POWER_MODE_DEEP) {
        s_cfg.mode = (power_mgr_mode_t)mode;
    }
    nvs_close(nvs);
}

esp_err_t power_mgr_set_config(power_mgr_mode_t mode, uint16_t interval_min)
{
    if (mode > POWER_MODE_DEEP || interval_min > POWER_INTERVAL_MAX_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(POWER_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(nvs, POWER_NVS_INTERVAL, interval_min);
    if (err == ESP_OK) err = nvs_set_u8(nvs, POWER_NVS_MODE, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save policy: %s", esp_err_to_name(err));
        return err;
    }
    s_cfg.mode = mode;
    s_cfg.interval_min = interval_min;
    /* The next low power entry reads s_cfg, so the new rule applies without a
     * reboot; the current session is left alone. */
    ESP_LOGI(TAG, "policy %s, timer wake %u min", power_mgr_mode_name(mode),
             (unsigned)interval_min);
    return ESP_OK;
}

/* Timer wake -> maintenance. EXT1/GPIO -> interactive. Power-on and resets
 * keep the usual behaviour, which is interactive. */
static bool classify_boot(void)
{
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER:
        s_last_wake = "timer";
        return true;
    case ESP_SLEEP_WAKEUP_EXT1:
    case ESP_SLEEP_WAKEUP_GPIO:
        s_last_wake = "gpio";
        return false;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        s_last_wake = esp_reset_reason() == ESP_RST_POWERON ? "poweron" : "reset";
        return false;
    default:
        s_last_wake = "other";
        return false;
    }
}

void power_mgr_boot_init(void)
{
    /* NVS is opened before wifi_mgr so the policy is known to the first wake
     * decision. A damaged partition is left to wifi_mgr_start(), which erases
     * and re-initialises it; the defaults apply until then. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs an erase; using default policy");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init: %s", esp_err_to_name(err));
    }
    load_config();
    probe_wake_pins();
    s_maintenance_boot = classify_boot();
    ESP_LOGI(TAG, "wake %s, policy %s, timer wake %u min (%s), power key wake %s",
             s_last_wake, power_mgr_mode_name(s_cfg.mode),
             (unsigned)s_cfg.interval_min, s_cfg.interval_min ? "on" : "off",
             s_pin25 ? "ext1" : "timer-only");
}

void power_mgr_set_wake_cb(power_mgr_wake_cb_t cb)
{
    s_wake_cb = cb;
}

void power_mgr_set_maintenance_cb(power_mgr_maintenance_cb_t cb)
{
    s_maint_cb = cb;
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

/* Power down the MCU and come back on the timer (and on the power key when its
 * pin is in the LP domain). Waits for the panel/flash jobs first and keeps the
 * rotation lock so nothing can start on a suspended SPI bus. Returns only when
 * no wake source could be armed. */
esp_err_t power_mgr_deep_sleep(void)
{
#if !POWER_USE_DEEP_SLEEP
    return ESP_ERR_NOT_SUPPORTED;
#else
    const bool timer = s_cfg.interval_min > 0;
    if (!timer && !s_pin25) {
        /* Nothing would bring the MCU back: stay in the light sleep loop. */
        return ESP_ERR_INVALID_STATE;
    }

    screen_rotation_lock();
    while (web_server_screen_busy() || screen_rotation_busy()) {
        screen_rotation_unlock();
        vTaskDelay(pdMS_TO_TICKS(POWER_BUSY_POLL_MS));
        screen_rotation_lock();
    }

    const int soc = axp2101_battery_percent();
    if (soc >= 0 && soc < POWER_BATT_CUTOFF_PCT && !axp2101_is_usb_connected()) {
        ESP_LOGW(TAG, "battery %d%%: power off instead of sleeping", soc);
        power_off(); /* returns only when the PMIC refused */
        ESP_LOGW(TAG, "power off refused, deep sleeping anyway");
    }

    esp_err_t err = ESP_OK;
    if (timer) {
        const uint64_t wake_us = (uint64_t)s_cfg.interval_min * 60ULL * 1000000ULL;
        err = esp_sleep_enable_timer_wakeup(wake_us);
        if (err != ESP_OK) {
            screen_rotation_unlock();
            ESP_LOGE(TAG, "timer wake: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (s_pin25) {
        /* One pin only, active low (nIRQ is open drain). ALL_LOW is deprecated
         * and unsupported past ESP32; with a single pin ANY_LOW is exactly the
         * same trigger (multi-pin ANY_LOW would trip on the idle-high pins). */
        err = esp_sleep_enable_ext1_wakeup_io(1ULL << (int)pmic_irq,
                                              ESP_EXT1_WAKEUP_ANY_LOW);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "EXT1 wake on GPIO%d: %s", (int)pmic_irq, esp_err_to_name(err));
        }
    }

    /* The PCF8563 keeps the wall clock across the power down; the soft clock
     * would restart at zero. */
    esp_err_t rtc_err = pcf8563_save_system_time();
    if (rtc_err != ESP_OK) {
        ESP_LOGW(TAG, "RTC write before sleep: %s", esp_err_to_name(rtc_err));
    }
    /* Consume latched PMIC events so a pending nIRQ cannot fire EXT1 (or be
     * reported as a fresh press) right after the wake. */
    bool latched = false;
    (void)axp2101_take_pwron_short_press(&latched);
    if (latched) {
        ESP_LOGI(TAG, "POWER key pressed while parking, event consumed");
    }

    s_state = POWER_STATE_LOW_POWER;
    s_low_power = true;
    if (timer) {
        ESP_LOGI(TAG, "deep sleep, timer wake in %u min%s", (unsigned)s_cfg.interval_min,
                 s_pin25 ? ", POWER key armed" : ", POWER key latched (timer-only)");
    } else {
        ESP_LOGI(TAG, "deep sleep, timer off, POWER key armed");
    }
    esp_deep_sleep_start();
    /* esp_deep_sleep_start() does not return once a wake source is armed. */
    screen_rotation_unlock();
    return ESP_FAIL;
#endif
}

/* SNTP one-shot for the maintenance wake; returns false when the clock did not
 * sync inside the budget. */
static bool maintenance_sntp(uint64_t budget_ms)
{
    if (budget_ms == 0) {
        return false;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, POWER_SNTP_SERVER);
    esp_sntp_init();
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)budget_ms * 1000;
    bool synced = false;
    while (esp_timer_get_time() < deadline_us) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            synced = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_sntp_stop();
    return synced;
}

void power_mgr_maintenance_session(void)
{
    const int64_t start_us = esp_timer_get_time();
    const int64_t deadline_us = start_us + (int64_t)POWER_MAINT_TOTAL_MS * 1000;
    s_state = POWER_STATE_MAINTENANCE;
    s_low_power = true;

    /* A scheduled wake never touches the panel: no AP, no address strip, no
     * refresh. Only the clock and the battery are updated. */
    ESP_LOGI(TAG, "maintenance wake: %u s budget, screen untouched",
             (unsigned)(POWER_MAINT_TOTAL_MS / 1000));

    esp_err_t err = wifi_mgr_sta_start(POWER_MAINT_STA_MS);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved STA credentials, skip network");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA connect: %s, back to sleep", esp_err_to_name(err));
    } else {
        const int64_t left_ms = (int64_t)((deadline_us - esp_timer_get_time()) / 1000);
        uint64_t budget_ms = 0;
        if (left_ms > 0) {
            budget_ms = (uint64_t)left_ms < POWER_MAINT_SNTP_MS
                            ? (uint64_t)left_ms : (uint64_t)POWER_MAINT_SNTP_MS;
        }
        if (maintenance_sntp(budget_ms)) {
            esp_err_t save = pcf8563_save_system_time();
            ESP_LOGI(TAG, "SNTP ok, RTC write %s", esp_err_to_name(save));
        } else {
            ESP_LOGW(TAG, "SNTP not synced");
        }
        const int soc = axp2101_battery_percent();
        ESP_LOGI(TAG, "maintenance battery %d%% usb=%d", soc,
                 axp2101_is_usb_connected() ? 1 : 0);
        if (s_maint_cb) {
            s_maint_cb();
        }
        (void)wifi_mgr_suspend();
    }

    const int64_t used_ms = (esp_timer_get_time() - start_us) / 1000;
    if (used_ms >= POWER_MAINT_TOTAL_MS) {
        ESP_LOGW(TAG, "maintenance budget exceeded (%lld ms)", (long long)used_ms);
    }

    if (power_mgr_deep_sleep() != ESP_OK) {
        ESP_LOGE(TAG, "no deep sleep wake source, power off");
        power_off();
        /* Only reached when the PMIC refused too: park instead of falling
         * through into the interactive boot with half a session set up. */
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
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
    int64_t last_guard_us = 0;
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

        /* The 30 minute shutdown only makes sense without a scheduled wake:
         * with the timer armed the device sleeps again and again for days, so
         * the battery guard is the only shutdown. */
        if (s_cfg.interval_min == 0 && POWER_AUTO_OFF_MS > 0 &&
            now - entered_us >= (int64_t)POWER_AUTO_OFF_MS * 1000) {
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

        if (s_cfg.interval_min > 0 && now - last_guard_us >= POWER_BATT_GUARD_US) {
            last_guard_us = now;
            const int soc = axp2101_battery_percent();
            if (soc >= 0 && soc < POWER_BATT_CUTOFF_PCT && !axp2101_is_usb_connected()) {
                screen_rotation_unlock();
                ESP_LOGW(TAG, "battery %d%%: power off to protect the cell", soc);
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

/* Decides between deep and light sleep for this low power entry. The policy is
 * read here, not cached, so a /api/power change applies to the next entry. */
static bool deep_sleep_selected(void)
{
#if !POWER_USE_DEEP_SLEEP
    return false;
#else
    if (!s_pin25 && s_cfg.interval_min == 0) {
        return false; /* no wake source at all */
    }
    switch (s_cfg.mode) {
    case POWER_MODE_LIGHT:
        return false;
    case POWER_MODE_DEEP:
        return true;
    default:
        break;
    }
    /* auto: a USB cable means a debug or charge session, keep the light sleep
     * so the console and the web page stay reachable. */
    return !axp2101_is_usb_connected();
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

    if (deep_sleep_selected()) {
        esp_err_t sleep_err = power_mgr_deep_sleep();
        ESP_LOGW(TAG, "deep sleep unavailable: %s, staying in light sleep",
                 esp_err_to_name(sleep_err));
    }

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

    ESP_LOGI(TAG, "idle timeout %u s, policy %s, timer wake %u min, deep sleep %s",
             (unsigned)(POWER_IDLE_TIMEOUT_MS / 1000),
             power_mgr_mode_name(s_cfg.mode), (unsigned)s_cfg.interval_min,
             POWER_USE_DEEP_SLEEP ? (s_pin25 ? "key+timer" : "timer-only")
                                  : "off");
    ESP_LOGI(TAG, "auto power off after %u min only while the timer wake is off (0 = never)",
             (unsigned)(POWER_AUTO_OFF_MS / 60000));
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
