#include "screen_rotation.h"
#include "pin_map.h"
#include "power_mgr.h"
#include "screen_driver.h"
#include "screen_ui.h"

#include "web_server.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "rotation";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static unsigned s_rotation = EINK_ROTATION;
static uint32_t s_epoch = 1, s_pending;
static bool s_working, s_ui_current;
static const esp_partition_t *s_partition;

/* Three picture slots share the 1 MB image_cache partition: each slot keeps a
 * 4 KB header followed by one verified six-ink frame, and the last sector
 * records which slot is on the glass so a power cycle resumes the cycle. */
#define CACHE_SLOTS        3
#define CACHE_SLOT_BYTES   0x50000U        /* 320 KiB */
#define CACHE_HEADER_BYTES 4096U
#define CACHE_SECTOR_BYTES 4096U
#define CACHE_META_OFFSET  ((size_t)CACHE_SLOTS * CACHE_SLOT_BYTES)
#define SLOT_MAGIC         0x314B4E45U     /* "ENK1" */
#define SLOT_VERSION       1U
#define META_MAGIC         0x4154454DU     /* "META" */
#define META_NONE          0xFFFFFFFFU

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t kind;
    uint8_t rotation;
    uint8_t planes;
    uint8_t blank[3];
    uint32_t bytes;
    uint32_t fnv;
} slot_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t current;                      /* slot index shown last, or META_NONE */
    uint32_t fnv;                          /* over the fields above */
} cache_meta_t;

/* One verified six-ink frame per slot. An upload may target any slot; the one
 * on the glass is the only one that must not be overwritten implicitly. */
typedef struct {
    bool valid;
    screen_cache_kind_t kind;
    unsigned rotation, planes;
    uint8_t blank[3];
    size_t bytes, tail;
    uint32_t generation;
} image_slot_t;
static image_slot_t s_slots[CACHE_SLOTS];
static int s_slot_current = -1;
/* Text drawn as a bottom strip while the device is awake (empty = none). */
static char s_overlay[24];
static char s_overlay_applied[24];
static bool s_overlay_shown;

typedef enum { CAP_NONE, CAP_FULL } capture_mode_t;
typedef struct {
    int target;
    capture_mode_t capturing;
    screen_cache_kind_t kind;
    unsigned rotation, planes;
    uint8_t blank[3];
    size_t bytes, tail;
    size_t written, capture_bytes;
    uint32_t fnv;
} capture_t;
static capture_t s_capture;

static bool s_blank_current;
static bool s_hidden;
static bool s_image_current;
static uint32_t s_hidden_generation;
static uint32_t s_source_generation;

static size_t sector_size(size_t n) { return (n + CACHE_SECTOR_BYTES - 1U) & ~(CACHE_SECTOR_BYTES - 1U); }
static size_t slot_base(int index) { return (size_t)index * CACHE_SLOT_BYTES; }
static size_t slot_data_base(int index) { return slot_base(index) + CACHE_HEADER_BYTES; }
static uint32_t fnv1a(uint32_t hash, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) hash = (hash ^ data[i]) * 16777619U;
    return hash;
}

