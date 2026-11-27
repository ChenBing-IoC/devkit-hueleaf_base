#include "screen_driver.h"

#include <stdbool.h>
#include <string.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_lut.h"
#include "pin_map.h"
#include "panel_profile.h"
#include "screen_rotation.h"

static const char *TAG = "screen";

#define EINK_SPI_CLOCK_HZ     EPD037_SPI_HZ
#define EINK_CHUNK_BYTES      2048
#define EINK_BUSY_TIMEOUT_MS  EPD037_BUSY_TIMEOUT_MS

/* 固定 DMA 暂存区；HTTP 缓冲无需占用 DMA 堆。由屏幕锁保护。 */
static DMA_ATTR uint8_t s_spi_tx[EINK_CHUNK_BYTES];

typedef struct {
    spi_device_handle_t spi;
    SemaphoreHandle_t lock;
    bool inited;
} screen_ctx_t;

static screen_ctx_t s_ctx;
static uint32_t s_display_generation = 1;
uint32_t screen_driver_generation(void)
{
    return __atomic_load_n(&s_display_generation, __ATOMIC_RELAXED);
}
static void invalidate_display(void)
{
    if (__atomic_add_fetch(&s_display_generation, 1, __ATOMIC_RELAXED) == 0) {
        (void)__atomic_add_fetch(&s_display_generation, 1, __ATOMIC_RELAXED);
    }
}

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t wait_busy(void)
{
    const TickType_t started = xTaskGetTickCount();
    while (gpio_get_level(eink_busy) == 0) {
        if ((TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(EINK_BUSY_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "BUSY timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

/* Every DRF must produce an observed low -> high BUSY cycle. Never turn a
 * missing assertion into a successful refresh after an arbitrary delay.
 * asserted reports whether BUSY was actually seen low, even on timeout. */
static esp_err_t wait_refresh_ex(bool *asserted)
{
    if (asserted) *asserted = false;
    const TickType_t t0 = xTaskGetTickCount();
    while (gpio_get_level(eink_busy) != 0) {
        if ((TickType_t)(xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(EPD037_DRF_ASSERT_MS)) {
            ESP_LOGE(TAG, "DRF: no BUSY assertion observed");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    if (asserted) *asserted = true;
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "DRF busy");
    ESP_LOGI(TAG, "DRF busy %ums",
             (unsigned)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
    return ESP_OK;
}

static esp_err_t wait_refresh(void)
{
    return wait_refresh_ex(NULL);
}

static void lcd_spi_pre_transfer_callback(spi_transaction_t *t)
{
    int dc = (int)t->user;
    gpio_set_level(eink_dc, dc);
}

static esp_err_t write_cmd(uint8_t cmd)
{
    /* Any RAM write or reset/sleep invalidates browser-side region baselines,
     * including boot QR and non-HTTP display operations. */
    if (cmd == 0x10 || cmd == 0xE9 || cmd == 0x07) {
        invalidate_display();
    }
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void *)0,
    };
    return spi_device_polling_transmit(s_ctx.spi, &t);
}

static esp_err_t write_data(const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t chunk = len > EINK_CHUNK_BYTES ? EINK_CHUNK_BYTES : len;
        /* 所有来源均复制到固定 DMA 区，避免 DMA 能力/对齐差异。 */
        if (data != s_spi_tx) {
            memcpy(s_spi_tx, data, chunk);
        }
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = s_spi_tx,
            .user = (void *)1,
        };
        esp_err_t err = spi_device_polling_transmit(s_ctx.spi, &t);
        if (err != ESP_OK) {
            return err;
        }
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static esp_err_t write_data_byte(uint8_t data)
{
    return write_data(&data, 1);
}

static esp_err_t write_solid(uint8_t pixel_byte, size_t count)
{
    memset(s_spi_tx, pixel_byte, EINK_CHUNK_BYTES);

    esp_err_t err = ESP_OK;
    while (count > 0) {
        size_t n = count > EINK_CHUNK_BYTES ? EINK_CHUNK_BYTES : count;
        err = write_data(s_spi_tx, n);
        if (err != ESP_OK) {
            break;
        }
        count -= n;
    }
    return err;
}

static esp_err_t hw_reset(void)
{
    invalidate_display();
    gpio_set_level(eink_rst, 0);
    delay_ms(20);
    gpio_set_level(eink_rst, 1);
    delay_ms(10);
    gpio_set_level(eink_rst, 0);
    delay_ms(20);
    gpio_set_level(eink_rst, 1);
    delay_ms(10);
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "reset busy");
    delay_ms(10);
    return ESP_OK;
}

/* Panel-specific sequence; shared IST opcodes do not imply that another
 * panel's extended initialization or waveform is compatible. */
static esp_err_t panel_init(void)
{
    for (size_t i = 0; i < sizeof(k_epd037_init) / sizeof(k_epd037_init[0]); ++i) {
        ESP_RETURN_ON_ERROR(write_cmd(k_epd037_init[i].command), TAG, "panel init command");
        ESP_RETURN_ON_ERROR(write_data_byte(k_epd037_init[i].value), TAG, "panel init data");
    }
    return ESP_OK;
}

/* Six-ink startup: preserve the existing double reset and E9=01 sequence. */
static esp_err_t panel_six_color_init(void)
{
    ESP_RETURN_ON_ERROR(hw_reset(), TAG, "six-color reset");
    ESP_RETURN_ON_ERROR(write_cmd(0xE9), TAG, "six-color E9");
    ESP_RETURN_ON_ERROR(write_data_byte(0x01), TAG, "six-color E9 data");
    ESP_LOGI(TAG, "six-color init: legacy double reset + E9=01");
    return ESP_OK;
}

static esp_err_t select_otp_lut(void)
{
    ESP_RETURN_ON_ERROR(write_cmd(0x00), TAG, "PSR OTP");
    return write_data(k_lut_otp_setting, sizeof(k_lut_otp_setting));
}

static esp_err_t panel_power_on(void)
{
    ESP_RETURN_ON_ERROR(write_cmd(0x04), TAG, "PON");
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "PON busy");
    delay_ms(10);
    return ESP_OK;
}

static esp_err_t panel_refresh_keep(void)
{
    ESP_RETURN_ON_ERROR(write_cmd(0x12), TAG, "DRF");
    ESP_RETURN_ON_ERROR(write_data_byte(0x00), TAG, "DRF data");
    return wait_refresh();
}


static esp_err_t panel_power_off(void)
{
    ESP_RETURN_ON_ERROR(write_cmd(0x02), TAG, "POF");
    ESP_RETURN_ON_ERROR(write_data_byte(0x00), TAG, "POF data");
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "POF busy");
    delay_ms(20);
    return ESP_OK;
}

static esp_err_t panel_refresh(void)
{
    /* 与参考驱动一致：完整帧写入 RAM 后再 PON，随后 DRF、POF。 */
    ESP_RETURN_ON_ERROR(panel_power_on(), TAG, "PON");
    ESP_RETURN_ON_ERROR(panel_refresh_keep(), TAG, "DRF");
    return panel_power_off();
}

/* Recovery is best effort, but each failure remains visible. The original
 * operation error is never overwritten by a later recovery result. */
static esp_err_t recover_panel(void)
{
    esp_err_t first = ESP_OK;
    esp_err_t err = panel_power_off();
    if (err != ESP_OK) {
        first = err;
        ESP_LOGE(TAG, "recovery POF failed: %s", esp_err_to_name(err));
    }
    err = hw_reset();
    if (err != ESP_OK) {
        if (first == ESP_OK) first = err;
        ESP_LOGE(TAG, "recovery reset failed: %s", esp_err_to_name(err));
    } else {
        err = panel_init();
        if (err != ESP_OK) {
            if (first == ESP_OK) first = err;
            ESP_LOGE(TAG, "recovery init failed: %s", esp_err_to_name(err));
        }
    }
    const esp_err_t otp = select_otp_lut();
    if (otp != ESP_OK) {
        if (first == ESP_OK) first = otp;
        ESP_LOGE(TAG, "recovery OTP restore failed: %s", esp_err_to_name(otp));
    }
    return first;
}

static esp_err_t finish_panel_operation(esp_err_t operation_err)
{
    if (operation_err == ESP_OK) {
        operation_err = select_otp_lut();
        if (operation_err == ESP_OK) return ESP_OK;
        ESP_LOGE(TAG, "OTP restore failed after refresh: %s", esp_err_to_name(operation_err));
    }
    const esp_err_t recovery_err = recover_panel();
    ESP_LOGW(TAG, "operation failed=%s recovery=%s",
             esp_err_to_name(operation_err), esp_err_to_name(recovery_err));
    return operation_err;
}

static uint8_t color_byte(screen_color_t color)
{
    return (uint8_t)((color << 4) | color);
}

esp_err_t screen_driver_init(void)
{
    if (s_ctx.inited) {
        return ESP_OK;
    }
    uint32_t generation;
    do { generation = esp_random(); } while (generation == 0);
    __atomic_store_n(&s_display_generation, generation, __ATOMIC_RELAXED);

    s_ctx.lock = xSemaphoreCreateMutex();
    if (s_ctx.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io_out = {
        .pin_bit_mask = (1ULL << eink_dc) | (1ULL << eink_rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_out), TAG, "gpio out");

    gpio_config_t io_in = {
        .pin_bit_mask = (1ULL << eink_busy),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_in), TAG, "gpio busy");

    gpio_set_level(eink_rst, 1);
    gpio_set_level(eink_dc, 1);

    ESP_RETURN_ON_ERROR(board_init(), TAG, "board");

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EINK_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = eink_cs,
        .queue_size = 4,
        .pre_cb = lcd_spi_pre_transfer_callback,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(board_spi_host(), &devcfg, &s_ctx.spi),
                        TAG, "spi device");

    ESP_LOGI(TAG, "pins MOSI=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d  shared MISO=%d TF_CS=%d",
             (int)eink_sda, (int)eink_clk, (int)eink_cs,
             (int)eink_dc, (int)eink_rst, (int)eink_busy,
             (int)spi2_sdo, (int)spi2_tf_cs);

    board_spi_lock();
    esp_err_t err = hw_reset();
    if (err == ESP_OK) {
        err = wait_busy();
    }
    if (err == ESP_OK) {
        err = panel_init();
    }
    board_spi_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "panel init");

    s_ctx.inited = true;
    ESP_LOGI(TAG, "panel=%s ready %dx%d", EPD037_PROFILE_NAME, SCREEN_WIDTH, SCREEN_HEIGHT);
    return ESP_OK;
}

/* Caller holds the display lock and the board SPI lock. Shared with the
 * partial probe's mandatory cleanup, which runs inside one lock window. */
static esp_err_t fill_locked(screen_color_t color, screen_upload_report_t *report,
                             TickType_t started)
{
    esp_err_t err = panel_six_color_init();
    if (err == ESP_OK) err = write_cmd(0x10);
    if (err == ESP_OK) err = wait_busy();
    if (err == ESP_OK) {
        err = write_solid(color_byte(color), SCREEN_FRAME_BYTES);
    }
    const TickType_t refresh_started = xTaskGetTickCount();
    if (err == ESP_OK) err = panel_refresh();
    const uint32_t refresh_ms = (xTaskGetTickCount() - refresh_started) * portTICK_PERIOD_MS;
    err = finish_panel_operation(err);
    if (report) {
        report->total_ms = (xTaskGetTickCount() - started) * portTICK_PERIOD_MS;
        report->refresh_ms = refresh_ms;
        report->display_generation = screen_driver_generation();
        report->completed = err == ESP_OK;
    }
    return err;
}

static esp_err_t fill_screen(screen_color_t color, screen_upload_report_t *report)
{
    if (!s_ctx.inited) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    board_spi_lock();

    const TickType_t started = xTaskGetTickCount();
    const esp_err_t err = fill_locked(color, report, started);

    board_spi_unlock();
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "fill color=0x%X done", (unsigned)color);
    }
    return err;
}

