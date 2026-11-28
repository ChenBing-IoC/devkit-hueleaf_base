#include "imu_app.h"

#include <math.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_map.h"
#include "power_mgr.h"
#include "screen_rotation.h"
#include "web_server.h"

static const char *TAG = "imu_app";

#define IMU_TASK_STACK       4096
#define IMU_TASK_PRIORITY    2
#define IMU_SAMPLE_MS        66      /* about 15 Hz while awake */
#define IMU_LOW_POWER_MS     500     /* no I2C while the radio is parked */
#define IMU_FLIP_Z_G         0.55f   /* z must point clearly up or down */
#define IMU_FACE_G           0.75f   /* |z| above this means lying flat */
#define IMU_FLIP_STABLE_MS   300     /* a sign change must hold this long */
#define IMU_FLIP_GUARD_MS    800     /* one flip per this window */
#define IMU_TAP_G            1.25f   /* |a| spike above 1 g */
#define IMU_TAP_GAP_MS       120     /* ignore the same shock twice */
#define IMU_TAP_WINDOW_MS    400     /* second impact within this = tap */
#define IMU_ACTION_GUARD_MS  800     /* one gesture action per window */
#define IMU_ACTION_WAIT_MS   2000    /* wait for an upload before giving up */

static TaskHandle_t s_task;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static imu_app_status_t s_status;

const char *imu_app_orientation_name(msa321_orientation_t orientation)
{
    switch (orientation) {
    case MSA321_ORIENT_PORTRAIT: return "portrait";
    case MSA321_ORIENT_LANDSCAPE: return "landscape";
    case MSA321_ORIENT_FACEUP: return "faceup";
    case MSA321_ORIENT_FACEDOWN: return "facedown";
    default: return "unknown";
    }
}

/* The register table, the LP GPIO check and the sensor answer are all static
 * properties, so this can be asked at any time, including a maintenance boot
 * where the sampling task has not been created. */
bool imu_app_wake_supported(void)
{
#if POWER_IMU_WAKE
    return msa321_int_ready() && esp_sleep_is_valid_wakeup_gpio(imu_irq);
#else
    return false;
#endif
}

void imu_app_clear_wake(void)
{
    /* Snapshot the axis that started the event before RESET_INT drops it:
     * false wakes from the desk can then be traced to a direction and the
     * threshold tuned accordingly. Only the first boot call has data. */
    uint8_t active = 0;
    if (msa321_active_status(&active) == ESP_OK && (active & 0x07) != 0) {
        const char *axis = (active & 0x04) ? "X" : (active & 0x02) ? "Y" : "Z";
        ESP_LOGI(TAG, "last motion: %s %s (0x0B=0x%02X)", axis,
                 (active & 0x08) ? "negative" : "positive", active);
    }
    if (msa321_int_clear() == ESP_OK) {
        ESP_LOGI(TAG, "latched motion interrupt cleared");
    }
}

bool imu_app_prepare_wake(void)
{
    if (!imu_app_wake_supported()) {
        return false;
    }
    /* board.c configures every shared IRQ input with an internal pull-up.
     * INT1 is push-pull and active high here, so the pull-up only sinks about
     * 70 uA while the sensor drives the line low all night, and it reads as
     * "motion" if the sensor ever loses power. A pull-down is the correct
     * bias for this signal. */
    esp_err_t pull = gpio_set_pull_mode(imu_irq, GPIO_PULLDOWN_ONLY);
    if (pull != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d pull-down: %s", (int)imu_irq, esp_err_to_name(pull));
    }
    esp_err_t err = msa321_int_config_motion();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "motion interrupt: %s", esp_err_to_name(err));
        return false;
    }
    /* The sensor latches INT1, and a sample can latch while the interrupt is
     * being programmed. The pin must read low before it is armed, otherwise
     * the chip wakes the moment it sleeps. */
    if (msa321_int_clear() != ESP_OK) {
        ESP_LOGW(TAG, "motion interrupt latch not cleared, wake disabled");
        return false;
    }
    ESP_LOGI(TAG, "motion interrupt armed on GPIO%d", (int)imu_irq);
    return true;
}

void imu_app_status(imu_app_status_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    out->present = msa321_present();
    out->int_ready = msa321_int_ready();
    out->wake_armed = imu_app_wake_supported();
}

static void publish_sample(const msa321_sample_t *sample,
                           msa321_orientation_t orientation)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.x_g = sample->x_g;
    s_status.y_g = sample->y_g;
    s_status.z_g = sample->z_g;
    s_status.orientation = orientation;
    portEXIT_CRITICAL(&s_status_lock);
}

static void count_event(bool flip)
{
    portENTER_CRITICAL(&s_status_lock);
    if (flip) {
        s_status.flip_events++;
    } else {
        s_status.tap_events++;
    }
    portEXIT_CRITICAL(&s_status_lock);
}

/* The panel is remapped nowhere: names describe the gravity direction in the
 * sensor frame. Flat means the panel faces up or down, otherwise the stronger
 * horizontal axis decides between portrait and landscape. */
