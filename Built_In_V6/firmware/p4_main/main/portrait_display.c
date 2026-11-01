#include <assert.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "driver/ppa.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp_private/dw_gdma.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "portrait_frame_copy.h"
#include "portrait_display.h"
#include "ui_layout.h"
#include "screen_idle.h"

static struct {
    esp_lcd_panel_handle_t panel;
    uint16_t *frames[2];
    SemaphoreHandle_t refresh_done;
    unsigned back;
    int width;
    int height;
    lv_area_t dirty;
    bool sync_pending;
    ppa_client_handle_t rotation;
    bool preview_active;
    lv_area_t preview;
    uint64_t wait_us, cache_us, ppa_us, submit_us;
    unsigned profile_frames;
    atomic_uint dma_count, dma_last_us, dma_max_gap_us;
    atomic_uint dma_late_count, dma_late_at_us, dma_late_gap_us;
    unsigned submitted;
} scanout;
static bool editor_cpu_copy;
static int editor_copy_top;

void portrait_editor_cpu_copy(bool enabled) { editor_cpu_copy = enabled; }
void portrait_editor_cpu_region(int logical_top)
{
    editor_cpu_copy = true;
    editor_copy_top = logical_top;
}

/* This adapter depends on the pinned DPI driver's DMA callback restarting the
 * next frame before returning. No driver internals or dependency files change. */
_Static_assert(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5, 5, 4),
               "Review DPI DMA retirement adapter for the new IDF version");
static struct {
    dw_gdma_trans_done_event_callback_t driver_done;
    SemaphoreHandle_t done;
    atomic_uint requested;
    atomic_uint retired;
} retirement;

bool mipi_dsi_dma_trans_done_cb(dw_gdma_channel_handle_t channel,
                                const dw_gdma_trans_done_event_data_t *event,
                                void *context);

static bool IRAM_ATTR portrait_dma_done(dw_gdma_channel_handle_t channel,
                                        const dw_gdma_trans_done_event_data_t *event,
                                        void *context)
{
    const uint32_t now = (uint32_t)esp_timer_get_time();
    const unsigned previous = atomic_load_explicit(&scanout.dma_last_us, memory_order_relaxed);
    const unsigned count = atomic_load_explicit(&scanout.dma_count, memory_order_relaxed);
    if (count) {
        const uint32_t gap = now - previous;
        if (gap > atomic_load_explicit(&scanout.dma_max_gap_us, memory_order_relaxed))
            atomic_store_explicit(&scanout.dma_max_gap_us, gap, memory_order_relaxed);
        if (gap > 50000) {
            atomic_store_explicit(&scanout.dma_late_gap_us, gap, memory_order_relaxed);
            atomic_store_explicit(&scanout.dma_late_at_us, now, memory_order_relaxed);
            atomic_store_explicit(&scanout.dma_late_count,
                atomic_load_explicit(&scanout.dma_late_count, memory_order_relaxed) + 1,
                memory_order_relaxed);
        }
    }
    atomic_store_explicit(&scanout.dma_last_us, now, memory_order_relaxed);
    atomic_store_explicit(&scanout.dma_count, count + 1, memory_order_relaxed);
    /* Snapshot BEFORE the driver reads its next-frame index. A submission
     * racing this ISR is acknowledged only on a later DMA completion. */
    const unsigned generation = atomic_load_explicit(&retirement.requested, memory_order_acquire);
    bool wake_driver = retirement.driver_done(channel, event, context);
    if (generation != atomic_load_explicit(&retirement.retired, memory_order_relaxed)) {
        atomic_store_explicit(&retirement.retired, generation, memory_order_release);
        BaseType_t wake = pdFALSE;
        xSemaphoreGiveFromISR(retirement.done, &wake);
        wake_driver |= wake == pdTRUE;
    }
    return wake_driver;
}

esp_err_t __real_dw_gdma_channel_register_event_callbacks(dw_gdma_channel_handle_t channel,
                                                          dw_gdma_event_callbacks_t *callbacks,
                                                          void *context);

esp_err_t __wrap_dw_gdma_channel_register_event_callbacks(dw_gdma_channel_handle_t channel,
                                                          dw_gdma_event_callbacks_t *callbacks,
                                                          void *context)
{
    if (!callbacks || callbacks->on_full_trans_done != mipi_dsi_dma_trans_done_cb)
        return __real_dw_gdma_channel_register_event_callbacks(channel, callbacks, context);
    assert(retirement.driver_done == NULL);
    retirement.done = xSemaphoreCreateBinary();
    if (!retirement.done) return ESP_ERR_NO_MEM;
    retirement.driver_done = callbacks->on_full_trans_done;
    dw_gdma_event_callbacks_t adapted = *callbacks;
    adapted.on_full_trans_done = portrait_dma_done;
    const esp_err_t result = __real_dw_gdma_channel_register_event_callbacks(channel, &adapted, context);
    if (result != ESP_OK) {
        retirement.driver_done = NULL;
        vSemaphoreDelete(retirement.done);
        retirement.done = NULL;
    }
    return result;
}