esp_err_t screen_driver_fill(screen_color_t color)
{
    return fill_screen(color, NULL);
}

esp_err_t screen_driver_clear(screen_upload_report_t *report)
{
    if (!report) return ESP_ERR_INVALID_ARG;
    memset(report, 0, sizeof(*report));
    return fill_screen(SCREEN_COLOR_WHITE, report);
}

esp_err_t screen_driver_show_frame(const uint8_t *frame, size_t len)
{
    if (!s_ctx.inited || frame == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len != SCREEN_FRAME_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    board_spi_lock();

    esp_err_t err = panel_six_color_init();
    if (err == ESP_OK) err = write_cmd(0x10);
    if (err == ESP_OK) err = wait_busy();
    if (err == ESP_OK) {
        err = write_data(frame, len);
    }
    if (err == ESP_OK) {
        err = panel_refresh();
    }

    err = finish_panel_operation(err);

    board_spi_unlock();
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "frame flushed");
    }
    return err;
}

/* All consumers hold the display and board SPI locks. The callback must not
 * re-enter the display driver. A short read is valid; zero/oversized reads fail.
 * Hash only when upload evidence is requested; boot QR avoids that extra work. */
static esp_err_t stream_bytes(uint8_t *buffer, size_t capacity, size_t remaining,
                              screen_frame_read_cb_t read_cb, void *context,
                              screen_frame_decorate_cb_t decorate, void *decorate_ctx,
                              uint32_t *sent_fnv)
{
    uint32_t hash = 2166136261U;
    size_t sent = 0;
    while (remaining > 0) {
        const size_t limit = remaining < capacity ? remaining : capacity;
        size_t received = 0;
        ESP_RETURN_ON_ERROR(read_cb(context, buffer, limit, &received), TAG, "stream read");
        if (received == 0 || received > limit) return ESP_ERR_INVALID_SIZE;
        if (sent_fnv) {
            for (size_t i = 0; i < received; ++i) hash = (hash ^ buffer[i]) * 16777619U;
        }
        if (decorate) decorate(decorate_ctx, buffer, sent, received);
        ESP_RETURN_ON_ERROR(write_data(buffer, received), TAG, "stream SPI");
        remaining -= received;
        sent += received;
        if (sent_fnv && (sent % (32U * 1024U) < received || remaining == 0)) {
            ESP_LOGI(TAG, "upload DTM %u/%u", (unsigned)sent, (unsigned)SCREEN_FRAME_BYTES);
        }
    }
    if (sent_fnv) {
        *sent_fnv = hash;
        ESP_LOGI(TAG, "upload plane source bytes=%u fnv=%08x", (unsigned)sent, (unsigned)hash);
    }
    return ESP_OK;
}