unsigned screen_rotation_get(void) { return __atomic_load_n(&s_rotation, __ATOMIC_ACQUIRE); }
uint32_t screen_rotation_epoch(void) { return __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE); }
bool screen_rotation_busy(void)
{
    return __atomic_load_n(&s_working, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE) != 0;
}
bool screen_rotation_matches(uint32_t epoch) { return epoch == screen_rotation_epoch(); }
void screen_rotation_lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void screen_rotation_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }
void screen_rotation_size(unsigned r, int *w, int *h)
{
    *w = r == 90 || r == 270 ? SCREEN_HEIGHT : SCREEN_WIDTH;
    *h = r == 90 || r == 270 ? SCREEN_WIDTH : SCREEN_HEIGHT;
}
void screen_rotation_source_xy(unsigned r, int px, int py, int *x, int *y)
{
    switch (r) {
    case 90: *x = py; *y = SCREEN_WIDTH - 1 - px; break;
    case 180: *x = SCREEN_WIDTH - 1 - px; *y = SCREEN_HEIGHT - 1 - py; break;
    case 270: *x = SCREEN_HEIGHT - 1 - py; *y = px; break;
    default: *x = px; *y = py; break;
    }
}
void screen_rotation_panel_xy(unsigned r, int x, int y, int *px, int *py)
{
    switch (r) {
    case 90: *px = SCREEN_WIDTH - 1 - y; *py = x; break;
    case 180: *px = SCREEN_WIDTH - 1 - x; *py = SCREEN_HEIGHT - 1 - y; break;
    case 270: *px = y; *py = SCREEN_HEIGHT - 1 - x; break;
    default: *px = x; *py = y; break;
    }
}
uint32_t screen_rotation_source_generation(void)
{
    return __atomic_load_n(&s_source_generation, __ATOMIC_ACQUIRE);
}

/* Only the on-glass tracking is dropped: a page redraw does not make the
 * stored pictures unreachable, and they survive a power cycle anyway. */
void screen_rotation_invalidate(void)
{
    s_hidden = false;
    s_image_current = false;
    s_capture.capturing = CAP_NONE;
    s_overlay_shown = false;
    __atomic_store_n(&s_source_generation, 0, __ATOMIC_RELEASE);
}
void screen_rotation_note_external(void)
{
    /* The display is driven by an upload or UI refresh. Keep slot contents:
     * an upload replaces one slot, so a failure leaves the other pictures
     * intact and replayable. */
    s_hidden = false;
    s_image_current = false;
    s_ui_current = false;
    s_blank_current = false;
    s_capture.capturing = CAP_NONE;
    s_overlay_shown = false;
}
void screen_rotation_note_ui(void)
{
    screen_rotation_invalidate(); s_ui_current = true; s_blank_current = false;
}
void screen_rotation_note_notice(void)
{
    /* A transient firmware notice (low power) replaced the content on the
     * glass, but the flash slots keep the pictures for replay. */
    s_hidden = false;
    s_image_current = false;
    s_ui_current = true;
    s_blank_current = false;
    s_capture.capturing = CAP_NONE;
    s_overlay_shown = false;
}
void screen_rotation_note_blank(void)
{
    screen_rotation_invalidate(); s_ui_current = false; s_blank_current = true;
}

void screen_rotation_set_overlay(const char *text)
{
    snprintf(s_overlay, sizeof(s_overlay), "%s", text ? text : "");
}
const char *screen_rotation_overlay(void)
{
    return s_overlay;
}

int screen_rotation_current(void)
{
    return s_slot_current;
}
unsigned screen_rotation_image_count(void)
{
    unsigned count = 0;
    for (int i = 0; i < CACHE_SLOTS; ++i) if (s_slots[i].valid) ++count;
    return count;
}
bool screen_rotation_has_images(void)
{
    return screen_rotation_image_count() > 0;
}
bool screen_rotation_slot_info(int index, unsigned *rotation)
{
    if (index < 0 || index >= CACHE_SLOTS || !s_slots[index].valid) return false;
    if (rotation) *rotation = s_slots[index].rotation;
    return true;
}

static void meta_save(void)
{
    if (!s_partition || s_partition->size < CACHE_META_OFFSET + CACHE_SECTOR_BYTES) return;
    cache_meta_t meta = {
        .magic = META_MAGIC,
        .current = s_slot_current < 0 ? META_NONE : (uint32_t)s_slot_current,
    };
    meta.fnv = fnv1a(2166136261U, (const uint8_t *)&meta, offsetof(cache_meta_t, fnv));
    if (esp_partition_erase_range(s_partition, CACHE_META_OFFSET, CACHE_SECTOR_BYTES) != ESP_OK) {
        ESP_LOGW(TAG, "meta erase failed");
        return;
    }
    if (esp_partition_write(s_partition, CACHE_META_OFFSET, &meta, sizeof(meta)) != ESP_OK) {
        ESP_LOGW(TAG, "meta write failed");
    }
}
static int meta_load(void)
{
    if (!s_partition || s_partition->size < CACHE_META_OFFSET + CACHE_SECTOR_BYTES) return -1;
    cache_meta_t meta;
    if (esp_partition_read(s_partition, CACHE_META_OFFSET, &meta, sizeof(meta)) != ESP_OK) return -1;
    if (meta.magic != META_MAGIC || meta.current >= CACHE_SLOTS) return -1;
    if (meta.fnv != fnv1a(2166136261U, (const uint8_t *)&meta, offsetof(cache_meta_t, fnv))) return -1;
    return (int)meta.current;
}

