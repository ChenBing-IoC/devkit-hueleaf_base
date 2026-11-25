#include "power_mgr.h"

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_map.h"
#include "screen_rotation.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "power";

#define POWER_POLL_MS        2000
#define POWER_WAKE_HOLD_MS   2000   /* give up if a wake line never releases */
#define POWER_SLEEP_RETRIES  10

static TaskHandle_t s_button_task;
static volatile bool s_suspended;
static int64_t s_last_active_us;

void power_mgr_register_button_task(TaskHandle_t task)
{
    s_button_task = task;
}

bool power_mgr_is_suspended(void)
{
    return s_suspended;
}

static void configure_pm(void)
{
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PM unavailable (%s); enable CONFIG_PM_ENABLE in menuconfig",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PM ready: 80..160 MHz, automatic light sleep");
    }
}

static bool wake_line_idle(int gpio, int active_level)
{
    return gpio_get_level(gpio) != active_level;
}

static bool all_wake_lines_idle(void)
{
    return wake_line_idle(EINK_ROTATE_BUTTON, EINK_ROTATE_BUTTON_ACTIVE_LEVEL) &&
           wake_line_idle(pmic_irq, 0) &&
           wake_line_idle(rtc_irq, 0);
}

static void enable_wake_sources(void)
{
    /* Light sleep GPIO wake works on any GPIO; deep sleep only on GPIO0..5,
     * which this board does not use for its buttons. */
    gpio_wakeup_enable(EINK_ROTATE_BUTTON, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(pmic_irq, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(rtc_irq, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

static void disable_wake_sources(void)
{
    gpio_wakeup_disable(EINK_ROTATE_BUTTON);
    gpio_wakeup_disable(pmic_irq);
    gpio_wakeup_disable(rtc_irq);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    /* gpio_wakeup_enable() changes the trigger level; restore the original
     * configurations (PMIC keeps its falling-edge task notification). */
    gpio_set_intr_type(EINK_ROTATE_BUTTON, GPIO_INTR_DISABLE);
    gpio_set_intr_type(rtc_irq, GPIO_INTR_DISABLE);
    gpio_set_intr_type(pmic_irq, GPIO_INTR_NEGEDGE);
}

static void wake_button_task(void)
{
    if (s_button_task) {
        xTaskNotifyGive(s_button_task);
    }
}

static void sleep_until_user(void)
{
    s_suspended = true;

    /* Hold the rotation lock across the sleep: a display job must not start
     * while the CPU is parked, and this also waits for an in-flight job. */
    screen_rotation_lock();
    if (web_server_screen_busy() || screen_rotation_busy()) {
        screen_rotation_unlock();
        ESP_LOGI(TAG, "display job active; stay awake");
        s_suspended = false;
        s_last_active_us = esp_timer_get_time();
        wake_button_task();
        return;
    }

    esp_err_t err = wifi_mgr_suspend();
    if (err != ESP_OK) {
        screen_rotation_unlock();
        ESP_LOGW(TAG, "Wi-Fi suspend failed (%s); stay awake", esp_err_to_name(err));
        s_suspended = false;
        s_last_active_us = esp_timer_get_time();   /* avoid a retry storm */
        wake_button_task();
        return;
    }

    /* Let the radio and HTTP sockets settle before the CPU sleeps. */
    vTaskDelay(pdMS_TO_TICKS(100));
    enable_wake_sources();
    ESP_LOGW(TAG, "provisioning idle: Wi-Fi off, light sleep (BOOT / POWER / RTC wakes)");

    TickType_t held = 0;
    int failures = 0;
    for (;;) {
        if (!all_wake_lines_idle()) {
            if (held == 0) {
                held = xTaskGetTickCount();
            } else if ((TickType_t)(xTaskGetTickCount() - held) >= pdMS_TO_TICKS(POWER_WAKE_HOLD_MS)) {
                ESP_LOGW(TAG, "wake line stuck low; aborting sleep");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        held = 0;

        err = esp_light_sleep_start();
        const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_GPIO) {
            ESP_LOGI(TAG, "woken by button/IRQ");
            break;
        }
        if (err != ESP_OK && ++failures >= POWER_SLEEP_RETRIES) {
            ESP_LOGW(TAG, "light sleep failed (%s); aborting", esp_err_to_name(err));
            break;
        }
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    disable_wake_sources();
    screen_rotation_unlock();
    s_suspended = false;

    /* esp_wifi_start() can transiently fail right after a stop; retry so the
     * provisioning AP comes back instead of leaving the device unreachable. */
    for (int attempt = 0; attempt < 5; ++attempt) {
        err = wifi_mgr_resume();
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "Wi-Fi resume failed (%s), retry %d", esp_err_to_name(err), attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi resume gave up: %s", esp_err_to_name(err));
    }
    s_last_active_us = esp_timer_get_time();
    wake_button_task();
    ESP_LOGI(TAG, "awake, provisioning AP reopened");
}

static void power_task(void *arg)
{
    (void)arg;
    s_last_active_us = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POWER_POLL_MS));

        const bool provisioned = wifi_mgr_is_sta_up();
        const bool ap_up = wifi_mgr_is_ap_up();
        const bool busy = web_server_screen_busy() || screen_rotation_busy();
        const bool client = wifi_mgr_ap_client_count() > 0;

        /* Only a provisioning AP with nobody attached is worth suspending.
         * Once provisioned (STA up) the web server must stay reachable. */
        if (provisioned || !ap_up || busy || client) {
            s_last_active_us = esp_timer_get_time();
            continue;
        }

        const int64_t idle_us = esp_timer_get_time() - s_last_active_us;
        if (idle_us < (int64_t)POWER_AP_IDLE_TIMEOUT_MS * 1000) {
            continue;
        }

        sleep_until_user();
    }
}

esp_err_t power_mgr_init(void)
{
    configure_pm();
    if (xTaskCreate(power_task, "power", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "power task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "provisioning AP idle timeout %d s", POWER_AP_IDLE_TIMEOUT_MS / 1000);
    return ESP_OK;
}