static esp_err_t show_otp_stream(const uint8_t *head, size_t head_len,
                                 screen_frame_read_cb_t read_cb, void *context,
                                 screen_before_refresh_cb_t completed, void *completed_ctx)
{
    if (!s_ctx.inited) return ESP_ERR_INVALID_STATE;
    if (!read_cb || head_len > SCREEN_FRAME_BYTES || (head_len && !head)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    board_spi_lock();
    esp_err_t err = panel_six_color_init();
    if (err == ESP_OK) err = write_cmd(0x10);
    if (err == ESP_OK) err = wait_busy();
    if (err == ESP_OK && head_len) err = write_data(head, head_len);
    if (err == ESP_OK) {
        err = stream_bytes(s_spi_tx, sizeof(s_spi_tx), SCREEN_FRAME_BYTES - head_len,
                           read_cb, context, NULL, NULL, NULL);
    }
    if (err == ESP_OK) err = panel_refresh();
    err = finish_panel_operation(err);
    board_spi_unlock();
    xSemaphoreGive(s_ctx.lock);
    /* Completion notification after physical refresh and recovery boundary. */
    if (err == ESP_OK) {
        if (completed) completed(completed_ctx);
        ESP_LOGI(TAG, "streamed frame flushed");
    }
    return err;
}

esp_err_t screen_driver_show_stream_ex(screen_frame_read_cb_t read_cb, void *context,
                                       screen_before_refresh_cb_t before_refresh, void *refresh_ctx)
{
    return show_otp_stream(NULL, 0, read_cb, context, before_refresh, refresh_ctx);
}

esp_err_t screen_driver_show_stream(screen_frame_read_cb_t read_cb, void *context)
{
    return show_otp_stream(NULL, 0, read_cb, context, NULL, NULL);
}

esp_err_t screen_driver_show_stream_prefaced(const uint8_t *head, size_t head_len,
                                             screen_frame_read_cb_t read_cb, void *context)
{
    if (!head || !head_len) return ESP_ERR_INVALID_ARG;
    return show_otp_stream(head, head_len, read_cb, context, NULL, NULL);
}

/* Stream exactly one controller plane. The callback validates the last chunk
 * before it is committed, so an incomplete/corrupt plane never starts DRF. */
static esp_err_t upload_plane(uint8_t *buffer, size_t capacity,
                               screen_frame_read_cb_t read_cb, void *context,
                               screen_frame_decorate_cb_t decorate, void *decorate_ctx,
                               uint32_t *sent_fnv)
{
    ESP_RETURN_ON_ERROR(write_cmd(0x10), TAG, "upload DTM");
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "upload DTM busy");
    return stream_bytes(buffer, capacity, SCREEN_FRAME_BYTES, read_cb, context,
                        decorate, decorate_ctx, sent_fnv);
}

