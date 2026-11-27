#ifndef SCREEN_UI_H
#define SCREEN_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t screen_ui_show_link_qr(const char *title, const char *url);
/* Offline provisioning page: text and address, without QR generation. */
esp_err_t screen_ui_show_setup(const char *url);
/* Centered message page (title + up to three lines, ASCII 5x7 font).
 * Used for the low power notice; empty lines are skipped. */
esp_err_t screen_ui_show_message(const char *title, const char *line1,
                                 const char *line2, const char *line3);

esp_err_t screen_ui_redraw_rotation(unsigned rotation);

/* Awake marker drawn over an uploaded picture: a white strip along the bottom
 * of the rotated display with the text in black, centred. Returns true when
 * (x,y) belongs to the strip and writes the ink id; false elsewhere. */
bool screen_ui_overlay_pixel(const char *text, unsigned rotation, int x, int y, uint8_t *ink);
#endif /* SCREEN_UI_H */
