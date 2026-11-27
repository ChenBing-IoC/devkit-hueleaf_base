#ifndef NETCFG_H
#define NETCFG_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Network identity settings kept in one NVS namespace:
 *   nrcfg/ap_pass   SoftAP WPA2 password ("hueleaf-" + 4 MAC hex digits)
 *   netcfg/token    optional API token, "" when the API is open
 * Values are plain text: NVS is not encrypted, and the token only guards
 * against accidental changes from another device on the same LAN. */
#define NETCFG_NS "netcfg"

esp_err_t netcfg_init(void);

/* Default AP password: "hueleaf-" followed by the last two bytes of the STA
 * MAC in lowercase hex (12 characters). Generated and stored on first boot. */
esp_err_t netcfg_get_ap_password(char *buf, size_t len);
esp_err_t netcfg_set_ap_password(const char *password);
esp_err_t netcfg_reset_ap_password(void);

/* Optional API token. An empty value disables the check. */
esp_err_t netcfg_get_api_token(char *buf, size_t len);
esp_err_t netcfg_set_api_token(const char *token);
bool netcfg_token_set(void);

/* "hueleaf-xxxx.local"; also exposes the 4 hex digit suffix. */
esp_err_t netcfg_hostname(char *buf, size_t len);
esp_err_t netcfg_mac_suffix(char *buf, size_t len);

/* 8..64 printable ASCII without quote or backslash. */
bool netcfg_text_ok(const char *text, size_t min_len, size_t max_len);

/* Boot-time safety valve: when BOOT (GPIO28) is held for five seconds right
 * after reset, clear the AP password, the API token and the STA credentials.
 * Returns true when the reset ran. Never prints a secret. */
bool netcfg_boot_recovery(void);

#endif /* NETCFG_H */