esp_err_t screen_driver_upload(uint32_t expected_generation,
    uint8_t *buffer, size_t capacity, screen_frame_read_cb_t read_cb, void *context,
    screen_upload_progress_cb_t progress, screen_frame_decorate_cb_t decorate,
    void *decorate_ctx, screen_upload_report_t *report)
{
    if (!s_ctx.inited) return ESP_ERR_INVALID_STATE;
    if (!buffer || !capacity || capacity > EINK_CHUNK_BYTES || !read_cb || !progress || !report)
        return ESP_ERR_INVALID_ARG;
    memset(report, 0, sizeof(*report));
    if (xSemaphoreTake(s_ctx.lock, pdMS_TO_TICKS(30000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (expected_generation && expected_generation != screen_driver_generation()) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_ERR_INVALID_STATE;
    }
    board_spi_lock();
    const TickType_t started = xTaskGetTickCount();
    esp_err_t err = panel_six_color_init();
    if (err == ESP_OK) err = progress(context, SCREEN_UPLOAD_RECEIVING);
    if (err == ESP_OK) err = upload_plane(buffer, capacity, read_cb, context,
                                          decorate, decorate_ctx, &report->frame_fnv);
    const TickType_t stage_started = xTaskGetTickCount();
    if (err == ESP_OK) err = progress(context, SCREEN_UPLOAD_REFRESH);
    if (err == ESP_OK) err = panel_refresh();
    if (err == ESP_OK) report->completed = true;
    report->refresh_ms = (xTaskGetTickCount() - stage_started) * portTICK_PERIOD_MS;
    err = finish_panel_operation(err);
    report->total_ms = (xTaskGetTickCount() - started) * portTICK_PERIOD_MS;
    report->display_generation = screen_driver_generation();
    board_spi_unlock();
    xSemaphoreGive(s_ctx.lock);
    return err;
}

/* ---- Partial-window bring-up probe --------------------------------------- */

#define SCREEN_PARTIAL_WINDOW_BYTES 9

bool screen_driver_partial_probe_enabled(void)
{
    return SCREEN_PARTIAL_PROBE_ENABLE != 0;
}

static uint32_t elapsed_ms(int64_t since_us)
{
    const int64_t delta = esp_timer_get_time() - since_us;
    return delta <= 0 ? 0U : (uint32_t)(delta / 1000);
}

/* Map a logical rectangle onto the panel. The frame is gate-major:
 * gate = 479 - px, and one RAM byte covers two pixels (rows = py / 2).
 * Axis-aligned logical rectangles stay axis-aligned under 0/90/180/270, so
 * four corners are enough. Y edges widen to whole byte rows, which can paint
 * at most one extra pixel outside the requested window. */
static bool probe_map_window(const screen_probe_req_t *req,
                             uint16_t *gate_start, uint16_t *gate_end,
                             uint16_t *row_start, uint16_t *row_end,
                             int16_t *px0, int16_t *py0, int16_t *px1, int16_t *py1)
{
    if (req->rotation != 0 && req->rotation != 90 &&
        req->rotation != 180 && req->rotation != 270) return false;
    int logical_w = 0, logical_h = 0;
    screen_rotation_size(req->rotation, &logical_w, &logical_h);
    if (req->w < 8 || req->h < 8) return false;
    if (req->x < 0 || req->y < 0) return false;
    if (req->w > logical_w || req->h > logical_h) return false;
    if (req->x > logical_w - req->w || req->y > logical_h - req->h) return false;

    const int xs[2] = { req->x, req->x + req->w - 1 };
    const int ys[2] = { req->y, req->y + req->h - 1 };
    int min_x = SCREEN_WIDTH, max_x = -1, min_y = SCREEN_HEIGHT, max_y = -1;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            int px = 0, py = 0;
            screen_rotation_panel_xy(req->rotation, xs[i], ys[j], &px, &py);
            if (px < min_x) min_x = px;
            if (px > max_x) max_x = px;
            if (py < min_y) min_y = py;
            if (py > max_y) max_y = py;
        }
    }
    if (min_x < 0 || max_x >= SCREEN_WIDTH || min_y < 0 || max_y >= SCREEN_HEIGHT) return false;

    const int gs = SCREEN_WIDTH - 1 - max_x;
    const int ge = SCREEN_WIDTH - 1 - min_x;
    const int rs = min_y / 2;
    const int re = max_y / 2;
    if (gs < 0 || ge >= SCREEN_WIDTH || gs > ge) return false;
    if (re >= SCREEN_HEIGHT / 2 || rs < 0 || rs > re) return false;

    *gate_start = (uint16_t)gs;
    *gate_end = (uint16_t)ge;
    *row_start = (uint16_t)rs;
    *row_end = (uint16_t)re;
    *px0 = (int16_t)min_x;
    *py0 = (int16_t)min_y;
    *px1 = (int16_t)max_x;
    *py1 = (int16_t)max_y;
    return true;
}