static void portrait_sync_region(const lv_area_t *region,
                                  const lv_area_t *overwrite)
{
    for (int y = region->y1; y <= region->y2; ++y) {
        int left_end = region->x2;
        int right_start = region->x2 + 1;
        if (overwrite && y >= overwrite->y1 && y <= overwrite->y2) {
            if (overwrite->x1 - 1 < left_end) left_end = overwrite->x1 - 1;
            right_start = overwrite->x2 + 1;
            if (right_start < region->x1) right_start = region->x1;
        }
        if (left_end >= region->x1) {
            const size_t offset = (size_t)y * scanout.width + region->x1;
            memcpy(scanout.frames[scanout.back] + offset,
                   scanout.frames[scanout.back ^ 1] + offset,
                   (left_end - region->x1 + 1) * sizeof(uint16_t));
        }
        if (right_start <= region->x2) {
            const size_t offset = (size_t)y * scanout.width + right_start;
            memcpy(scanout.frames[scanout.back] + offset,
                   scanout.frames[scanout.back ^ 1] + offset,
                   (region->x2 - right_start + 1) * sizeof(uint16_t));
        }
    }
}

static void portrait_begin_frame(const lv_area_t *overwrite)
{
    if (!scanout.sync_pending) return;
    if (retirement.driver_done) {
        const unsigned generation = atomic_load_explicit(&retirement.requested, memory_order_acquire);
        while (atomic_load_explicit(&retirement.retired, memory_order_acquire) != generation) {
            ESP_ERROR_CHECK(xSemaphoreTake(retirement.done, pdMS_TO_TICKS(250)) == pdTRUE
                            ? ESP_OK : ESP_ERR_TIMEOUT);
        }
    } else {
        for (unsigned boundary = 0; boundary < 2; ++boundary) {
            ESP_ERROR_CHECK(xSemaphoreTake(scanout.refresh_done, pdMS_TO_TICKS(250)) == pdTRUE
                            ? ESP_OK : ESP_ERR_TIMEOUT);
        }
    }
    /* Camera frames replace their entire rectangle; do not copy old pixels
     * that will immediately be overwritten by PPA. */
    portrait_sync_region(&scanout.dirty, overwrite);
    scanout.dirty = (lv_area_t){scanout.width, scanout.height, -1, -1};
    scanout.sync_pending = false;
}

static void portrait_submit(void)
{
    while (xSemaphoreTake(scanout.refresh_done, 0) == pdTRUE) {}
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(scanout.panel, 0, 0,
                                             scanout.width, scanout.height,
                                             scanout.frames[scanout.back]));
    /* Publish after the driver has installed the next framebuffer index. */
    atomic_fetch_add_explicit(&retirement.requested, 1, memory_order_release);
    scanout.back ^= 1;
    scanout.sync_pending = true;
    scanout.submitted++;
}

/* Never log from the frame ISR. A low-rate UI timer reports numeric evidence
 * only; it does not submit frames, change PWM, or adjust display timing. */
static void portrait_health(lv_timer_t *timer)
{
    (void)timer;
    static unsigned polls, last_late, last_pwm = UINT_MAX;
    const unsigned late = atomic_load(&scanout.dma_late_count);
    const unsigned pwm = ledc_get_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH);
    if (++polls < 10 && late == last_late && pwm == last_pwm) return;
    polls = 0;
    last_late = late;
    last_pwm = pwm;
    const uint32_t now = (uint32_t)esp_timer_get_time();
    ESP_LOGI("portrait_display", "display_health dma=%u submitted=%u max_gap_us=%u late=%u"
             " late_gap_us=%u late_age_us=%u pwm=%u timeout_s=%u",
             atomic_load(&scanout.dma_count), scanout.submitted,
             atomic_load(&scanout.dma_max_gap_us), late,
             atomic_load(&scanout.dma_late_gap_us),
             late ? now - atomic_load(&scanout.dma_late_at_us) : 0,
             pwm, screen_idle_get_timeout());
}

