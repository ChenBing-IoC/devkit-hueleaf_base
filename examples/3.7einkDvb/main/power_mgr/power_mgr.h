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

/* How long the device may stay in low power before it switches itself off
 * through the PMIC. 0 disables the shutdown; it is also skipped while USB
 * power is present so a docked device never powers down mid charge. */
#ifndef POWER_AUTO_OFF_MS
#define POWER_AUTO_OFF_MS (30 * 60 * 1000)
#endif

/* Picture cycle: the three uploaded slots rotate in order. Every awake period
 * shows the current picture with an address strip along the bottom; entering
 * low power drops the strip and draws the next picture, so the strip is the
 * awake marker. One full six-ink refresh happens per state change. */

typedef enum {
    POWER_STATE_ACTIVE = 0, /* radio on, web server reachable */
    POWER_STATE_LOW_POWER,  /* AP closed, MCU in light sleep, waits for the key */
    POWER_STATE_OFF,        /* PMIC cut the rails */
} power_mgr_state_t;

/* Called after the power key brought the device back from low power, once the
 * network is up again. The owner redraws the network page from here. */
typedef void (*power_mgr_wake_cb_t)(void);

esp_err_t power_mgr_start(void);
void power_mgr_set_wake_cb(power_mgr_wake_cb_t cb);
/* Any request or local interaction restarts the inactivity countdown. */
void power_mgr_note_activity(void);
bool power_mgr_low_power(void);
power_mgr_state_t power_mgr_state(void);
const char *power_mgr_state_name(void);

#endif /* POWER_MGR_H */
