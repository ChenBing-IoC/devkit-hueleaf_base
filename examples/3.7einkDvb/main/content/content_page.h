#ifndef CONTENT_PAGE_H
#define CONTENT_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define CONTENT_NOTE_LINES 2
#define CONTENT_NOTE_MAX   32
#define CONTENT_LABEL_MAX  16
#define CONTENT_DATE_MAX   10
#define CONTENT_WEATHER_MAX 23

/* Persisted under NVS namespace "content". The information page is a firmware
 * page: it never consumes a picture slot. */
typedef struct {
    bool enabled;                                     /* default true */
    char note[CONTENT_NOTE_LINES][CONTENT_NOTE_MAX + 1];
    char countdown_date[CONTENT_DATE_MAX + 1];        /* "YYYY-MM-DD" or "" */
    char countdown_label[CONTENT_LABEL_MAX + 1];
} content_config_t;

/* Load defaults when the namespace is missing. Always fills `out`. */
esp_err_t content_config_get(content_config_t *out);
/* Validates and stores all fields. Printable ASCII only; quotes and
 * backslashes are rejected so the JSON API can echo the values. */
esp_err_t content_config_set(const content_config_t *cfg);
/* True for printable ASCII (0x20..0x7e, no quote/backslash) within max. */
bool content_config_note_ok(const char *text, size_t max);

/* Weather block input for the network-content follow-up (#5b). Empty or NULL
 * restores the "--" placeholder. The text is part of the fingerprint. */
void content_page_set_weather(const char *text);

/* Fingerprint of everything the page shows except the live clock and the
 * status strip (date, lunar date, solar term, countdown, notes, weather). */
uint32_t content_page_fingerprint(bool with_status);

/* Caller holds screen_rotation_lock(). Compare with the fingerprint stored by
 * the last successful draw and refresh only when it differs. Returns true when
 * the glass was refreshed. */
bool content_page_show(bool with_status);
/* Same lock rule, but always refreshes (idle entry, manual refresh, unblank). */
bool content_page_redraw(bool with_status);
/* Cheap pre-check for a maintenance wake: would content_page_show() draw?
 * Reads NVS and the clock only; no panel access and no lock needed. */
bool content_page_needs_redraw(bool with_status);

typedef struct {
    uint32_t fingerprint;  /* last successfully drawn fingerprint, 0 = none */
    uint32_t drawn_at;     /* Unix time of that draw */
    bool refreshed;        /* did the most recent show/redraw touch the glass */
} content_status_t;

void content_status_get(content_status_t *out);

#endif /* CONTENT_PAGE_H */