/* A slot is usable after a power cycle when its header is intact and the
 * frame still hashes to the value recorded with it. */
static bool slot_load(int index)
{
    if (!s_partition) return false;
    slot_header_t head;
    if (esp_partition_read(s_partition, slot_base(index), &head, sizeof(head)) != ESP_OK) return false;
    if (head.magic != SLOT_MAGIC || head.version != SLOT_VERSION ||
        head.rotation % 90 || head.kind > SCREEN_CACHE_512_FAST) return false;
    if (!head.planes || head.planes > 3) return false;
    if (head.bytes != (uint32_t)head.planes * SCREEN_FRAME_BYTES) return false;
    if (CACHE_HEADER_BYTES + sector_size(head.bytes) > CACHE_SLOT_BYTES) return false;

    uint32_t fnv = 2166136261U;
    uint8_t buffer[512];
    size_t remaining = head.bytes, offset = slot_data_base(index);
    while (remaining > 0) {
        size_t n = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (esp_partition_read(s_partition, offset, buffer, n) != ESP_OK) return false;
        fnv = fnv1a(fnv, buffer, n);
        offset += n;
        remaining -= n;
    }
    if (fnv != head.fnv) {
        ESP_LOGW(TAG, "slot %d header hash mismatch", index);
        return false;
    }
    s_slots[index] = (image_slot_t){.valid = true, .kind = (screen_cache_kind_t)head.kind,
        .rotation = head.rotation, .planes = head.planes,
        .bytes = head.bytes, .tail = sector_size(head.bytes), .generation = 0};
    memcpy(s_slots[index].blank, head.blank, sizeof(s_slots[index].blank));
    return true;
}

esp_err_t screen_rotation_capture_begin(screen_cache_kind_t kind, const uint8_t blank[3], int slot)
{
    screen_rotation_note_external();
    if (!s_partition) return ESP_ERR_NOT_FOUND;
    if (!blank) return ESP_ERR_INVALID_ARG;
    unsigned planes = kind == SCREEN_CACHE_OTP6 ? 1 :
        kind == SCREEN_CACHE_CQ64 || kind == SCREEN_CACHE_FAST64 ? 2 : 3;
    const size_t bytes = planes * SCREEN_FRAME_BYTES;
    const size_t erase = CACHE_HEADER_BYTES + sector_size(bytes);
    if (erase > CACHE_SLOT_BYTES) return ESP_ERR_INVALID_SIZE;
    if (slot < 0 || slot >= CACHE_SLOTS) {
        slot = s_slot_current < 0 ? 0 : (s_slot_current + 1) % CACHE_SLOTS;
    }
    s_slots[slot].valid = false;
    esp_err_t err = esp_partition_erase_range(s_partition, slot_base(slot), erase);
    if (err != ESP_OK) return err;
    s_capture = (capture_t){.target = slot, .capturing = CAP_FULL,
        .kind = kind, .rotation = screen_rotation_get(), .planes = planes,
        .bytes = bytes, .tail = sector_size(bytes), .capture_bytes = bytes,
        .fnv = 2166136261U};
    memcpy(s_capture.blank, blank, sizeof(s_capture.blank));
    return ESP_OK;
}

