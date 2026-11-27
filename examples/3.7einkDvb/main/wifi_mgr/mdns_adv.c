#include "mdns_adv.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "netcfg.h"

static const char *TAG = "mdns";

#define MDNS_PORT       5353
#define MDNS_GROUP_ADDR 0xFB0000E0U /* 224.0.0.251, network byte order */
#define MDNS_TTL_HOST   120
#define MDNS_TTL_SVC    4500
#define MDNS_PKT_MAX    768
#define MDNS_PROBES     3
#define MDNS_PROBE_MS   150

#define TYPE_A    1
#define TYPE_PTR  12
#define TYPE_TXT  16
#define TYPE_SRV  33
#define TYPE_ANY  255
#define CLASS_IN  1
#define CLASS_FLUSH 0x8001

static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_stop;
static int s_sock = -1;
static uint32_t s_ip;               /* network byte order */
static char s_host[40];             /* hueleaf-xxxx.local */
static char s_alias[24];            /* hueleaf.local, "" when conflicting */
static char s_instance[48];         /* HueLeaf E6._http._tcp.local */
static uint8_t s_pkt[MDNS_PKT_MAX];

static size_t put_u16(uint8_t *p, size_t n, uint16_t v)
{
    p[n++] = (uint8_t)(v >> 8);
    p[n++] = (uint8_t)(v & 0xFF);
    return n;
}

static size_t put_u32(uint8_t *p, size_t n, uint32_t v)
{
    p[n++] = (uint8_t)(v >> 24);
    p[n++] = (uint8_t)((v >> 16) & 0xFF);
    p[n++] = (uint8_t)((v >> 8) & 0xFF);
    p[n++] = (uint8_t)(v & 0xFF);
    return n;
}

/* Length-prefixed labels, no compression: the full answer set stays well below
 * the 512 byte multicast limit and needs no pointer table. */
static size_t put_name(uint8_t *p, const char *name)
{
    size_t n = 0;
    const char *label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t len = dot ? (size_t)(dot - label) : strlen(label);
        if (len > 63) len = 63;
        p[n++] = (uint8_t)len;
        memcpy(p + n, label, len);
        n += len;
        if (!dot) break;
        label = dot + 1;
    }
    p[n++] = 0;
    return n;
}

