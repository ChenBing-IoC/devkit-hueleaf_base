#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define WIFI_AP_SSID     "Eink E6 3.7"
#define WIFI_AP_CHANNEL  36
#define WIFI_AP_URL      "http://192.168.4.1"

typedef enum {
    WIFI_MGR_MODE_AP = 0,
    WIFI_MGR_MODE_STA,
} wifi_mgr_mode_t;

typedef void (*wifi_mgr_ready_cb_t)(void);

esp_err_t wifi_mgr_start(void);
/* Maintenance wake: STA only, never the provisioning AP. ESP_ERR_NOT_FOUND
 * means no saved credentials, ESP_ERR_TIMEOUT that the router was not
 * reachable inside timeout_ms (the radio is left stopped). */
esp_err_t wifi_mgr_sta_start(uint32_t timeout_ms);
/* Low power handshake: suspend() closes the SoftAP and stops the radio while
 * keeping saved credentials, resume() brings the same network back. */
esp_err_t wifi_mgr_suspend(void);
esp_err_t wifi_mgr_resume(void);
wifi_mgr_mode_t wifi_mgr_mode(void);
bool wifi_mgr_is_sta_up(void);
bool wifi_mgr_is_ap_up(void);
/* Connected SoftAP stations. A non-zero count postpones low power. */
int wifi_mgr_ap_clients(void);
esp_err_t wifi_mgr_get_ip(char *buf, size_t len);
esp_err_t wifi_mgr_get_ssid(char *buf, size_t len);
esp_err_t wifi_mgr_service_url(char *buf, size_t len);
esp_err_t wifi_mgr_scan_json(char *buf, size_t len);
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);
esp_err_t wifi_mgr_clear_creds(void);
void wifi_mgr_set_ready_cb(wifi_mgr_ready_cb_t cb);

#endif /* WIFI_MGR_H */