esp_err_t screen_rotation_capture_write(size_t offset, const uint8_t *data, size_t length)
{
    if (s_capture.capturing == CAP_NONE) return ESP_ERR_INVALID_STATE;
    if (offset >= s_capture.capture_bytes) return ESP_OK;
    if (offset != s_capture.written || !data) {
        s_capture.capturing = CAP_NONE;
        s_slots[s_capture.target].valid = false;
        return ESP_ERR_INVALID_SIZE;
    }
    if (length > s_capture.capture_bytes - offset) length = s_capture.capture_bytes - offset;
    esp_err_t err = esp_partition_write(s_partition,
        slot_data_base(s_capture.target) + offset, data, length);
    if (err != ESP_OK) {
        s_capture.capturing = CAP_NONE;
        s_slots[s_capture.target].valid = false;
        ESP_LOGW(TAG, "image cache write disabled: %s", esp_err_to_name(err));
        return err;
    }
    s_capture.fnv = fnv1a(s_capture.fnv, data, length);
    s_capture.written += length;
    return ESP_OK;
}
void screen_rotation_capture_end(bool success, uint32_t generation)
{
    const bool begun = s_capture.capturing != CAP_NONE;
    const bool complete = success && begun && s_capture.written == s_capture.capture_bytes;
    const int target = s_capture.target;
    const bool covered = begun && s_slot_current == target;
    s_capture.capturing = CAP_NONE;

    if (complete) {
        slot_header_t head = {
            .magic = SLOT_MAGIC, .version = SLOT_VERSION, .kind = (uint8_t)s_capture.kind,
            .rotation = (uint8_t)s_capture.rotation, .planes = (uint8_t)s_capture.planes,
            .bytes = (uint32_t)s_capture.bytes, .fnv = s_capture.fnv,
        };
        memcpy(head.blank, s_capture.blank, sizeof(head.blank));
        if (esp_partition_write(s_partition, slot_base(target), &head, sizeof(head)) != ESP_OK) {
            ESP_LOGW(TAG, "slot %d header write failed; picture lost after power cycle", target);
        }
        s_slots[target] = (image_slot_t){.valid = true, .kind = s_capture.kind,
            .rotation = s_capture.rotation, .planes = s_capture.planes,
            .bytes = s_capture.bytes, .tail = s_capture.tail, .generation = generation};
        memcpy(s_slots[target].blank, s_capture.blank, sizeof(s_slots[target].blank));
        s_slot_current = target;
        s_image_current = true;
        __atomic_store_n(&s_source_generation, generation, __ATOMIC_RELEASE);
        /* The upload was streamed with the IP strip applied when one was set. */
        s_overlay_shown = s_overlay[0] != 0;
        snprintf(s_overlay_applied, sizeof(s_overlay_applied), "%s", s_overlay_shown ? s_overlay : "");
        meta_save();
        ESP_LOGI(TAG, "image slot %d ready bytes=%u images=%u source_rotation=%u",
                 target, (unsigned)s_slots[target].bytes,
                 (unsigned)screen_rotation_image_count(), s_slots[target].rotation);
        return;
    }

    /* No verified flash copy of this transfer. The panel only refreshes after
     * a complete plane, so the previous picture is still on the glass. */
    if (begun) {
        s_slots[target].valid = false;
        if (s_slot_current == target) s_slot_current = -1;
    }
    if (s_slot_current >= 0) {
        /* Another slot still holds the picture on the glass. */
        s_image_current = true;
        __atomic_store_n(&s_source_generation, s_slots[s_slot_current].generation,
                         __ATOMIC_RELEASE);
    } else if (success || covered) {
        /* The glass kept the old picture, but its flash copy was replaced. */
        s_image_current = true;
        __atomic_store_n(&s_source_generation, 0, __ATOMIC_RELEASE);
    } else {
        s_image_current = false;
        __atomic_store_n(&s_source_generation, 0, __ATOMIC_RELEASE);
    }
    ESP_LOGW(TAG, "image cache not stored (target %d, %s), current=%d image=%d",
             target, success ? "incomplete" : "transfer failed",
             s_slot_current, (int)s_image_current);
}

