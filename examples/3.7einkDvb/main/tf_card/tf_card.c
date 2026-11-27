#include "tf_card.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_map.h"
#include "screen_rotation.h"
#include "sdmmc_cmd.h"
#include "web_server.h"

static const char *TAG = "tf";

#define TF_SPI_KHZ  10000

static bool s_mounted;
static sdmmc_card_t *s_card;
static FILE *s_file;
static bool s_file_locked;

static esp_err_t write_locked(const char *path, const char *text, bool append)
{
    FILE *f = fopen(path, append ? "a" : "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_FAIL;
    }

    size_t len = strlen(text);
    size_t n = fwrite(text, 1, len, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (n != len) {
        ESP_LOGE(TAG, "write %s short %u/%u", path, (unsigned)n, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tf_card_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    gpio_reset_pin(spi2_tf_cs);
    gpio_set_pull_mode(spi2_tf_cs, GPIO_PULLUP_ONLY);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.max_freq_khz = TF_SPI_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = BOARD_SPI_HOST;
    slot.gpio_cs = spi2_tf_cs;
    slot.wait_for_miso = -1;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    board_spi_lock();
    esp_err_t err = esp_vfs_fat_sdspi_mount(TF_CARD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err == ESP_OK) {
        (void)mkdir(TF_CARD_NFC_DIR, 0777);
        (void)mkdir(TF_CARD_EINK_DIR, 0777);
    }
    board_spi_unlock();

    if (err != ESP_OK) {
        s_card = NULL;
        board_tf_cs_idle();
        ESP_LOGW(TAG, "mount failed: %s  (FAT32 TF on CS=%d)",
                 esp_err_to_name(err), (int)spi2_tf_cs);
        return err;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "mounted %s  MOSI=%d MISO=%d CLK=%d CS=%d",
             TF_CARD_MOUNT_POINT,
             (int)spi2_sdi, (int)spi2_sdo, (int)spi2_sck, (int)spi2_tf_cs);

    (void)tf_card_write_text(TF_CARD_NFC_LOG, "# NFC dump log\n\n", true);
    return ESP_OK;
}

bool tf_card_is_mounted(void)
{
    return s_mounted;
}

void tf_card_wait_idle(void)
{
    /* One six-ink refresh owns the shared SPI bus for ~23 s. Mounting or
     * unmounting in the middle of it would stall the panel transfer, so wait
     * for the screen to go idle first. The cap keeps a stuck job from hanging
     * the caller forever; the bus lock still serialises the actual access. */
    const TickType_t start = xTaskGetTickCount();
    while (web_server_screen_busy() || screen_rotation_busy()) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(60000)) {
            ESP_LOGW(TAG, "screen still busy after 60 s, touching the card anyway");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t tf_card_ensure_mounted(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    tf_card_wait_idle();
    /* A failed mount leaves s_mounted false, so the next call retries. */
    return tf_card_mount();
}

esp_err_t tf_card_unmount(void)
{
    /* Close a half-written dump first: end_write also releases the SPI lock. */
    (void)tf_card_end_write();
    if (!s_mounted && s_card == NULL) {
        return ESP_OK;
    }

    board_spi_lock();
    esp_err_t err = ESP_OK;
    if (s_card != NULL) {
        err = esp_vfs_fat_sdcard_unmount(TF_CARD_MOUNT_POINT, s_card);
    }
    board_spi_unlock();

    s_mounted = false;
    s_card = NULL;
    board_tf_cs_idle();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "unmounted %s, CS idle", TF_CARD_MOUNT_POINT);
    } else {
        ESP_LOGW(TAG, "unmount: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t tf_card_write_text(const char *path, const char *text, bool append)
{
    if (!s_mounted || path == NULL || text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    board_spi_lock();
    esp_err_t err = write_locked(path, text, append);
    board_spi_unlock();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved %s (%u bytes)", path, (unsigned)strlen(text));
    }
    return err;
}

esp_err_t tf_card_begin_write(const char *path, bool append)
{
    if (!s_mounted || path == NULL || s_file != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    board_spi_lock();
    s_file_locked = true;
    s_file = fopen(path, append ? "a" : "w");
    if (s_file == NULL) {
        s_file_locked = false;
        board_spi_unlock();
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tf_card_write_raw(const void *data, size_t len)
{
    if (s_file == NULL || data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (fwrite(data, 1, len, s_file) != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tf_card_print(const char *text)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return tf_card_write_raw(text, strlen(text));
}

esp_err_t tf_card_end_write(void)
{
    esp_err_t err = ESP_OK;
    if (s_file) {
        fflush(s_file);
        fsync(fileno(s_file));
        if (fclose(s_file) != 0) {
            err = ESP_FAIL;
        }
        s_file = NULL;
    }
    if (s_file_locked) {
        s_file_locked = false;
        board_spi_unlock();
    }
    return err;
}
