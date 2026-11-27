#include "content_page.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "axp2101.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lunar.h"
#include "nvs.h"
#include "power_mgr.h"
#include "screen_driver.h"
#include "screen_rotation.h"
#include "wifi_mgr.h"

static const char *TAG = "content";

#define CONTENT_NVS_NS     "content"
#define CONTENT_KEY_ENABLE "enabled"
#define CONTENT_KEY_NOTE0  "note0"
#define CONTENT_KEY_NOTE1  "note1"
#define CONTENT_KEY_DATE   "cd_date"
#define CONTENT_KEY_LABEL  "cd_label"
#define CONTENT_KEY_FP     "fp"
#define CONTENT_KEY_AT     "at"

/* Same 5x7 column-major font as screen_ui.c (bit0 = top row). */
static const uint8_t k_font[96][5] = {
    {0,0,0,0,0}, {0,0,0x5F,0,0}, {0,7,0,7,0}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50},
    {0,5,3,0,0}, {0,0x1C,0x22,0x41,0}, {0,0x41,0x22,0x1C,0}, {0x14,8,0x3E,8,0x14},
    {8,8,0x3E,8,8}, {0,0x50,0x30,0,0}, {8,8,8,8,8}, {0,0x60,0x60,0,0},
    {0x20,0x10,8,4,2}, {0x3E,0x51,0x49,0x45,0x3E}, {0,0x42,0x7F,0x40,0},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36}, {6,0x49,0x49,0x29,0x1E}, {0,0x36,0x36,0,0},
    {0,0x56,0x36,0,0}, {8,0x14,0x22,0x41,0}, {0x14,0x14,0x14,0x14,0x14},
    {0,0x41,0x22,0x14,8}, {2,1,0x51,9,6}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,9,9,9,1},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,8,8,8,0x7F}, {0,0x41,0x7F,0x41,0},
    {0x20,0x40,0x41,0x3F,1}, {0x7F,8,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,2,0x0C,2,0x7F}, {0x7F,4,8,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,9,9,9,6}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {1,1,0x7F,1,1}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7}, {0x61,0x51,0x49,0x45,0x43}, {0,0x7F,0x41,0x41,0},
    {2,4,8,0x10,0x20}, {0,0x41,0x41,0x7F,0}, {4,2,1,2,4}, {0x40,0x40,0x40,0x40,0x40},
    {0,1,2,4,0}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18},
    {8,0x7E,9,1,2}, {0x0C,0x52,0x52,0x52,0x3E}, {0x7F,8,4,4,0x78},
    {0,0x44,0x7D,0x40,0}, {0x20,0x40,0x44,0x3D,0}, {0x7F,0x10,0x28,0x44,0},
    {0,0x41,0x7F,0x40,0}, {0x7C,4,0x18,4,0x78}, {0x7C,8,4,4,0x78},
    {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,8}, {8,0x14,0x14,0x18,0x7C},
    {0x7C,8,4,4,8}, {0x48,0x54,0x54,0x54,0x20}, {4,0x3F,0x44,0x40,0x20},
    {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44},
    {0,8,0x36,0x41,0}, {0,0,0x7F,0,0}, {0,0x41,0x36,8,0}, {8,4,8,0x10,8}, {0,0,0,0,0},
};

#define FONT_W 5
#define FONT_H 7
#define CELL_W (FONT_W + 1)

#define SCALE_DATE   4
#define SCALE_CLOCK  4
#define SCALE_SMALL  3
#define SCALE_COUNT  4
#define SCALE_NOTE   2
#define SCALE_STATUS 2