typedef struct {
    const image_slot_t *slot;
    const uint8_t *data;
    const char *overlay;
    unsigned target, plane;
    size_t sent;
    int sw, sh, dw, dh, width, height, left, top;
} replay_t;
static uint8_t replay_pixel(const replay_t *r, int px, int py)
{
    int x, y;
    screen_rotation_source_xy(r->target, px, py, &x, &y);
    uint8_t ink;
    if (r->overlay && screen_ui_overlay_pixel(r->overlay, r->target, x, y, &ink)) return ink;
    if (x < r->left || y < r->top || x >= r->left + r->width || y >= r->top + r->height)
        return r->slot->blank[r->plane];
    const int sx = (x - r->left) * r->sw / r->width;
    const int sy = (y - r->top) * r->sh / r->height;
    screen_rotation_panel_xy(r->slot->rotation, sx, sy, &px, &py);
    const unsigned gate = SCREEN_WIDTH - 1U - px;
    const size_t index = r->plane * SCREEN_FRAME_BYTES +
        gate * (SCREEN_HEIGHT / 2) + py / 2;
    const uint8_t pair = r->data[index];
    return py & 1 ? pair & 15 : pair >> 4;
}
static esp_err_t replay_read(void *context, uint8_t *buffer, size_t capacity, size_t *received)
{
    replay_t *r = context;
    size_t n = SCREEN_FRAME_BYTES - r->sent;
    if (n > capacity) n = capacity;
    for (size_t i = 0; i < n; ++i) {
        const size_t pos = r->sent + i;
        const int px = SCREEN_WIDTH - 1 - (int)(pos / (SCREEN_HEIGHT / 2));
        const int py = (int)(pos % (SCREEN_HEIGHT / 2)) * 2;
        buffer[i] = (uint8_t)((replay_pixel(r, px, py) << 4) | replay_pixel(r, px, py + 1));
    }
    r->sent += n;
    *received = n;
    return ESP_OK;
}
static void replay_setup(replay_t *r, const image_slot_t *slot, const uint8_t *mapped, unsigned target)
{
    *r = (replay_t){.slot = slot, .data = mapped, .target = target};
    screen_rotation_size(slot->rotation, &r->sw, &r->sh);
    screen_rotation_size(target, &r->dw, &r->dh);
    if (r->dw * r->sh <= r->dh * r->sw) { r->width = r->dw; r->height = r->sh * r->dw / r->sw; }
    else { r->height = r->dh; r->width = r->sw * r->dh / r->sh; }
    r->left = (r->dw - r->width) / 2; r->top = (r->dh - r->height) / 2;
}
static esp_err_t replay_progress(void *context, screen_upload_phase_t phase)
{
    (void)context;
    (void)phase;
    return ESP_OK;
}