esp_err_t portrait_preview_frame(const uint16_t *source, int width, int height,
                                 const lv_area_t *logical,
                                 ppa_srm_rotation_angle_t camera_rotation)
{
    if (!source || !logical || !scanout.panel || width <= 0 || height <= 0 ||
        logical->x1 < 0 || logical->y1 < 0 || logical->x2 >= scanout.height ||
        logical->y2 >= scanout.width || logical->x2 < logical->x1 ||
        logical->y2 < logical->y1) return ESP_ERR_INVALID_ARG;
    ppa_srm_rotation_angle_t rotation;
    switch (camera_rotation) {
        case PPA_SRM_ROTATION_ANGLE_0: rotation = PPA_SRM_ROTATION_ANGLE_90; break;
        case PPA_SRM_ROTATION_ANGLE_90: rotation = PPA_SRM_ROTATION_ANGLE_180; break;
        case PPA_SRM_ROTATION_ANGLE_180: rotation = PPA_SRM_ROTATION_ANGLE_270; break;
        case PPA_SRM_ROTATION_ANGLE_270: rotation = PPA_SRM_ROTATION_ANGLE_0; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    const lv_area_t native = {
        logical->y1, scanout.height - 1 - logical->x2,
        logical->y2, scanout.height - 1 - logical->x1,
    };
    int64_t stage = esp_timer_get_time();
    portrait_begin_frame(&native);
    scanout.wait_us += esp_timer_get_time() - stage;
    stage = esp_timer_get_time();
    const int native_width = native.x2 - native.x1 + 1;
    const int native_height = native.y2 - native.y1 + 1;
    uint16_t *back = scanout.frames[scanout.back];
    esp_err_t result = esp_cache_msync(back + native.y1 * scanout.width,
                                       (size_t)native_height * scanout.width * sizeof(uint16_t),
                                       ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (result != ESP_OK) return result;
    scanout.cache_us += esp_timer_get_time() - stage;
    stage = esp_timer_get_time();
    const bool swapped = rotation == PPA_SRM_ROTATION_ANGLE_90 ||
                         rotation == PPA_SRM_ROTATION_ANGLE_270;
    const ppa_srm_oper_config_t transform = {
        .in = {.buffer = source, .pic_w = width, .pic_h = height,
               .block_w = width, .block_h = height,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = back,
                .buffer_size = (size_t)scanout.width * scanout.height * sizeof(uint16_t),
                .pic_w = scanout.width, .pic_h = scanout.height,
                .block_offset_x = native.x1, .block_offset_y = native.y1,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = rotation,
        .scale_x = (float)(swapped ? native_height : native_width) / width,
        .scale_y = (float)(swapped ? native_width : native_height) / height,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    result = ppa_do_scale_rotate_mirror(scanout.rotation, &transform);
    if (result != ESP_OK) return result;
    scanout.ppa_us += esp_timer_get_time() - stage;
    stage = esp_timer_get_time();
    scanout.dirty = scanout.preview = native;
    scanout.preview_active = true;
    portrait_submit();
    scanout.submit_us += esp_timer_get_time() - stage;
    if (++scanout.profile_frames == 30) {
        ESP_LOGI("portrait_display", "preview_stages: wait_us=%" PRIu64 " cache_us=%" PRIu64
                 " ppa_us=%" PRIu64 " submit_us=%" PRIu64 " dma_retirement=%u",
                 scanout.wait_us / 30, scanout.cache_us / 30,
                 scanout.ppa_us / 30, scanout.submit_us / 30,
                 retirement.driver_done != NULL);
        scanout.profile_frames = 0;
        scanout.wait_us = scanout.cache_us = scanout.ppa_us = scanout.submit_us = 0;
    }
    return ESP_OK;
}

void portrait_preview_stop(void)
{
    scanout.preview_active = false;
    scanout.profile_frames = 0;
    scanout.wait_us = scanout.cache_us = scanout.ppa_us = scanout.submit_us = 0;
}

static esp_err_t portrait_copy(uint16_t *dst, int panel_width, int panel_height,
                               const uint16_t *src, int src_stride,
                               int x1, int y1, int x2, int y2)
{
    const int width = x2 - x1 + 1;
    const int height = y2 - y1 + 1;
    /* Editor dirty rectangles stalled in the hardware flush path. Keep their
     * copies synchronous in software; framebuffer retirement is unchanged. */
    if ((editor_cpu_copy && y2 >= editor_copy_top) || width * height < 4096) {
        portrait_frame_copy(dst, panel_width, panel_height, src, src_stride,
                             x1, y1, x2, y2);
        return ESP_OK;
    }
    const int native_y = panel_height - 1 - x2;
    /* PPA invalidates entire destination rows, including pixels outside its
     * target block. Preserve any CPU dirty-region copies before that happens. */
    esp_err_t result = esp_cache_msync(dst + native_y * panel_width,
                                       (size_t)width * panel_width * sizeof(uint16_t),
                                       ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (result != ESP_OK) return result;
    const ppa_srm_oper_config_t rotate = {
        .in = {
            .buffer = src, .pic_w = src_stride, .pic_h = height,
            .block_w = width, .block_h = height,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = (size_t)panel_width * panel_height * sizeof(uint16_t),
            .pic_w = panel_width, .pic_h = panel_height,
            .block_offset_x = y1, .block_offset_y = native_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(scanout.rotation, &rotate);
}

static bool IRAM_ATTR portrait_refresh_done(esp_lcd_panel_handle_t panel,
                                          esp_lcd_dpi_panel_event_data_t *event,
                                          void *context)
{
    (void)panel;
    (void)event;
    (void)context;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(scanout.refresh_done, &wake);
    return wake == pdTRUE;
}

static void portrait_flush(lv_display_t *display, const lv_area_t *area,
                           uint8_t *pixels)
{
    assert(lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_90);
    assert(area->x1 >= 0 && area->x2 < scanout.height);
    assert(area->y1 >= 0 && area->y2 < scanout.width);
    portrait_begin_frame(NULL);
    const int stride = lv_draw_buf_width_to_stride(lv_area_get_width(area),
                                                   LV_COLOR_FORMAT_RGB565) / 2;
    ESP_ERROR_CHECK(portrait_copy(scanout.frames[scanout.back], scanout.width,
                        scanout.height, (const uint16_t *)pixels, stride,
                        area->x1, area->y1, area->x2, area->y2));
    const lv_area_t physical = {
        .x1 = area->y1, .x2 = area->y2,
        .y1 = scanout.height - 1 - area->x2,
        .y2 = scanout.height - 1 - area->x1,
    };
    if (physical.x1 < scanout.dirty.x1) scanout.dirty.x1 = physical.x1;
    if (physical.y1 < scanout.dirty.y1) scanout.dirty.y1 = physical.y1;
    if (physical.x2 > scanout.dirty.x2) scanout.dirty.x2 = physical.x2;
    if (physical.y2 > scanout.dirty.y2) scanout.dirty.y2 = physical.y2;

    if (lv_display_flush_is_last(display)) {
        if (scanout.preview_active) {
            /* UI header/background redraws must not erase the direct camera
             * layer. Restore only its intersection with the UI dirty area. */
            lv_area_t overlap = scanout.preview;
            if (scanout.dirty.x1 > overlap.x1) overlap.x1 = scanout.dirty.x1;
            if (scanout.dirty.x2 < overlap.x2) overlap.x2 = scanout.dirty.x2;
            if (scanout.dirty.y1 > overlap.y1) overlap.y1 = scanout.dirty.y1;
            if (scanout.dirty.y2 < overlap.y2) overlap.y2 = scanout.dirty.y2;
            if (overlap.x1 <= overlap.x2 && overlap.y1 <= overlap.y2)
                portrait_sync_region(&overlap, NULL);
        }
        portrait_submit();
    }
    lv_display_flush_ready(display);
}

lv_display_t *__real_lvgl_port_add_disp_dsi(const lvgl_port_display_cfg_t *config,
                                           const lvgl_port_display_dsi_cfg_t *dsi);

/* Project-local link adapter: retain BSP ownership of LCD, touch and rotation;
 * replace only the partial-to-scanout transport, without editing dependencies. */
lv_display_t *__wrap_lvgl_port_add_disp_dsi(const lvgl_port_display_cfg_t *config,
                                           const lvgl_port_display_dsi_cfg_t *dsi)
{
    lvgl_port_lock(0);
    lv_display_t *display = __real_lvgl_port_add_disp_dsi(config, dsi);
    if (display != NULL) {
        assert(scanout.panel == NULL);
        assert(config->hres == UI_HEIGHT && config->vres == UI_WIDTH);
        assert(config->color_format == LV_COLOR_FORMAT_RGB565);
        assert(!config->flags.swap_bytes);
        assert(!config->flags.direct_mode && !config->flags.full_refresh);
        scanout.panel = config->panel_handle;
        scanout.width = config->hres;
        scanout.height = config->vres;
        scanout.dirty = (lv_area_t){scanout.width, scanout.height, -1, -1};
        scanout.back = 1;
        scanout.refresh_done = xSemaphoreCreateCounting(2, 0);
        ESP_ERROR_CHECK(scanout.refresh_done != NULL ? ESP_OK : ESP_ERR_NO_MEM);
        void *front = NULL;
        void *back = NULL;
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(scanout.panel, 2,
                                                         &front, &back));
        scanout.frames[0] = front;
        scanout.frames[1] = back;
        const ppa_client_config_t rotation_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        ESP_ERROR_CHECK(ppa_register_client(&rotation_config, &scanout.rotation));
        memcpy(scanout.frames[1], scanout.frames[0],
               (size_t)scanout.width * scanout.height * sizeof(uint16_t));
        const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
            .on_refresh_done = portrait_refresh_done,
        };
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(
            scanout.panel, &callbacks, NULL));
        lv_display_set_flush_cb(display, portrait_flush);
        lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
        lv_timer_create(portrait_health, 1000, NULL);
        ESP_LOGI("portrait_display", "complete-frame scanout: two LCD buffers, VSYNC retirement");
    }
    lvgl_port_unlock();
    return display;
}
