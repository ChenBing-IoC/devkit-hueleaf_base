#ifndef POWER_MGR_H
#define POWER_MGR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Drop to low power after this much network inactivity. A connected SoftAP
 * client (or a running screen job) always postpones the countdown, so the
 * two minute clock restarts when the last client leaves. */
#ifndef POWER_IDLE_TIMEOUT_MS
#define POWER_IDLE_TIMEOUT_MS (2 * 60 * 1000)
#endif

/* Low power polls the AXP2101 power key through a 500ms light-sleep timer.
 * Short sleeps keep the wake latency and the average current low. */
#ifndef POWER_SLEEP_POLL_MS
#define POWER_SLEEP_POLL_MS 500
#endif

/* Light sleep drops the ESP32-C5 USB-Serial/JTAG console: the host port
 * disappears at the first sleep and the ROM does not re-attach it, so
 * idf.py monitor prints nothing afterwards even though the firmware keeps
 * running. UART0 on the header is unaffected. Set this to 0 to poll with
 * vTaskDelay during bring-up and keep the USB console alive; the standby
 * current is then several mA instead of ~1 mA. */
#ifndef POWER_USE_LIGHT_SLEEP
#define POWER_USE_LIGHT_SLEEP 1
#endif

/* Adaptive standby: the low power state becomes esp_deep_sleep_start() with
 * an RTC timer wake, so the MCU really powers down instead of idling in light
 * sleep. The power key wakes it through EXT1 only when its GPIO is in the LP
 * domain (checked at boot); otherwise a press during sleep is latched by the
 * PMIC and promotes the next timer wake to an interactive session. Set this
 * to 0 to keep the old light-sleep-only behaviour. */
#ifndef POWER_USE_DEEP_SLEEP
#define POWER_USE_DEEP_SLEEP 1
#endif

/* Motion wake: add the accelerometer any-motion interrupt (GPIO4) as an extra
 * deep sleep wake source next to the timer and the power key. The IMU must be
 * present and its interrupt register table verified; otherwise this silently
 * stays out of the way and the other sources still apply. Set to 0 to keep
 * GPIO4 out of the wake configuration entirely. */
#ifndef POWER_IMU_WAKE
#define POWER_IMU_WAKE 1
#endif

/* Default timer wake interval in minutes (0 disables scheduled wakes). The
 * value lives in NVS ("pwrmgr"/"interval_min") and can be changed from
 * /api/power, so this is only the first-boot default. */
#ifndef POWER_WAKE_INTERVAL_MIN
#define POWER_WAKE_INTERVAL_MIN 60
#endif

/* Last-resort wake interval used when a deep sleep would otherwise have no
 * armed source at all (for example the IMU interrupt failed after the policy
 * pre-check). Keeps a sensor fault from parking the MCU until the battery dies. */
#ifndef POWER_FALLBACK_WAKE_MIN
#define POWER_FALLBACK_WAKE_MIN 60
#endif

/* Upper bound for the stored interval (one week); a malformed POST cannot
 * schedule a wake months away. */
#define POWER_INTERVAL_MAX_MIN 10080u

/* How long the device may stay in low power before it switches itself off
 * through the PMIC. Only used while the timer wake is disabled
 * (interval_min == 0): with scheduled wakes the 30 minute rule would kill the
 * wake cycle, so the battery guard below is the only shutdown instead. 0
 * disables the shutdown; it is also skipped while USB power is present so a
 * docked device never powers down mid charge. */
#ifndef POWER_AUTO_OFF_MS
#define POWER_AUTO_OFF_MS (30 * 60 * 1000)
#endif

/* Battery protection: when the fuel gauge reports a valid state of charge
 * below this percentage the device powers off instead of sleeping. Checked
 * before every deep sleep and once a minute in the light sleep loop; skipped
 * on USB power, where the cell is charging rather than draining. */
#ifndef POWER_BATT_CUTOFF_PCT
#define POWER_BATT_CUTOFF_PCT 5
#endif