static esp_err_t replay_image(int index, unsigned target, bool overlay)
{
    if (index < 0 || index >= CACHE_SLOTS || !s_slots[index].valid) return ESP_ERR_INVALID_STATE;
    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(s_partition, slot_data_base(index), s_slots[index].tail,
        ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (err != ESP_OK) return err;
    replay_t r;
    replay_setup(&r, &s_slots[index], mapped, target);
    r.overlay = overlay && s_overlay[0] ? s_overlay : NULL;
    uint8_t buffer[1024];
    screen_upload_report_t report = {0};
    err = screen_driver_upload(0, buffer, sizeof(buffer), replay_read, &r, replay_progress,
                               NULL, NULL, &report);
    esp_partition_munmap(handle);
    if (err == ESP_OK) {
        const bool moved = index != s_slot_current;
        s_slots[index].generation = report.display_generation;
        s_slot_current = index;
        s_image_current = true;
        __atomic_store_n(&s_source_generation, report.display_generation, __ATOMIC_RELEASE);
        s_overlay_shown = r.overlay != NULL;
        snprintf(s_overlay_applied, sizeof(s_overlay_applied), "%s", r.overlay ? r.overlay : "");
        if (moved) meta_save();
    } else {
        /* The flash copy is still intact; only the replay failed. Keep it so
         * a later wake can try again instead of falling back to a UI page. */
        s_slot_current = -1;
        s_image_current = false;
        s_overlay_shown = false;
        __atomic_store_n(&s_source_generation, 0, __ATOMIC_RELEASE);
    }
    return err;
}
bool screen_rotation_hidden(void)
{
    return s_hidden && s_hidden_generation == screen_driver_generation();
}

bool screen_rotation_shows_image(void)
{
    return s_image_current && !screen_rotation_hidden();
}

/* Bring the next cached picture in rotation order to the panel. When only the
 * current picture exists it is redrawn so the awake-only strip disappears
 * (idle entry) or is left untouched (side button, already showing it).
 * Caller holds the rotation lock. */
esp_err_t screen_rotation_show_next(bool overlay)
{
    int index = -1;
    for (unsigned step = 1; step <= CACHE_SLOTS && index < 0; ++step) {
        const int candidate = s_slot_current < 0 ? (int)step - 1 :
                              (s_slot_current + (int)step) % CACHE_SLOTS;
        if (s_slots[candidate].valid) index = candidate;
    }
    if (index < 0) return ESP_ERR_INVALID_STATE;
    const bool shown = overlay && s_overlay[0];
    if (index == s_slot_current && s_overlay_shown == shown &&
        strcmp(s_overlay_applied, shown ? s_overlay : "") == 0) return ESP_OK;
    ESP_LOGI(TAG, "next picture: slot %d -> %d strip=%d", s_slot_current, index, (int)shown);
    return replay_image(index, screen_rotation_get(), shown);
}

/* Replay the current (or any) picture with the configured strip. Nothing is
 * drawn when the same picture and text are already on the glass. */
esp_err_t screen_rotation_show_image(void)
{
    if (s_image_current && s_slot_current >= 0 && s_slots[s_slot_current].valid &&
        s_overlay_shown && strcmp(s_overlay_applied, s_overlay) == 0) {
        return ESP_OK;
    }
    int index = s_slot_current;
    if (index < 0 || !s_slots[index].valid) {
        for (int i = 0; i < CACHE_SLOTS && (index < 0 || !s_slots[index].valid); ++i) {
            if (s_slots[i].valid) index = i;
        }
    }
    if (index < 0 || !s_slots[index].valid) return ESP_ERR_INVALID_STATE;
    return replay_image(index, screen_rotation_get(), true);
}

/* Thumbnail of a cached slot for the web page: samples the stored frame in
 * place, no heap. Returns RGBA rows at the requested size. */
esp_err_t screen_rotation_preview(int index, uint8_t *rgba, int width, int height)
{
    static const uint8_t inks[7][4] = {
        {0x11, 0x14, 0x12, 255}, {0xf4, 0xf3, 0xe8, 255}, {0xe7, 0xc8, 0x3f, 255},
        {0xc6, 0x3b, 0x35, 255}, {0x00, 0x00, 0x00, 255}, {0x31, 0x5e, 0xae, 255},
        {0x39, 0x84, 0x51, 255},
    };
    if (!rgba || width < 1 || height < 1) return ESP_ERR_INVALID_ARG;
    if (index < 0 || index >= CACHE_SLOTS || !s_slots[index].valid) return ESP_ERR_INVALID_STATE;
    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(s_partition, slot_data_base(index), s_slots[index].tail,
        ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (err != ESP_OK) return err;
    const uint8_t *data = mapped;
    for (int y = 0; y < height; ++y) {
        const int py = y * SCREEN_HEIGHT / height;
        const size_t row = (size_t)(py / 2);
        for (int x = 0; x < width; ++x) {
            const int px = x * SCREEN_WIDTH / width;
            const size_t gate = SCREEN_WIDTH - 1 - px;
            const uint8_t pair = data[gate * (SCREEN_HEIGHT / 2) + row];
            const uint8_t ink = py & 1 ? pair & 15 : pair >> 4;
            uint8_t *out = rgba + ((size_t)y * width + x) * 4;
            memcpy(out, inks[ink < 7 ? ink : 1], 4);
        }
    }
    esp_partition_munmap(handle);
    return ESP_OK;
}

esp_err_t screen_rotation_toggle_blank(void)
{
    if (screen_rotation_hidden()) {
        esp_err_t err = s_ui_current ? screen_ui_redraw_rotation(screen_rotation_get()) :
            s_slot_current >= 0 ? replay_image(s_slot_current, screen_rotation_get(), true) :
            ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) s_hidden = false;
        return err;
    }
    s_hidden = false;
    if (!s_ui_current && (s_slot_current < 0 ||
        s_slots[s_slot_current].generation != screen_driver_generation()))
        return ESP_ERR_INVALID_STATE;
    screen_upload_report_t report = {0};
    esp_err_t err = screen_driver_clear(&report);
    if (err == ESP_OK) {
        s_hidden = true;
        s_hidden_generation = screen_driver_generation();
        s_overlay_shown = false;
        if (s_slot_current >= 0) s_slots[s_slot_current].generation = s_hidden_generation;
    }
    return err;
}

/* The side button no longer rotates the panel: it walks the stored picture
 * slots, so an awake device can preview the whole cycle by hand. */
static void picture_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (__atomic_load_n(&s_pending, __ATOMIC_ACQUIRE)) {
            /* Leave an accepted upload its entire session, including gaps
             * between HTTP requests, before applying queued button presses. */
            if (web_server_screen_busy()) { vTaskDelay(pdMS_TO_TICKS(25)); continue; }
            __atomic_store_n(&s_working, true, __ATOMIC_RELEASE);
            screen_rotation_lock();
            if (web_server_screen_busy()) {
                screen_rotation_unlock();
                __atomic_store_n(&s_working, false, __ATOMIC_RELEASE);
                vTaskDelay(pdMS_TO_TICKS(25)); continue;
            }
            (void)__atomic_exchange_n(&s_pending, 0, __ATOMIC_ACQ_REL);
            esp_err_t err;
            if (screen_rotation_hidden()) {
                err = ESP_OK;   /* A deliberately blanked panel stays blank. */
            } else if (screen_rotation_has_images()) {
                err = screen_rotation_show_next(true);
            } else {
                err = ESP_ERR_INVALID_STATE;
                ESP_LOGW(TAG, "next picture: no stored slot; upload one first");
            }
            screen_rotation_unlock();
            __atomic_store_n(&s_working, false, __ATOMIC_RELEASE);
            power_mgr_note_activity();
            ESP_LOGI(TAG, "side button next picture err=%s", esp_err_to_name(err));
        }
    }
}
static void picture_button(void *arg)
{
    (void)arg;
    int raw = gpio_get_level(EINK_PICTURE_BUTTON), stable = raw;
    TickType_t changed = xTaskGetTickCount();
    bool armed = false; /* Ignore a BOOT button still held during power-on. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const int value = gpio_get_level(EINK_PICTURE_BUTTON);
        const TickType_t now = xTaskGetTickCount();
        if (value != raw) { raw = value; changed = now; }
        if ((TickType_t)(now - changed) < pdMS_TO_TICKS(30)) continue;
        if (raw != EINK_PICTURE_BUTTON_ACTIVE_LEVEL) armed = true;
        if (raw == stable) continue;
        stable = raw;
        if (stable == EINK_PICTURE_BUTTON_ACTIVE_LEVEL && armed) {
            armed = false; /* Keep libc/logging off the small GPIO polling stack. */
            (void)__atomic_add_fetch(&s_pending, 1, __ATOMIC_ACQ_REL);
            xTaskNotifyGive(s_worker);
        }
    }
}
/* The three demo pictures are linked into the app so a fresh device shows a
 * picture instead of the setup text. They are seeded once, only when no slot
 * holds a verified frame; a device that already has pictures is left alone. */