/* Decode one (possibly compressed) name and advance *off past the field. */
static esp_err_t get_name(const uint8_t *pkt, size_t len, size_t *off,
                          char *out, size_t out_len)
{
    size_t pos = *off;
    size_t used = 0;
    size_t out_pos = 0;
    bool jumped = false;
    int hops = 0;

    if (out_len == 0) return ESP_ERR_INVALID_ARG;
    while (pos < len && hops < 8) {
        const uint8_t c = pkt[pos];
        if (c == 0) {
            if (!jumped) used++;
            *off += used;
            out[out_pos] = 0;
            return ESP_OK;
        }
        if ((c & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return ESP_FAIL;
            const size_t target = ((size_t)(c & 0x3F) << 8) | pkt[pos + 1];
            if (!jumped) used += 2;
            jumped = true;
            hops++;
            if (target >= len) return ESP_FAIL;
            pos = target;
            continue;
        }
        if ((c & 0xC0) != 0) return ESP_FAIL;
        const size_t label = c;
        if (pos + 1 + label > len) return ESP_FAIL;
        if (out_pos + 1 < out_len) {
            if (out_pos > 0) out[out_pos++] = '.';
            for (size_t i = 0; i < label && out_pos < out_len - 1; ++i) {
                out[out_pos++] = (char)pkt[pos + 1 + i];
            }
        }
        pos += 1 + label;
        if (!jumped) used += 1 + label;
    }
    return ESP_FAIL;
}

static size_t rr_begin(uint8_t *buf, size_t off, const char *name,
                       uint16_t type, uint16_t cls, uint32_t ttl,
                       size_t *rdlen_pos)
{
    off += put_name(buf + off, name);
    off = put_u16(buf, off, type);
    off = put_u16(buf, off, cls);
    off = put_u32(buf, off, ttl);
    *rdlen_pos = off;
    off += 2;
    return off;
}

static size_t rr_end(uint8_t *buf, size_t off, size_t rdlen_pos)
{
    const size_t rdlen = off - (rdlen_pos + 2);
    buf[rdlen_pos] = (uint8_t)(rdlen >> 8);
    buf[rdlen_pos + 1] = (uint8_t)(rdlen & 0xFF);
    return off;
}

/* Every announcement and every query answer carries the same record set: the
 * service PTR, the DNS-SD enumeration PTR, SRV/TXT and one or two A records. */
static size_t build_response(uint8_t *buf, size_t cap)
{
    if (cap < 200) return 0;
    memset(buf, 0, 12);
    buf[2] = 0x84;                      /* response, authoritative */
    buf[3] = 0x00;
    size_t off = 12;
    uint16_t count = 0;
    size_t rd;

    off = rr_begin(buf, off, "_http._tcp.local", TYPE_PTR, CLASS_IN, MDNS_TTL_SVC, &rd);
    off += put_name(buf + off, s_instance);
    off = rr_end(buf, off, rd);
    count++;

    off = rr_begin(buf, off, "_services._dns-sd._udp.local", TYPE_PTR, CLASS_IN,
                   MDNS_TTL_SVC, &rd);
    off += put_name(buf + off, "_http._tcp.local");
    off = rr_end(buf, off, rd);
    count++;

    off = rr_begin(buf, off, s_instance, TYPE_SRV, CLASS_FLUSH, MDNS_TTL_HOST, &rd);
    off = put_u16(buf, off, 0);
    off = put_u16(buf, off, 0);
    off = put_u16(buf, off, 80);
    off += put_name(buf + off, s_host);
    off = rr_end(buf, off, rd);
    count++;

    off = rr_begin(buf, off, s_instance, TYPE_TXT, CLASS_FLUSH, MDNS_TTL_SVC, &rd);
    buf[off++] = 6;
    memcpy(buf + off, "path=/", 6);
    off += 6;
    off = rr_end(buf, off, rd);
    count++;

    off = rr_begin(buf, off, s_host, TYPE_A, CLASS_FLUSH, MDNS_TTL_HOST, &rd);
    memcpy(buf + off, &s_ip, 4);
    off += 4;
    off = rr_end(buf, off, rd);
    count++;

    if (s_alias[0]) {
        off = rr_begin(buf, off, s_alias, TYPE_A, CLASS_FLUSH, MDNS_TTL_HOST, &rd);
        memcpy(buf + off, &s_ip, 4);
        off += 4;
        off = rr_end(buf, off, rd);
        count++;
    }

    buf[6] = (uint8_t)(count >> 8);
    buf[7] = (uint8_t)(count & 0xFF);
    return off;
}

static bool query_matches(const uint8_t *pkt, size_t len)
{
    if (len < 12) return false;
    const uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    size_t off = 12;
    bool match = false;

    for (uint16_t i = 0; i < qd && off < len; ++i) {
        char name[80];
        if (get_name(pkt, len, &off, name, sizeof(name)) != ESP_OK) break;
        if (off + 4 > len) break;
        const uint16_t type = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        off += 4;
        if (type == TYPE_A || type == TYPE_ANY) {
            if (!strcasecmp(name, s_host) ||
                (s_alias[0] && !strcasecmp(name, s_alias))) {
                match = true;
            }
        }
        if (type == TYPE_PTR &&
            (!strcasecmp(name, "_http._tcp.local") ||
             !strcasecmp(name, "_services._dns-sd._udp.local"))) {
            match = true;
        }
        if ((type == TYPE_SRV || type == TYPE_TXT || type == TYPE_ANY) &&
            !strcasecmp(name, s_instance)) {
            match = true;
        }
    }
    return match;
}

/* An answer section owner equal to the alias means somebody else owns it,
 * except an A record that still points at our own address: that is a cache of
 * a previous announcement from this device, not a conflict. */
static bool response_claims(const uint8_t *pkt, size_t len, const char *name)
{
    if (len < 12 || !(pkt[2] & 0x80)) return false;
    const uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    const uint16_t an = (uint16_t)((pkt[6] << 8) | pkt[7]);
    size_t off = 12;
    char owner[80];

    for (uint16_t i = 0; i < qd; ++i) {
        if (get_name(pkt, len, &off, owner, sizeof(owner)) != ESP_OK) return false;
        if (off + 4 > len) return false;
        off += 4;
    }
    for (uint16_t i = 0; i < an; ++i) {
        if (get_name(pkt, len, &off, owner, sizeof(owner)) != ESP_OK) return false;
        if (off + 10 > len) return false;
        const uint16_t type = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        const uint16_t rdlen = (uint16_t)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if (off + rdlen > len) return false;
        if (!strcasecmp(owner, name)) {
            if (type == TYPE_A && rdlen == 4 && memcmp(pkt + off, &s_ip, 4) == 0) {
                off += rdlen;
                continue;
            }
            return true;
        }
        off += rdlen;
    }
    return false;
}

static void send_packet(size_t len)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(MDNS_PORT);
    dst.sin_addr.s_addr = htonl(MDNS_GROUP_ADDR);
    if (sendto(s_sock, s_pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        ESP_LOGW(TAG, "send failed");
    }
}

/* Standard probing: same question three times, any answer naming the alias is
 * a conflict. Runs before the first announcement so the audit is clean. */
static bool alias_conflict(void)
{
    uint8_t q[64];
    memset(q, 0, sizeof(q));
    q[5] = 1;                           /* qdcount */
    size_t off = 12;
    off += put_name(q + off, s_alias);
    off = put_u16(q, off, TYPE_A);
    off = put_u16(q, off, CLASS_IN);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(MDNS_PORT);
    dst.sin_addr.s_addr = htonl(MDNS_GROUP_ADDR);

    for (int round = 0; round < MDNS_PROBES; ++round) {
        (void)sendto(s_sock, q, off, 0, (struct sockaddr *)&dst, sizeof(dst));
        for (;;) {
            const int n = recvfrom(s_sock, s_pkt, sizeof(s_pkt), 0, NULL, NULL);
            if (n <= 0) break;
            if (response_claims(s_pkt, (size_t)n, s_alias)) return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_stop) return false;
    }
    return false;
}

static void mdns_task(void *arg)
{
    (void)arg;

    if (s_alias[0] && alias_conflict()) {
        ESP_LOGW(TAG, "hueleaf.local already answered by another host, keeping %s", s_host);
        s_alias[0] = 0;
    } else if (s_alias[0]) {
        ESP_LOGI(TAG, "alias hueleaf.local registered");
    }

    for (int i = 0; i < 2 && !s_stop; ++i) {
        size_t len = build_response(s_pkt, sizeof(s_pkt));
        if (len) send_packet(len);
        for (int wait = 0; wait < 10 && !s_stop; ++wait) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ESP_LOGI(TAG, "announced %s (_http._tcp:80)", s_host);

    while (!s_stop) {
        const int n = recvfrom(s_sock, s_pkt, sizeof(s_pkt), 0, NULL, NULL);
        if (n <= 0) continue;
        if ((s_pkt[2] & 0x80) || !query_matches(s_pkt, (size_t)n)) continue;
        const size_t len = build_response(s_pkt, sizeof(s_pkt));
        if (len) send_packet(len);
    }

    close(s_sock);
    s_sock = -1;
    s_task = NULL;
    ESP_LOGI(TAG, "stopped");
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t mdns_adv_start(esp_netif_t *netif)
{
    mdns_adv_stop();
    if (netif == NULL) return ESP_ERR_INVALID_ARG;

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    char host[40];
    netcfg_hostname(host, sizeof(host));
    snprintf(s_host, sizeof(s_host), "%s", host);
    snprintf(s_alias, sizeof(s_alias), "hueleaf.local");
    snprintf(s_instance, sizeof(s_instance), "HueLeaf E6._http._tcp.local");
    s_ip = info.ip.addr;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket failed");
        return ESP_FAIL;
    }
    int yes = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "bind 5353 failed");
        close(sock);
        return ESP_FAIL;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = htonl(MDNS_GROUP_ADDR);
    mreq.imr_interface.s_addr = info.ip.addr;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGW(TAG, "multicast join failed");
        close(sock);
        return ESP_FAIL;
    }
    struct in_addr iface;
    iface.s_addr = info.ip.addr;
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
    uint8_t ttl = 255;
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    uint8_t loop = 0;
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    /* recvfrom() timeouts bound the shutdown latency of stop(). */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (s_done == NULL) s_done = xSemaphoreCreateBinary();
    if (s_done == NULL) {
        close(sock);
        return ESP_ERR_NO_MEM;
    }
    /* Drop a token left by a task that exited before stop() ran. */
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {
    }
    s_sock = sock;
    s_stop = false;
    if (xTaskCreate(mdns_task, "mdns", 4096, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL;
        close(sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "start %s on " IPSTR, s_host, IP2STR(&info.ip));
    return ESP_OK;
}

void mdns_adv_stop(void)
{
    if (s_task == NULL) return;
    s_stop = true;
    (void)xSemaphoreTake(s_done, pdMS_TO_TICKS(1500));
}

bool mdns_adv_running(void)
{
    return s_task != NULL;
}

bool mdns_adv_alias_active(void)
{
    return s_alias[0] != 0;
}