/* Maintenance wake budget (timer wake, no AP): STA connect cap, SNTP wait and
 * the hard cap for the whole session. The session always ends in deep sleep,
 * so a stuck network cannot keep the radio on for long. */
#ifndef POWER_MAINT_STA_MS
#define POWER_MAINT_STA_MS 10000
#endif
#ifndef POWER_MAINT_SNTP_MS
/* lwIP delays the first request by a random 0..5 s (CONFIG_LWIP_SNTP_*), so the
 * wait must cover that plus the round trip. */
#define POWER_MAINT_SNTP_MS 10000
#endif
#ifndef POWER_MAINT_TOTAL_MS
#define POWER_MAINT_TOTAL_MS 45000
#endif

/* Picture cycle: the three uploaded slots rotate in order. Every awake period
 * shows the current picture with an address strip along the bottom; entering
 * low power drops the strip and draws the next picture, so the strip is the
 * awake marker. One full six-ink refresh happens per state change. */

typedef enum {
    POWER_STATE_ACTIVE = 0, /* radio on, web server reachable */
    POWER_STATE_LOW_POWER,  /* AP closed, MCU sleeping, waits for the key */
    POWER_STATE_MAINTENANCE,/* timer wake: STA + clock only, no panel/AP */
    POWER_STATE_OFF,        /* PMIC cut the rails */
} power_mgr_state_t;

/* Sleep policy. auto keeps light sleep while USB power is present (console and
 * web page stay usable) and uses deep sleep on battery; light pins the old
 * behaviour; deep sleeps even with the cable attached. */
typedef enum {
    POWER_MODE_AUTO = 0,
    POWER_MODE_LIGHT,
    POWER_MODE_DEEP,
} power_mgr_mode_t;

typedef struct {
    power_mgr_mode_t mode;
    uint16_t interval_min; /* 0 = timer wake disabled */
} power_mgr_config_t;

/* Called after the power key brought the device back from low power, once the
 * network is up again. The owner redraws the network page from here. */
typedef void (*power_mgr_wake_cb_t)(void);
/* Runs inside a maintenance wake after the clock was set and before the device
 * goes back to sleep. The content pipeline (weather, calendar, ...) plugs in
 * here; it must not touch the panel itself. */
typedef void (*power_mgr_maintenance_cb_t)(void);

/* First call in app_main: loads the stored policy, probes the deep sleep wake
 * pins and classifies this boot. Safe with a cold NVS partition. */
void power_mgr_boot_init(void);
/* True when this boot was a scheduled timer wake (maintenance session). */
bool power_mgr_boot_maintenance(void);
/* A latched PWRON press was found during a timer wake: treat the session as
 * interactive from now on. */
void power_mgr_promote_wake(void);
/* Timer wake body: STA only (no AP, no panel), SNTP -> PCF8563, wake task
 * hook, then deep sleep. Does not return. */
void power_mgr_maintenance_session(void);
/* Arm the wake sources and enter deep sleep. Returns only on failure. */
esp_err_t power_mgr_deep_sleep(void);
bool power_mgr_deep_ok(void);
bool power_mgr_wake_pin_valid(int gpio);
const char *power_mgr_last_wake_name(void);
power_mgr_config_t power_mgr_config(void);
esp_err_t power_mgr_set_config(power_mgr_mode_t mode, uint16_t interval_min);
const char *power_mgr_mode_name(power_mgr_mode_t mode);

esp_err_t power_mgr_start(void);
void power_mgr_set_wake_cb(power_mgr_wake_cb_t cb);
void power_mgr_set_maintenance_cb(power_mgr_maintenance_cb_t cb);
/* Any request or local interaction restarts the inactivity countdown. */
void power_mgr_note_activity(void);
bool power_mgr_low_power(void);
power_mgr_state_t power_mgr_state(void);
const char *power_mgr_state_name(void);

#endif /* POWER_MGR_H */