extern const uint8_t _binary_pic0_bin_start[]
    asm("_binary_pic0_bin_start");
extern const uint8_t _binary_pic1_bin_start[]
    asm("_binary_pic1_bin_start");
extern const uint8_t _binary_pic2_bin_start[]
    asm("_binary_pic2_bin_start");
static const uint8_t *const s_default_frames[CACHE_SLOTS] = {
    _binary_pic0_bin_start,
    _binary_pic1_bin_start,
    _binary_pic2_bin_start,
};
static bool seed_default_slot(int index)
{
    const uint8_t *frame = s_default_frames[index];
    const uint32_t bytes = SCREEN_FRAME_BYTES;
    const size_t erase = CACHE_HEADER_BYTES + sector_size(bytes);
    if (erase > CACHE_SLOT_BYTES) return false;
    if (esp_partition_erase_range(s_partition, slot_base(index), erase) != ESP_OK) return false;
    if (esp_partition_write(s_partition, slot_data_base(index), frame, bytes) != ESP_OK) return false;
    slot_header_t head = {
        .magic = SLOT_MAGIC, .version = SLOT_VERSION, .kind = SCREEN_CACHE_OTP6,
        .rotation = (uint8_t)screen_rotation_get(), .planes = 1,
        .bytes = bytes, .fnv = fnv1a(2166136261U, frame, bytes),
    };
    head.blank[0] = head.blank[1] = head.blank[2] = SCREEN_COLOR_WHITE;
    if (esp_partition_write(s_partition, slot_base(index), &head, sizeof(head)) != ESP_OK) return false;
    s_slots[index] = (image_slot_t){.valid = true, .kind = SCREEN_CACHE_OTP6,
        .rotation = head.rotation, .planes = 1, .bytes = bytes, .tail = sector_size(bytes)};
    memcpy(s_slots[index].blank, head.blank, sizeof(s_slots[index].blank));
    return true;
}

