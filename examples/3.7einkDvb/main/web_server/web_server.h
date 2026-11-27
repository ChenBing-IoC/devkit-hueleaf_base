#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include <stdbool.h>
bool web_server_screen_busy(void);

/* Fired after the SoftAP password changed so the UI task can redraw the setup
 * page with the new credentials. */
typedef void (*web_server_net_cb_t)(void);
void web_server_set_net_changed_cb(web_server_net_cb_t cb);

esp_err_t web_server_start(void);

#endif /* WEB_SERVER_H */