/* Candidate JD/IST 9-byte 0x92 payload: two 9-bit fields per axis in
 * little-endian order, then one control byte. X is the gate index (0..479) and
 * Y is the RAM byte row (0..359). This layout is NOT confirmed on EPD037A02;
 * it is the only reason the probe is compiled out by default. */
static void build_partial_window(uint8_t payload[SCREEN_PARTIAL_WINDOW_BYTES],
                                 uint16_t x_start, uint16_t x_end,
                                 uint16_t y_start, uint16_t y_end)
{
    payload[0] = (uint8_t)(x_start & 0xFFU);
    payload[1] = (uint8_t)((x_start >> 8) & 0x01U);
    payload[2] = (uint8_t)(x_end & 0xFFU);
    payload[3] = (uint8_t)((x_end >> 8) & 0x01U);
    payload[4] = (uint8_t)(y_start & 0xFFU);
    payload[5] = (uint8_t)((y_start >> 8) & 0x01U);
    payload[6] = (uint8_t)(y_end & 0xFFU);
    payload[7] = (uint8_t)((y_end >> 8) & 0x01U);
    payload[8] = 0x00;
}

/* One solid ink inside the mapped window, in the same gate-then-row order as a
 * full frame so the probe can pinpoint a swapped or offset window. */
static esp_err_t write_window_pattern(uint16_t gate_start, uint16_t gate_end,
                                      uint16_t row_start, uint16_t row_end,
                                      uint8_t ink)
{
    const uint8_t value = (uint8_t)((ink << 4) | ink);
    const size_t rows = (size_t)row_end - row_start + 1;
    for (uint16_t gate = gate_start; gate <= gate_end; ++gate) {
        ESP_RETURN_ON_ERROR(write_solid(value, rows), TAG, "probe window data");
    }
    return ESP_OK;
}

