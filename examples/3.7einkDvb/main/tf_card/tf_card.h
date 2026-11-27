#ifndef TF_CARD_H
#define TF_CARD_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define TF_CARD_MOUNT_POINT "/sdcard"
#define TF_CARD_NFC_DIR     TF_CARD_MOUNT_POINT "/nfc"
#define TF_CARD_NFC_LOG     TF_CARD_NFC_DIR "/nfc_log.txt"
#define TF_CARD_NFC_LAST    TF_CARD_NFC_DIR "/nfc_last.txt"
#define TF_CARD_NFC_DUMP    TF_CARD_NFC_DIR "/nfc_dump.txt"
#define TF_CARD_NFC_BIN     TF_CARD_NFC_DIR "/nfc_dump.bin"
#define TF_CARD_EINK_DIR    TF_CARD_MOUNT_POINT "/eink"
#define TF_CARD_LOG_DIR     TF_CARD_MOUNT_POINT "/log"
#define TF_CARD_POWER_LOG   TF_CARD_LOG_DIR "/power.csv"

esp_err_t tf_card_mount(void);
/* Unmount so the slot and its SPI pull-ups stop drawing current in low power.
 * Idempotent. The caller must ensure no panel refresh is running (see
 * tf_card_wait_idle) unless it already holds the screen rotation lock. */
esp_err_t tf_card_unmount(void);
/* Mount on first use and retry after a failure instead of latching the error.
 * Waits for a running panel refresh to finish before touching the shared bus. */
esp_err_t tf_card_ensure_mounted(void);
/* Block until no panel job or refresh is running. */
void tf_card_wait_idle(void);
bool tf_card_is_mounted(void);
esp_err_t tf_card_write_text(const char *path, const char *text, bool append);
esp_err_t tf_card_begin_write(const char *path, bool append);
esp_err_t tf_card_write_raw(const void *data, size_t len);
esp_err_t tf_card_print(const char *text);
esp_err_t tf_card_end_write(void);

#endif /* TF_CARD_H */
