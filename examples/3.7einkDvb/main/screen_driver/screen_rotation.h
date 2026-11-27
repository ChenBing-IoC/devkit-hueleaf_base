#ifndef SCREEN_ROTATION_H
#define SCREEN_ROTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SCREEN_CACHE_OTP6, SCREEN_CACHE_CQ64, SCREEN_CACHE_CQ256,
    SCREEN_CACHE_CQ256_V2, SCREEN_CACHE_OTP384, SCREEN_CACHE_FAST64,
    SCREEN_CACHE_512_QUAL, SCREEN_CACHE_512_FAST,
} screen_cache_kind_t;

/* Lock order: rotation -> display -> board SPI. Never take rotation from a
 * display read/progress callback. HTTP and local UI own this lock per job. */
esp_err_t screen_rotation_init(void);
void screen_rotation_lock(void);
void screen_rotation_unlock(void);
unsigned screen_rotation_get(void);
uint32_t screen_rotation_epoch(void);
bool screen_rotation_busy(void);
bool screen_rotation_matches(uint32_t epoch);
void screen_rotation_size(unsigned rotation, int *width, int *height);
void screen_rotation_source_xy(unsigned rotation, int px, int py, int *x, int *y);
void screen_rotation_panel_xy(unsigned rotation, int x, int y, int *px, int *py);

/* Three picture slots live in the image_cache partition. Each keeps a header
 * and the verified frame, so the cycle of pictures survives a power cycle.
 * Call with the rotation lock held. slot selects the slot to replace, or -1
 * to take the one after the picture on the glass. */
esp_err_t screen_rotation_capture_begin(screen_cache_kind_t kind, const uint8_t blank[3], int slot);
uint32_t screen_rotation_source_generation(void);
esp_err_t screen_rotation_capture_write(size_t offset, const uint8_t *data, size_t length);
void screen_rotation_capture_end(bool success, uint32_t display_generation);
void screen_rotation_invalidate(void);
void screen_rotation_note_ui(void);
void screen_rotation_note_external(void);
void screen_rotation_note_blank(void);
/* A transient firmware notice (low power page) replaced the content on the
 * glass; the cached slots stay valid so it can be replayed later. */
void screen_rotation_note_notice(void);
bool screen_rotation_has_images(void);
unsigned screen_rotation_image_count(void);
int screen_rotation_current(void);
bool screen_rotation_slot_info(int index, unsigned *rotation);
/* RGB(A) thumbnail of a stored slot for the web page. */
esp_err_t screen_rotation_preview(int index, uint8_t *rgba, int width, int height);

/* Bottom text strip used to mark the awake state (the device IP). An empty or
 * NULL string disables it; it is never stored with the picture. */
void screen_rotation_set_overlay(const char *text);
const char *screen_rotation_overlay(void);
/* Replay the current picture with the configured strip. Does nothing when the
 * same picture and text are already on the glass. Caller holds the lock. */
esp_err_t screen_rotation_show_image(void);
/* Bring the next cached picture in slot order to the glass. overlay=true
 * keeps the awake address strip (side button); false is the idle entry that
 * clears it. Nothing is redrawn when the result is already on the glass.
 * Caller holds the lock. */
esp_err_t screen_rotation_show_next(bool overlay);
bool screen_rotation_hidden(void);
/* True while the panel shows an uploaded picture rather than a firmware page
 * or a blank fill. */
bool screen_rotation_shows_image(void);
esp_err_t screen_rotation_toggle_blank(void);

#endif