esp_err_t screen_driver_partial_probe(const screen_probe_req_t *req,
                                      screen_probe_result_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!req) return ESP_ERR_INVALID_ARG;
    out->rotation = req->rotation;
    if (!screen_driver_partial_probe_enabled()) {
        out->err = ESP_ERR_NOT_SUPPORTED;
        return out->err;
    }
    if (!s_ctx.inited) {
        out->err = ESP_ERR_INVALID_STATE;
        return out->err;
    }
    if (req->repeat < 1 || req->repeat > 10 ||
        (req->mode != SCREEN_PROBE_MODE_WRITE && req->mode != SCREEN_PROBE_MODE_REFRESH)) {
        out->err = ESP_ERR_INVALID_ARG;
        return out->err;
    }

    uint16_t gate_start = 0, gate_end = 0, row_start = 0, row_end = 0;
    if (!probe_map_window(req, &gate_start, &gate_end, &row_start, &row_end,
                          &out->px0, &out->py0, &out->px1, &out->py1)) {
        out->err = ESP_ERR_INVALID_ARG;
        return out->err;
    }
    out->gate_start = gate_start;
    out->gate_end = gate_end;
    out->row_start = row_start;
    out->row_end = row_end;
    out->bytes = (uint32_t)(gate_end - gate_start + 1) * (uint32_t)(row_end - row_start + 1);

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    board_spi_lock();

    const int64_t probe_started = esp_timer_get_time();
    esp_err_t first_err = panel_six_color_init();
    bool partial_active = false;

    if (first_err == ESP_OK) {
        esp_err_t err = write_cmd(0x91);          /* partial in */
        if (err == ESP_OK) partial_active = true;
        if (err == ESP_OK) err = write_data_byte(0x00);
        if (err != ESP_OK) first_err = err;
    }
    if (first_err == ESP_OK) {
        uint8_t payload[SCREEN_PARTIAL_WINDOW_BYTES];
        build_partial_window(payload, gate_start, gate_end, row_start, row_end);
        esp_err_t err = write_cmd(0x92);          /* partial window */
        if (err == ESP_OK) err = write_data(payload, sizeof(payload));
        if (err != ESP_OK) first_err = err;
    }

    int64_t write_us = 0, drf_us = 0;
    for (uint8_t pass = 0; pass < req->repeat && first_err == ESP_OK; ++pass) {
        const int64_t write_started = esp_timer_get_time();
        esp_err_t err = write_cmd(0x10);
        if (err == ESP_OK) err = wait_busy();
        if (err == ESP_OK) err = write_window_pattern(gate_start, gate_end,
                                                      row_start, row_end,
                                                      (uint8_t)SCREEN_COLOR_RED);
        write_us += esp_timer_get_time() - write_started;
        if (err != ESP_OK) {
            first_err = err;
            break;
        }
        ++out->repeats_done;

        if (req->mode == SCREEN_PROBE_MODE_WRITE) continue;

        err = panel_power_on();
        if (err != ESP_OK) {
            first_err = err;
            break;
        }
        const int64_t drf_started = esp_timer_get_time();
        bool asserted = false;
        err = write_cmd(0x12);
        if (err == ESP_OK) err = write_data_byte(0x00);
        if (err == ESP_OK) {
            err = wait_refresh_ex(&asserted);
            if (asserted) {
                out->busy_asserted = true;
            } else {
                ++out->busy_failures;
            }
        }
        drf_us += esp_timer_get_time() - drf_started;
        if (err != ESP_OK) {
            first_err = err;
            break;
        }
        err = panel_power_off();
        if (err != ESP_OK) {
            first_err = err;
            break;
        }
    }
    out->write_ms = (uint32_t)(write_us / 1000);
    out->drf_ms = (uint32_t)(drf_us / 1000);

    /* Leave partial mode no matter where the probe stopped. */
    if (partial_active) {
        const esp_err_t exit_err = write_cmd(0x90); /* partial out */
        if (exit_err != ESP_OK) {
            if (first_err == ESP_OK) first_err = exit_err;
            out->exit_partial_ok = false;
        } else {
            out->exit_partial_ok = true;
        }
        delay_ms(10);
    } else {
        out->exit_partial_ok = true;
    }

    /* Mandatory clean frame. The reset inside this path also drops partial
     * mode if 0x90 itself failed, so the panel cannot stay half-windowed. */
    const int64_t cleanup_started = esp_timer_get_time();
    esp_err_t cleanup_err = fill_locked(SCREEN_COLOR_WHITE, NULL, xTaskGetTickCount());
    out->cleanup_ms = elapsed_ms(cleanup_started);
    out->cleanup_ok = cleanup_err == ESP_OK;
    if (first_err == ESP_OK && cleanup_err != ESP_OK) first_err = cleanup_err;

    out->total_ms = elapsed_ms(probe_started);
    out->err = first_err;

    board_spi_unlock();
    xSemaphoreGive(s_ctx.lock);

    ESP_LOGI(TAG, "probe %s gate=%u..%u row=%u..%u bytes=%u repeat=%u/%u "
             "write=%ums drf=%ums cleanup=%ums total=%ums busy=%d/%u",
             esp_err_to_name(first_err), (unsigned)gate_start, (unsigned)gate_end,
             (unsigned)row_start, (unsigned)row_end,
             (unsigned)out->bytes, (unsigned)out->repeats_done, (unsigned)req->repeat,
             (unsigned)out->write_ms, (unsigned)out->drf_ms,
             (unsigned)out->cleanup_ms, (unsigned)out->total_ms,
             out->busy_asserted ? 1 : 0, (unsigned)out->busy_failures);
    return first_err;
}

esp_err_t screen_driver_deep_sleep(void)
{
    if (!s_ctx.inited) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    board_spi_lock();
    esp_err_t err = write_cmd(0x07);
    if (err == ESP_OK) {
        err = write_data_byte(0xA5);
    }
    board_spi_unlock();
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "deep sleep");
    }
    return err;
}