esp_err_t screen_rotation_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "image_cache");
    if (!s_partition || s_partition->size < CACHE_META_OFFSET + CACHE_SECTOR_BYTES) {
        s_partition = NULL;
        ESP_LOGW(TAG, "image_cache partition unavailable; flash the updated partition table");
    }
    if (s_partition) {
        unsigned valid = 0;
        for (int i = 0; i < CACHE_SLOTS; ++i) if (slot_load(i)) ++valid;
        if (valid == 0) {
            for (int i = 0; i < CACHE_SLOTS; ++i) if (seed_default_slot(i)) ++valid;
            if (valid) ESP_LOGI(TAG, "seeded %u default pictures", valid);
        }
        const int saved = meta_load();
        if (saved >= 0 && s_slots[saved].valid) {
            s_slot_current = saved;
        } else if (valid) {
            for (int i = 0; i < CACHE_SLOTS && s_slot_current < 0; ++i) {
                if (s_slots[i].valid) s_slot_current = i;
            }
            meta_save();
        }
        /* The panel keeps its last image across a power cycle, so the stored
         * current slot is still what the viewer sees. */
        s_image_current = s_slot_current >= 0;
        ESP_LOGI(TAG, "image cache: %u/%d slots, current=%d",
                 valid, CACHE_SLOTS, s_slot_current);
    }
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << EINK_PICTURE_BUTTON, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) { vSemaphoreDelete(s_lock); s_lock = NULL; return err; }
    if (xTaskCreate(picture_worker, "picture_ui", 4096, NULL, 3, &s_worker) != pdPASS) {
        vSemaphoreDelete(s_lock); s_lock = NULL; return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(picture_button, "picture_key", 2048, NULL, 2, NULL) != pdPASS) {
        vTaskDelete(s_worker); s_worker = NULL; vSemaphoreDelete(s_lock); s_lock = NULL; return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "GPIO%d next-picture button ready, rotation=%u cache=%s",
        (int)EINK_PICTURE_BUTTON, screen_rotation_get(), s_partition ? "flash" : "unavailable");
    return ESP_OK;
}