static const char *const k_weekday[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};
static const char *const k_month[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

/* Weather block; weather_publish_page() fills it from the cache before a draw.
 * Empty detail and forecast select the single-line placeholder. */
static char s_weather_main[CONTENT_WEATHER_MAIN_MAX + 1] = "WEATHER --";
static char s_weather_detail[CONTENT_WEATHER_DETAIL_MAX + 1];
static char s_weather_forecast[CONTENT_WEATHER_FORECAST_MAX + 1];
static volatile bool s_refreshed;

/* Strings shown by the current draw, independent of layout. */
typedef struct {
    char iso[11];
    char date[24];
    char clock[16];
    char lunar[24];
    char term[24];
    char weather[24];
    char weather_detail[40];
    char weather_forecast[80];
    char countdown[28];
    char label[20];
    char note[CONTENT_NOTE_LINES][34];
    char status[64];
    bool has_label;
    bool has_note[CONTENT_NOTE_LINES];
    bool has_countdown;
    bool with_status;
    bool lunar_valid;
    bool lunar_leap;
    bool low;          /* soc < 15 %: highlight the status strip */
    bool charge_me;    /* soc < 8 %: extra red line */
    int lunar_year, lunar_month, lunar_day;
    int days;
    int soc;           /* -1 when the gauge cannot be read */
} content_snapshot_t;

static content_snapshot_t s_snap;

/* Layout in the rotated logical space; the stream maps panel pixels into it. */
typedef struct {
    int ui_w, ui_h;
    int date_x, date_y, date_w;
    int clock_x, clock_y, clock_w;
    int lunar_x, lunar_y, lunar_w;
    int term_x, term_y, term_w;
    int weather_x, weather_y, weather_w;
    int detail_x, detail_y, detail_w, detail_scale;
    int forecast_x, forecast_y, forecast_w, forecast_scale;
    bool weather_split;
    int count_x, count_y, count_w;
    int label_x, label_y, label_w;
    int note_x, note0_y, note1_y, note_w[CONTENT_NOTE_LINES];
    int status_x, status_y, status_w;
    int charge_x, charge_y, charge_w;
    int box_x0, box_y0, box_x1, box_y1;
} page_layout_t;

static page_layout_t s_layout;

/* 360-byte byte-column cache for one panel gate; no frame buffer. */
typedef struct {
    unsigned rotation;
    int gate;
    uint8_t col[SCREEN_HEIGHT / 2];
    size_t sent;
} page_stream_t;

static page_stream_t s_stream;

/* ---- Small 5x7 text engine ----------------------------------------------- */

static int font_index(char c)
{
    if (c < 32 || c > 126) return 0;
    return c - 32;
}

static bool glyph_pixel(char c, int gx, int gy)
{
    if (gx < 0 || gx >= FONT_W || gy < 0 || gy >= FONT_H) return false;
    return ((k_font[font_index(c)][gx] >> gy) & 1) != 0;
}

static int text_width(const char *text, int scale)
{
    const int length = (int)strlen(text);
    return length > 0 ? length * CELL_W * scale - scale : 0;
}

static bool glyph_at(const char *text, int width, int x0, int y0, int scale,
                     int x, int y)
{
    if (!text || !text[0] || width <= 0) return false;
    if (x < x0 || x >= x0 + width) return false;
    if (y < y0 || y >= y0 + FONT_H * scale) return false;
    const int lx = (x - x0) / scale;
    const int index = lx / CELL_W;
    const int ox = lx % CELL_W;
    if (index < 0 || ox >= FONT_W) return false;
    return glyph_pixel(text[index], ox, (y - y0) / scale);
}

static int centered_x(int width, int ui_w)
{
    const int x = (ui_w - width) / 2;
    return x < 0 ? 0 : x;
}

static void copy_text(char *dst, size_t size, const char *src)
{
    size_t length = strlen(src);
    if (length >= size) length = size - 1;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

/* Printable ASCII only; anything else becomes a space and trailing spaces are
 * dropped, so a malformed source cannot paint a control glyph. */
static void copy_ascii(char *dst, size_t size, const char *src)
{
    size_t length = src ? strlen(src) : 0;
    if (length >= size) length = size - 1;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
    }
    dst[length] = '\0';
    while (length > 0 && dst[length - 1] == ' ') dst[--length] = '\0';
}

/* ---- Configuration ------------------------------------------------------- */

static void nvs_read_str(nvs_handle_t nvs, const char *key, char *dst, size_t size)
{
    size_t length = size;
    if (nvs_get_str(nvs, key, dst, &length) != ESP_OK) {
        dst[0] = '\0';
    }
}

static bool text_ok(const char *text, size_t max)
{
    if (!text) return false;
    const size_t length = strlen(text);
    if (length > max) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

bool content_config_note_ok(const char *text, size_t max)
{
    return text_ok(text, max);
}

esp_err_t content_config_get(content_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->enabled = true;

    nvs_handle_t nvs;
    const esp_err_t err = nvs_open(CONTENT_NVS_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t enabled = 1;
    if (nvs_get_u8(nvs, CONTENT_KEY_ENABLE, &enabled) == ESP_OK) {
        out->enabled = enabled != 0;
    }
    nvs_read_str(nvs, CONTENT_KEY_NOTE0, out->note[0], sizeof(out->note[0]));
    nvs_read_str(nvs, CONTENT_KEY_NOTE1, out->note[1], sizeof(out->note[1]));
    nvs_read_str(nvs, CONTENT_KEY_DATE, out->countdown_date,
                 sizeof(out->countdown_date));
    nvs_read_str(nvs, CONTENT_KEY_LABEL, out->countdown_label,
                 sizeof(out->countdown_label));
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t content_config_set(const content_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < CONTENT_NOTE_LINES; ++i) {
        if (!text_ok(cfg->note[i], CONTENT_NOTE_MAX)) return ESP_ERR_INVALID_ARG;
    }
    if (!text_ok(cfg->countdown_label, CONTENT_LABEL_MAX)) return ESP_ERR_INVALID_ARG;
    if (cfg->countdown_date[0] && !lunar_parse_date(cfg->countdown_date, NULL, NULL, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(cfg->countdown_date) > CONTENT_DATE_MAX) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CONTENT_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, CONTENT_KEY_ENABLE, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(nvs, CONTENT_KEY_NOTE0, cfg->note[0]);
    if (err == ESP_OK) err = nvs_set_str(nvs, CONTENT_KEY_NOTE1, cfg->note[1]);
    if (err == ESP_OK) err = nvs_set_str(nvs, CONTENT_KEY_DATE, cfg->countdown_date);
    if (err == ESP_OK) err = nvs_set_str(nvs, CONTENT_KEY_LABEL, cfg->countdown_label);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

void content_page_set_weather_lines(const char *main_line, const char *detail_line,
                                    const char *forecast_line)
{
    char main_text[CONTENT_WEATHER_MAIN_MAX + 1];
    char detail_text[CONTENT_WEATHER_DETAIL_MAX + 1];
    char forecast_text[CONTENT_WEATHER_FORECAST_MAX + 1];
    copy_ascii(main_text, sizeof(main_text), main_line);
    copy_ascii(detail_text, sizeof(detail_text), detail_line);
    copy_ascii(forecast_text, sizeof(forecast_text), forecast_line);

    if (main_text[0] == '\0') {
        copy_text(s_weather_main, sizeof(s_weather_main), "WEATHER --");
        s_weather_detail[0] = '\0';
        s_weather_forecast[0] = '\0';
        return;
    }
    copy_text(s_weather_main, sizeof(s_weather_main), main_text);
    copy_text(s_weather_detail, sizeof(s_weather_detail), detail_text);
    copy_text(s_weather_forecast, sizeof(s_weather_forecast), forecast_text);
}

void content_page_set_weather(const char *text)
{
    content_page_set_weather_lines(text, NULL, NULL);
}

/* ---- Snapshot and fingerprint -------------------------------------------- */

static void snapshot_build(const content_config_t *cfg, bool with_status,
                           content_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->with_status = with_status;
    /* Battery is part of the fingerprint only as a coarse bucket, so a 10 %
     * step (and the two warning thresholds) triggers one redraw on the next
     * maintenance wake instead of a refresh every minute. */
    const int soc = axp2101_battery_percent();
    snap->soc = soc >= 0 ? soc : -1;
    snap->low = snap->soc >= 0 && snap->soc < 15;
    snap->charge_me = snap->soc >= 0 && snap->soc < 8;
    snap->has_label = cfg->countdown_label[0] != '\0';
    copy_text(snap->label, sizeof(snap->label), cfg->countdown_label);
    for (int i = 0; i < CONTENT_NOTE_LINES; ++i) {
        copy_text(snap->note[i], sizeof(snap->note[i]), cfg->note[i]);
        snap->has_note[i] = snap->note[i][0] != '\0';
    }
    copy_text(snap->weather, sizeof(snap->weather), s_weather_main);
    copy_text(snap->weather_detail, sizeof(snap->weather_detail), s_weather_detail);
    copy_text(snap->weather_forecast, sizeof(snap->weather_forecast), s_weather_forecast);

    time_t now = time(NULL);
    struct tm local;
    if (now <= 0 || localtime_r(&now, &local) == NULL) {
        memset(&local, 0, sizeof(local));
        local.tm_year = 70;
        local.tm_mday = 1;
        local.tm_wday = 4;
    }
    const int year = local.tm_year + 1900;
    const int month = local.tm_mon + 1;
    const int day = local.tm_mday;

    char work[64];
    (void)snprintf(work, sizeof(work), "%04d-%02d-%02d", year, month, day);
    copy_text(snap->iso, sizeof(snap->iso), work);

    const int weekday = ((local.tm_wday % 7) + 7) % 7;
    (void)snprintf(work, sizeof(work), "%.3s %02d %.3s %04d",
                   k_weekday[weekday], day, k_month[(month - 1 + 12) % 12], year);
    copy_text(snap->date, sizeof(snap->date), work);

    (void)snprintf(work, sizeof(work), "UPDATED %02d:%02d", local.tm_hour, local.tm_min);
    copy_text(snap->clock, sizeof(snap->clock), work);

    lunar_date_t lunar;
    if (lunar_from_gregorian(year, month, day, &lunar)) {
        snap->lunar_valid = true;
        snap->lunar_year = lunar.year;
        snap->lunar_month = lunar.month;
        snap->lunar_day = lunar.day;
        snap->lunar_leap = lunar.leap;
        (void)snprintf(work, sizeof(work), "LUNAR %s%d-%d",
                       lunar.leap ? "L " : "", lunar.month, lunar.day);
        copy_text(snap->lunar, sizeof(snap->lunar), work);
    } else {
        copy_text(snap->lunar, sizeof(snap->lunar), "LUNAR --");
    }

    char term[24];
    if (lunar_solar_term_name(year, month, day, term, sizeof(term))) {
        copy_text(snap->term, sizeof(snap->term), term);
    } else {
        copy_text(snap->term, sizeof(snap->term), "TERM --");
    }

    int cd_year = 0, cd_month = 0, cd_day = 0;
    if (cfg->countdown_date[0] &&
        lunar_parse_date(cfg->countdown_date, &cd_year, &cd_month, &cd_day)) {
        const int days = lunar_days_until(cd_year, cd_month, cd_day);
        snap->has_countdown = true;
        snap->days = days;
        if (days >= 0) {
            (void)snprintf(work, sizeof(work), "DAYS LEFT %d", days);
        } else {
            (void)snprintf(work, sizeof(work), "DAYS AGO %d", -days);
        }
        copy_text(snap->countdown, sizeof(snap->countdown), work);
    } else {
        copy_text(snap->countdown, sizeof(snap->countdown), "DAYS LEFT --");
    }
}

static uint32_t hash_str(uint32_t hash, const char *text)
{
    for (const char *p = text; p && *p; ++p) {
        hash = (hash ^ (uint8_t)*p) * 16777619U;
    }
    return hash;
}

static uint32_t hash_int(uint32_t hash, int value)
{
    char text[24];
    (void)snprintf(text, sizeof(text), "%d", value);
    return hash_str(hash, text);
}

static uint32_t snapshot_hash(const content_snapshot_t *snap)
{
    uint32_t hash = 2166136261U;
    hash = hash_str(hash, "content-v3");
    hash = hash_int(hash, snap->with_status ? 1 : 0);
    /* Coarse battery state only: one redraw per 10 % step, plus the exact
     * crossings of the 15 % (LOW) and 8 % (CHARGE ME) thresholds. */
    hash = hash_int(hash, snap->soc >= 0 ? snap->soc / 10 : -1);
    hash = hash_int(hash, snap->low ? 1 : 0);
    hash = hash_int(hash, snap->charge_me ? 1 : 0);
    hash = hash_str(hash, snap->iso);
    hash = hash_int(hash, snap->lunar_valid ? 1 : 0);
    hash = hash_int(hash, snap->lunar_year);
    hash = hash_int(hash, snap->lunar_month);
    hash = hash_int(hash, snap->lunar_day);
    hash = hash_int(hash, snap->lunar_leap ? 1 : 0);
    hash = hash_str(hash, snap->term);
    hash = hash_int(hash, snap->has_countdown ? 1 : 0);
    hash = hash_int(hash, snap->days);
    hash = hash_str(hash, snap->label);
    hash = hash_str(hash, snap->note[0]);
    hash = hash_str(hash, snap->note[1]);
    hash = hash_str(hash, snap->weather);
    hash = hash_str(hash, snap->weather_detail);
    hash = hash_str(hash, snap->weather_forecast);
    return hash;
}

uint32_t content_page_fingerprint(bool with_status)
{
    content_config_t cfg;
    (void)content_config_get(&cfg);
    content_snapshot_t snap;
    snapshot_build(&cfg, with_status, &snap);
    return snapshot_hash(&snap);
}

/* ---- Layout and pixel fill ----------------------------------------------- */

/* Drops trailing words until the text fits max_width at scale. The weather
 * forecast loses whole days ("FRI 21/10") instead of being cut mid-number. */
static void fit_text(char *text, int max_width, int scale)
{
    size_t length = strlen(text);
    while (length > 0 && text_width(text, scale) > max_width) {
        size_t cut = length;
        while (cut > 0 && text[cut - 1] != ' ') --cut;
        if (cut > 1) {
            text[cut - 1] = '\0';
            length = cut - 1;
        } else {
            text[--length] = '\0';
        }
    }
}

static void layout(const content_snapshot_t *snap, int ui_w, int ui_h)
{
    s_snap = *snap;
    memset(&s_layout, 0, sizeof(s_layout));
    s_layout.ui_w = ui_w;
    s_layout.ui_h = ui_h;

    s_layout.date_y = ui_h * 5 / 100;
    s_layout.date_w = text_width(s_snap.date, SCALE_DATE);
    s_layout.date_x = centered_x(s_layout.date_w, ui_w);

    s_layout.clock_y = ui_h * 13 / 100;
    s_layout.clock_w = text_width(s_snap.clock, SCALE_CLOCK);
    s_layout.clock_x = centered_x(s_layout.clock_w, ui_w);

    s_layout.lunar_y = ui_h * 24 / 100;
    s_layout.lunar_w = text_width(s_snap.lunar, SCALE_SMALL);
    s_layout.lunar_x = centered_x(s_layout.lunar_w, ui_w);

    s_layout.term_y = ui_h * 30 / 100;
    s_layout.term_w = text_width(s_snap.term, SCALE_SMALL);
    s_layout.term_x = centered_x(s_layout.term_w, ui_w);

    s_layout.box_x0 = 40;
    s_layout.box_x1 = ui_w - 40;
    s_layout.box_y0 = ui_h * 37 / 100;
    s_layout.box_y1 = ui_h * 58 / 100;
    const int box_h = s_layout.box_y1 - s_layout.box_y0;
    const int inner_w = (s_layout.box_x1 - s_layout.box_x0) - 20;
    s_layout.weather_split = s_snap.weather_detail[0] != '\0' ||
                             s_snap.weather_forecast[0] != '\0';
    s_layout.weather_w = text_width(s_snap.weather, SCALE_DATE);
    s_layout.weather_x = centered_x(s_layout.weather_w, ui_w);
    s_layout.detail_scale = SCALE_SMALL;
    s_layout.forecast_scale = SCALE_NOTE;
    if (!s_layout.weather_split) {
        s_layout.weather_y = s_layout.box_y0 +
            (box_h - FONT_H * SCALE_DATE) / 2;
    } else {
        /* Main line large on top, detail in the middle, forecast at the
         * bottom. Every line is fitted inside the box before drawing. */
        s_layout.detail_w = text_width(s_snap.weather_detail, s_layout.detail_scale);
        if (s_layout.detail_w > inner_w) {
            s_layout.detail_scale = SCALE_NOTE;
            s_layout.detail_w = text_width(s_snap.weather_detail, s_layout.detail_scale);
        }
        if (s_layout.detail_w > inner_w) {
            fit_text(s_snap.weather_detail, inner_w, s_layout.detail_scale);
            s_layout.detail_w = text_width(s_snap.weather_detail, s_layout.detail_scale);
        }
        s_layout.detail_x = centered_x(s_layout.detail_w, ui_w);

        s_layout.forecast_w = text_width(s_snap.weather_forecast, s_layout.forecast_scale);
        if (s_layout.forecast_w > inner_w) {
            fit_text(s_snap.weather_forecast, inner_w, s_layout.forecast_scale);
            s_layout.forecast_w = text_width(s_snap.weather_forecast, s_layout.forecast_scale);
        }
        s_layout.forecast_x = centered_x(s_layout.forecast_w, ui_w);

        const int main_h = FONT_H * SCALE_DATE;
        const int detail_h = FONT_H * s_layout.detail_scale;
        const int forecast_h = FONT_H * s_layout.forecast_scale;
        int gap = (box_h - (main_h + detail_h + forecast_h)) / 4;
        if (gap < 4) gap = 4;
        s_layout.weather_y = s_layout.box_y0 + gap;
        s_layout.detail_y = s_layout.weather_y + main_h + gap;
        s_layout.forecast_y = s_layout.detail_y + detail_h + gap;
    }

    s_layout.count_y = ui_h * 63 / 100;
    s_layout.count_w = text_width(s_snap.countdown, SCALE_COUNT);
    s_layout.count_x = centered_x(s_layout.count_w, ui_w);

    s_layout.label_y = ui_h * 70 / 100;
    s_layout.label_w = text_width(s_snap.label, SCALE_SMALL);
    s_layout.label_x = centered_x(s_layout.label_w, ui_w);

    s_layout.note_x = 40;
    s_layout.note0_y = ui_h * 78 / 100;
    s_layout.note1_y = ui_h * 84 / 100;
    for (int i = 0; i < CONTENT_NOTE_LINES; ++i) {
        s_layout.note_w[i] = text_width(s_snap.note[i], SCALE_NOTE);
    }

    /* Status strip: battery, USB, address (awake only) and next wake. */
    char status[64];
    char work[32];
    const int soc = s_snap.soc;
    if (soc >= 0) {
        (void)snprintf(work, sizeof(work), "BATT %d%%", soc);
    } else {
        copy_text(work, sizeof(work), "BATT --");
    }
    copy_text(status, sizeof(status), work);
    if (s_snap.low) {
        strlcat(status, " LOW", sizeof(status));
    }
    if (axp2101_is_usb_connected()) {
        strlcat(status, " USB", sizeof(status));
    }
    if (s_snap.with_status) {
        char ip[16];
        wifi_mgr_get_ip(ip, sizeof(ip));
        if (ip[0]) {
            strlcat(status, " ", sizeof(status));
            strlcat(status, ip, sizeof(status));
        }
    }
    const power_mgr_config_t policy = power_mgr_config();
    if (policy.interval_min > 0) {
        (void)snprintf(work, sizeof(work), " NEXT %uM", (unsigned)policy.interval_min);
        strlcat(status, work, sizeof(status));
    }
    copy_text(s_snap.status, sizeof(s_snap.status), status);
    s_layout.status_y = ui_h * 92 / 100;
    s_layout.status_w = text_width(s_snap.status, SCALE_STATUS);
    s_layout.status_x = centered_x(s_layout.status_w, ui_w);

    /* Critical battery: one red line under the status strip. */
    s_layout.charge_w = 0;
    if (s_snap.charge_me) {
        s_layout.charge_w = text_width("CHARGE ME", SCALE_SMALL);
        s_layout.charge_x = centered_x(s_layout.charge_w, ui_w);
        s_layout.charge_y = ui_h * 96 / 100;
    }
}

static uint8_t page_pixel(int x, int y)
{
    const page_layout_t *l = &s_layout;
    if (x >= l->box_x0 && x < l->box_x1 && y >= l->box_y0 && y < l->box_y1 &&
        (x < l->box_x0 + 3 || x >= l->box_x1 - 3 ||
         y < l->box_y0 + 3 || y >= l->box_y1 - 3)) {
        return SCREEN_COLOR_RED;
    }
    if (glyph_at(s_snap.date, l->date_w, l->date_x, l->date_y, SCALE_DATE, x, y)) {
        return SCREEN_COLOR_BLACK;
    }
    if (glyph_at(s_snap.clock, l->clock_w, l->clock_x, l->clock_y, SCALE_CLOCK, x, y)) {
        return SCREEN_COLOR_RED;
    }
    if (glyph_at(s_snap.lunar, l->lunar_w, l->lunar_x, l->lunar_y, SCALE_SMALL, x, y)) {
        return SCREEN_COLOR_BLACK;
    }
    if (glyph_at(s_snap.term, l->term_w, l->term_x, l->term_y, SCALE_SMALL, x, y)) {
        return SCREEN_COLOR_RED;
    }
    if (glyph_at(s_snap.weather, l->weather_w, l->weather_x, l->weather_y,
                 SCALE_DATE, x, y)) {
        return SCREEN_COLOR_BLACK;
    }
    if (l->weather_split &&
        glyph_at(s_snap.weather_detail, l->detail_w, l->detail_x, l->detail_y,
                 l->detail_scale, x, y)) {
        return SCREEN_COLOR_BLACK;
    }
    if (l->weather_split &&
        glyph_at(s_snap.weather_forecast, l->forecast_w, l->forecast_x,
                 l->forecast_y, l->forecast_scale, x, y)) {
        return SCREEN_COLOR_BLACK;
    }
    if (glyph_at(s_snap.countdown, l->count_w, l->count_x, l->count_y,
                 SCALE_COUNT, x, y)) {
        return SCREEN_COLOR_BLACK;
    }
    if (s_snap.has_label &&
        glyph_at(s_snap.label, l->label_w, l->label_x, l->label_y, SCALE_SMALL, x, y)) {
        return SCREEN_COLOR_BLACK;
    }
    for (int i = 0; i < CONTENT_NOTE_LINES; ++i) {
        if (!s_snap.has_note[i]) continue;
        const int y0 = i == 0 ? l->note0_y : l->note1_y;
        if (glyph_at(s_snap.note[i], l->note_w[i], l->note_x, y0,
                     SCALE_NOTE, x, y)) {
            return SCREEN_COLOR_BLACK;
        }
    }
    if (glyph_at(s_snap.status, l->status_w, l->status_x, l->status_y,
                 SCALE_STATUS, x, y)) {
        /* A low pack paints the whole status strip red, including the LOW tag. */
        return s_snap.low ? SCREEN_COLOR_RED : SCREEN_COLOR_BLACK;
    }
    if (l->charge_w > 0 &&
        glyph_at("CHARGE ME", l->charge_w, l->charge_x, l->charge_y,
                 SCALE_SMALL, x, y)) {
        return SCREEN_COLOR_RED;
    }
    return SCREEN_COLOR_WHITE;
}

/* Frame order: gate-major, byte row r holds panel y = 2r (high nibble) and
 * 2r+1 (low nibble). One 360-byte gate column is reused for the whole frame. */
static esp_err_t page_read(void *context, uint8_t *buffer, size_t capacity,
                           size_t *received)
{
    page_stream_t *stream = context;
    if (!buffer || !capacity || !received) return ESP_ERR_INVALID_ARG;
    size_t count = SCREEN_FRAME_BYTES - stream->sent;
    if (count > capacity) count = capacity;
    const int rows = SCREEN_HEIGHT / 2;
    for (size_t i = 0; i < count; ++i) {
        const size_t position = stream->sent + i;
        const int gate = (int)(position / (size_t)rows);
        const int row = (int)(position % (size_t)rows);
        if (gate != stream->gate) {
            stream->gate = gate;
            const int px = SCREEN_WIDTH - 1 - gate;
            for (int r = 0; r < rows; ++r) {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                screen_rotation_source_xy(stream->rotation, px, r * 2, &x0, &y0);
                screen_rotation_source_xy(stream->rotation, px, r * 2 + 1, &x1, &y1);
                const uint8_t high = page_pixel(x0, y0);
                const uint8_t low = page_pixel(x1, y1);
                stream->col[r] = (uint8_t)((high << 4) | low);
            }
        }
        buffer[i] = stream->col[row];
    }
    stream->sent += count;
    *received = count;
    return ESP_OK;
}

static bool content_redraw_cb(bool with_status);

static bool draw_snapshot(const content_snapshot_t *snap)
{
    const unsigned rotation = screen_rotation_get();
    int ui_w = 0, ui_h = 0;
    screen_rotation_size(rotation, &ui_w, &ui_h);
    layout(snap, ui_w, ui_h);

    s_stream.rotation = rotation;
    s_stream.gate = -1;
    s_stream.sent = 0;

    screen_rotation_set_content_redraw(content_redraw_cb);
    screen_rotation_note_external();
    const esp_err_t err = screen_driver_show_stream(page_read, &s_stream);
    if (err == ESP_OK) {
        screen_rotation_note_content();
    } else {
        ESP_LOGE(TAG, "refresh failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

/* ---- Persisted fingerprint ----------------------------------------------- */

static bool state_load(uint32_t *fingerprint, uint32_t *drawn_at)
{
    nvs_handle_t nvs;
    if (nvs_open(CONTENT_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return false;
    uint32_t fp = 0, at = 0;
    const esp_err_t fp_err = nvs_get_u32(nvs, CONTENT_KEY_FP, &fp);
    const esp_err_t at_err = nvs_get_u32(nvs, CONTENT_KEY_AT, &at);
    nvs_close(nvs);
    if (fp_err != ESP_OK) return false;
    if (fingerprint) *fingerprint = fp;
    if (drawn_at && at_err == ESP_OK) *drawn_at = at;
    return true;
}

static void state_save(uint32_t fingerprint)
{
    nvs_handle_t nvs;
    if (nvs_open(CONTENT_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    const uint32_t at = (uint32_t)time(NULL);
    esp_err_t err = nvs_set_u32(nvs, CONTENT_KEY_FP, fingerprint);
    if (err == ESP_OK) err = nvs_set_u32(nvs, CONTENT_KEY_AT, at);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fingerprint save: %s", esp_err_to_name(err));
    }
}

/* ---- Public entry points ------------------------------------------------- */

bool content_page_show(bool with_status)
{
    content_config_t cfg;
    (void)content_config_get(&cfg);
    if (!cfg.enabled) {
        __atomic_store_n(&s_refreshed, false, __ATOMIC_RELAXED);
        return false;
    }
    content_snapshot_t snap;
    snapshot_build(&cfg, with_status, &snap);
    const uint32_t fingerprint = snapshot_hash(&snap);

    uint32_t stored = 0;
    if (state_load(&stored, NULL) && stored == fingerprint) {
        __atomic_store_n(&s_refreshed, false, __ATOMIC_RELAXED);
        ESP_LOGI(TAG, "content unchanged fp=%08x", (unsigned)fingerprint);
        return false;
    }
    if (!draw_snapshot(&snap)) {
        __atomic_store_n(&s_refreshed, false, __ATOMIC_RELAXED);
        return false;
    }
    state_save(fingerprint);
    __atomic_store_n(&s_refreshed, true, __ATOMIC_RELAXED);
    ESP_LOGI(TAG, "content refreshed fp=%08x status=%d", (unsigned)fingerprint,
             (int)with_status);
    return true;
}

bool content_page_redraw(bool with_status)
{
    content_config_t cfg;
    (void)content_config_get(&cfg);
    content_snapshot_t snap;
    snapshot_build(&cfg, with_status, &snap);
    const uint32_t fingerprint = snapshot_hash(&snap);
    if (!draw_snapshot(&snap)) {
        __atomic_store_n(&s_refreshed, false, __ATOMIC_RELAXED);
        return false;
    }
    state_save(fingerprint);
    __atomic_store_n(&s_refreshed, true, __ATOMIC_RELAXED);
    ESP_LOGI(TAG, "content drawn fp=%08x status=%d", (unsigned)fingerprint,
             (int)with_status);
    return true;
}

bool content_page_needs_redraw(bool with_status)
{
    content_config_t cfg;
    (void)content_config_get(&cfg);
    if (!cfg.enabled) return false;
    content_snapshot_t snap;
    snapshot_build(&cfg, with_status, &snap);
    uint32_t stored = 0;
    if (!state_load(&stored, NULL)) return true;
    return stored != snapshot_hash(&snap);
}

static bool content_redraw_cb(bool with_status)
{
    return content_page_redraw(with_status);
}

void content_status_get(content_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    (void)state_load(&out->fingerprint, &out->drawn_at);
    out->refreshed = __atomic_load_n(&s_refreshed, __ATOMIC_RELAXED);
}
