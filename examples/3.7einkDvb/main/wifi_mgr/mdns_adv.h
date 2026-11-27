#ifndef MDNS_ADV_H
#define MDNS_ADV_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

/* Minimal mDNS responder for the product name: A records for
 * "hueleaf-xxxx.local" and (best effort) "hueleaf.local", plus a PTR/SRV/TXT
 * advertisement of _http._tcp on port 80 with TXT path=/.
 *
 * ESP-IDF v5.x ships mDNS as a separate component (espressif/mdns), which this
 * project tree does not vendor. The responder below covers exactly what the
 * device advertises, without a component dependency.
 *
 * start() probes for a "hueleaf.local" owner and drops the alias when another
 * responder already claims it. stop() joins the task before the radio closes. */
esp_err_t mdns_adv_start(esp_netif_t *netif);
void mdns_adv_stop(void);
bool mdns_adv_running(void);
/* True when the friendly alias was kept (no conflict was seen). */
bool mdns_adv_alias_active(void);

#endif /* MDNS_ADV_H */