static msa321_orientation_t classify(const msa321_sample_t *sample)
{
    if (sample->z_g >= IMU_FACE_G) {
        return MSA321_ORIENT_FACEUP;
    }
    if (sample->z_g <= -IMU_FACE_G) {
        return MSA321_ORIENT_FACEDOWN;
    }
    return fabsf(sample->x_g) >= fabsf(sample->y_g) ? MSA321_ORIENT_LANDSCAPE
                                                    : MSA321_ORIENT_PORTRAIT;
}

/* Same path as the BOOT key: wait for an upload, hold the rotation lock for
 * the whole refresh, and never add a second slideshow route. The address
 * strip stays on, because the device is awake. */
static void next_picture(const char *why)
{
    power_mgr_note_activity();
    const int64_t deadline = esp_timer_get_time() + (int64_t)IMU_ACTION_WAIT_MS * 1000;
    while (web_server_screen_busy() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    screen_rotation_lock();
    if (web_server_screen_busy()) {
        screen_rotation_unlock();
        ESP_LOGW(TAG, "%s: screen busy, gesture ignored", why);
        return;
    }
    esp_err_t err;
    if (screen_rotation_hidden()) {
        err = ESP_OK;   /* a deliberately blanked panel stays blank */
    } else if (screen_rotation_has_images()) {
        err = screen_rotation_show_next(true);
    } else {
        err = ESP_ERR_INVALID_STATE;
    }
    screen_rotation_unlock();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: next picture", why);
    } else {
        ESP_LOGW(TAG, "%s: next picture: %s", why, esp_err_to_name(err));
    }
}

static void imu_task(void *arg)
{
    (void)arg;
    int stable_sign = 0;
    int candidate = 0;
    int64_t candidate_since = 0;
    int64_t last_flip_ms = 0;
    int64_t last_impact_ms = 0;
    int64_t last_tap_ms = 0;
    float previous_mag = 1.0f;
    bool have_previous = false;

    for (;;) {
        /* Light sleep polls the power key; sampling I2C during that loop would
         * defeat it, and a deep sleep wake restarts this task anyway. */
        if (power_mgr_low_power()) {
            vTaskDelay(pdMS_TO_TICKS(IMU_LOW_POWER_MS));
            continue;
        }
        msa321_sample_t sample;
        if (msa321_read(&sample) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        const int64_t now = esp_timer_get_time() / 1000;
        publish_sample(&sample, classify(&sample));

        /* Flip: z must cross the threshold and hold the new sign for 300 ms.
         * The first stable sign is the baseline, not an event. */
        const int sign = sample.z_g > IMU_FLIP_Z_G ? 1
                       : sample.z_g < -IMU_FLIP_Z_G ? -1 : 0;
        if (sign == 0) {
            candidate = 0;
        } else {
            if (sign != candidate) {
                candidate = sign;
                candidate_since = now;
            }
            if (candidate != stable_sign &&
                now - candidate_since >= IMU_FLIP_STABLE_MS) {
                const bool turned = stable_sign != 0;
                stable_sign = candidate;
                if (turned && now - last_flip_ms >= IMU_FLIP_GUARD_MS) {
                    last_flip_ms = now;
                    count_event(true);
                    next_picture("flip");
                }
            }
        }

        /* Double tap: two shocks (|a| above 1.25 g) 120..400 ms apart, with
         * the same 800 ms lockout as a flip so walking cannot chain events. */
        const float magnitude = sqrtf(sample.x_g * sample.x_g +
                                      sample.y_g * sample.y_g +
                                      sample.z_g * sample.z_g);
        if (have_previous && previous_mag < IMU_TAP_G && magnitude >= IMU_TAP_G &&
            now - last_impact_ms >= IMU_TAP_GAP_MS) {
            if (last_impact_ms > 0 && now - last_impact_ms <= IMU_TAP_WINDOW_MS &&
                now - last_tap_ms >= IMU_ACTION_GUARD_MS &&
                now - last_flip_ms >= IMU_FLIP_GUARD_MS) {
                last_tap_ms = now;
                last_impact_ms = 0;
                count_event(false);
                next_picture("double tap");
            } else {
                last_impact_ms = now;
            }
        }
        previous_mag = magnitude;
        have_previous = true;
        vTaskDelay(pdMS_TO_TICKS(IMU_SAMPLE_MS));
    }
}

esp_err_t imu_app_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (!msa321_present()) {
        return ESP_ERR_NOT_FOUND;
    }
    const bool int_ready = msa321_int_ready();
    const bool wake = imu_app_wake_supported();
    portENTER_CRITICAL(&s_status_lock);
    s_status.present = true;
    s_status.int_ready = int_ready;
    s_status.wake_armed = wake;
    portEXIT_CRITICAL(&s_status_lock);

    if (xTaskCreate(imu_task, "imu", IMU_TASK_STACK, NULL,
                    IMU_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sampling at %d Hz, motion wake %s", 1000 / IMU_SAMPLE_MS,
             wake ? "available" : "unavailable");
    return ESP_OK;
}
