"""Portrait geometry and production LVGL display/touch mapping, without hardware."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/p4_main/main"
LVGL = ROOT / "firmware/p4_main/managed_components/lvgl__lvgl"
PORT = ROOT / "firmware/p4_main/managed_components/espressif__esp_lvgl_port"
TCC = ROOT / "logs/chat-host-tools/tcc/tcc.exe"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32)]


class Area(ctypes.Structure):
    _fields_ = [("x1", ctypes.c_int32), ("y1", ctypes.c_int32),
                ("x2", ctypes.c_int32), ("y2", ctypes.c_int32)]


class PortraitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TCC.exists():
            raise unittest.SkipTest("Optional host TCC unavailable")
        cls.temp = tempfile.TemporaryDirectory(dir=ROOT / "logs")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        display = (LVGL / "src/display/lv_display.c").read_text(encoding="utf-8")
        port = (PORT / "src/lvgl9/esp_lvgl_port_disp.c").read_text(encoding="utf-8")
        stub = r'''
#include <stdint.h>
#include "ui_layout.h"
#define LV_CHECK_ARG(condition, result) if (!(condition)) { result; }
typedef enum { LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90,
               LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270 } lv_display_rotation_t;
typedef struct { int32_t x, y; } lv_point_t;
typedef struct { int32_t x1, y1, x2, y2; } lv_area_t;
typedef struct { int32_t hor_res, ver_res; } lv_display_t;
int lv_display_get_rotation(lv_display_t *d) { return LV_DISPLAY_ROTATION_90; }
int lv_display_get_horizontal_resolution(lv_display_t *d) { return UI_WIDTH; }
int lv_display_get_vertical_resolution(lv_display_t *d) { return UI_HEIGHT; }
int lv_area_get_width(lv_area_t *a) { return a->x2 - a->x1 + 1; }
int lv_area_get_height(lv_area_t *a) { return a->y2 - a->y1 + 1; }
'''
        stub += function(display, "void lv_display_rotate_point(") + "\n"
        stub += function(port, "void lvgl_port_rotate_area(") + "\n"
        stub += '#include "portrait_frame_copy.h"\n'
        stub += r'''
__declspec(dllexport) void map_touch(lv_point_t *p) {
    lv_display_t d = {UI_HEIGHT, UI_WIDTH};
    lv_display_rotate_point(&d, p);
}
__declspec(dllexport) void map_area(lv_area_t *a) {
    lv_display_t d = {UI_HEIGHT, UI_WIDTH};
    lvgl_port_rotate_area(&d, a);
}
__declspec(dllexport) int geometry_valid(void) {
    return UI_CHAT_TOP + UI_CHAT_HEIGHT <= UI_HOME_INPUT_TOP - 94 &&
           UI_TRANSCRIPT_HEIGHT + 80 <= UI_HEIGHT - 180 &&
           UI_BUBBLE_TEXT_WIDTH + 24 + 44 + 10 + 24 <= UI_CONTENT_WIDTH &&
           UI_CONTENT_WIDTH + 2 * UI_MARGIN == UI_WIDTH;
}
__declspec(dllexport) void copy_frame(uint16_t *dst, int width, int height,
                                    const uint16_t *src, int stride,
                                    int x1, int y1, int x2, int y2) {
    portrait_frame_copy(dst, width, height, src, stride, x1, y1, x2, y2);
}
'''
        source = work / "portrait.c"
        source.write_text(stub, encoding="utf-8")
        compiled = subprocess.run([str(TCC), "-shared", "-I" + str(MAIN),
                                   str(source), "-o", str(work / "portrait.dll")],
                                  capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr)
        cls.lib = ctypes.CDLL(str(work / "portrait.dll"))
        free = ctypes.windll.kernel32.FreeLibrary
        free.argtypes = [ctypes.c_void_p]
        cls.addClassCleanup(free, cls.lib._handle)
        cls.lib.map_touch.argtypes = [ctypes.POINTER(Point)]
        cls.lib.map_area.argtypes = [ctypes.POINTER(Area)]

        transport = (MAIN / "portrait_display.c").read_text(encoding="utf-8")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "portrait_frame_copy.h"
typedef void *esp_lcd_panel_handle_t;
typedef void *SemaphoreHandle_t;
typedef void *ppa_client_handle_t;
typedef int ppa_srm_rotation_angle_t;
typedef int esp_err_t;
typedef int BaseType_t;
typedef void *dw_gdma_channel_handle_t;
typedef void dw_gdma_trans_done_event_data_t;
typedef bool (*dw_gdma_trans_done_event_callback_t)(void *, const void *, void *);
#define atomic_uint unsigned
#define memory_order_acquire 0
#define memory_order_release 0
#define memory_order_relaxed 0
#define atomic_load_explicit(p, order) (*(p))
#define atomic_store_explicit(p, v, order) (*(p) = (v))
#define atomic_store(p, v) (*(p) = (v))
#define atomic_fetch_add_explicit(p, v, order) ((*(p)) += (v))
#define IRAM_ATTR
#define pdFALSE 0
#define ESP_LOGI(...) ((void)0)
static int64_t test_time;
int64_t esp_timer_get_time(void) { return ++test_time; }
typedef struct {
    const void *buffer;
    int pic_w, pic_h, block_w, block_h, srm_cm;
} fake_in_t;
typedef struct {
    void *buffer;
    unsigned long long buffer_size;
    int pic_w, pic_h, block_offset_x, block_offset_y, srm_cm;
} fake_out_t;
typedef struct {
    fake_in_t in;
    fake_out_t out;
    int rotation_angle;
    float scale_x, scale_y;
    int mode;
} ppa_srm_oper_config_t;
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define PPA_SRM_ROTATION_ANGLE_90 90
#define PPA_SRM_ROTATION_ANGLE_0 0
#define PPA_SRM_ROTATION_ANGLE_180 180
#define PPA_SRM_ROTATION_ANGLE_270 270
#define PPA_SRM_COLOR_MODE_RGB565 1
#define PPA_TRANS_MODE_BLOCKING 1
typedef struct { int last; } lv_display_t;
typedef struct { int x1, y1, x2, y2; } lv_area_t;
#define LV_DISPLAY_ROTATION_90 1
#define LV_COLOR_FORMAT_RGB565 1
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_INVALID_ARG 2
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_ERROR_CHECK(x) do { if ((x) != 0) { errors++; return; } } while (0)
static int errors, waits, submitted, ready, copied;
static int cache_syncs, hardware_copies;
static int dma_mode, signals;
void simulate_retirement(void);
int xSemaphoreGiveFromISR(void *s, int *wake) { signals++; return 1; }
int esp_cache_msync(void *address, unsigned long long bytes, int flags) {
    cache_syncs++;
    return 0;
}
int ppa_do_scale_rotate_mirror(void *client, const ppa_srm_oper_config_t *c) {
    if (!cache_syncs || c->mode != PPA_TRANS_MODE_BLOCKING) errors++;
    hardware_copies++;
    const uint16_t *source = c->in.buffer;
    uint16_t *destination = c->out.buffer;
    int w = (int)(c->in.block_w * c->scale_x);
    int h = (int)(c->in.block_h * c->scale_y);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int dx = x, dy = y;
            switch (c->rotation_angle) {
                case 90: dx = y; dy = w - 1 - x; break;
                case 180: dx = w - 1 - x; dy = h - 1 - y; break;
                case 270: dx = h - 1 - y; dy = x; break;
            }
            destination[(c->out.block_offset_y + dy) * c->out.pic_w +
                        c->out.block_offset_x + dx] =
                source[(int)(y / c->scale_y) * c->in.pic_w + (int)(x / c->scale_x)];
        }
    return 0;
}
int lv_display_get_rotation(lv_display_t *d) { return 1; }
int lv_display_flush_is_last(lv_display_t *d) { return d->last; }
int lv_area_get_width(const lv_area_t *a) { return a->x2 - a->x1 + 1; }
int lv_draw_buf_width_to_stride(int w, int cf) { return w * 2; }
void lv_display_flush_ready(lv_display_t *d) { ready++; }
int xSemaphoreTake(void *s, int timeout) {
    if (!timeout) return 0;
    waits++;
    if (s == (void *)2) simulate_retirement();
    return pdTRUE;
}
int esp_lcd_panel_draw_bitmap(void *panel, int x1, int y1, int x2, int y2,
                              const void *pixels) {
    if ((!dma_mode && waits != submitted * 2) || x1 || y1 || x2 != 12 || y2 != 8) errors++;
    submitted++;
    return ESP_OK;
}
void *retired_copy(void *dst, const void *src, unsigned long long size) {
    if (!dma_mode && waits != submitted * 2) errors++;
    copied += size;
    return memcpy(dst, src, size);
}
'''
        start = transport.index("static struct {")
        end = transport.index("} scanout;", start) + len("} scanout;")
        harness += transport[start:end] + "\n"
        harness += "static bool editor_cpu_copy;\n"
        harness += "static int editor_copy_top;\n"
        harness += function(transport, "void portrait_editor_cpu_copy(")
        harness += function(transport, "void portrait_editor_cpu_region(")
        start = transport.index("static struct {", end)
        end = transport.index("} retirement;", start) + len("} retirement;")
        harness += transport[start:end] + "\n"
        harness += function(transport, "static bool IRAM_ATTR portrait_dma_done(")
        harness += r'''
static int driver_called, race_submission;
static bool fake_driver_done(void *channel, const void *event, void *context) {
    driver_called++;
    /* Ack must not be published until the real driver switches DMA lists. */
    if (retirement.retired == retirement.requested && retirement.requested) errors++;
    if (race_submission) retirement.requested++;
    return false;
}
void simulate_retirement(void) { portrait_dma_done(0, 0, 0); }
'''
        harness += function(transport, "static esp_err_t portrait_copy(")
        harness += "\n#define memcpy retired_copy\n"
        harness += function(transport, "static void portrait_sync_region(")
        harness += function(transport, "static void portrait_begin_frame(")
        harness += function(transport, "static void portrait_submit(void)")
        harness += function(transport, "esp_err_t portrait_preview_frame(")
        harness += function(transport, "void portrait_preview_stop(void)")
        harness += function(transport, "static void portrait_flush(")
        harness += r'''
#undef memcpy
__declspec(dllexport) int accelerated_case(void) {
    uint16_t source[72 * 65], actual[96 * 80], expected[96 * 80];
    errors = cache_syncs = hardware_copies = 0;
    scanout.ui_cpu_chunks = 0;
    for (int i = 0; i < 72 * 65; i++) source[i] = i;
    for (int i = 0; i < 96 * 80; i++) actual[i] = expected[i] = 777;
    portrait_copy(actual, 96, 80, source, 72, 3, 7, 67, 71);
    portrait_frame_copy(expected, 96, 80, source, 72, 3, 7, 67, 71);
    if (hardware_copies != 0 || cache_syncs != 0 ||
        memcmp(actual, expected, sizeof(actual))) errors++;
    portrait_copy(actual, 96, 80, source, 72, 0, 0, 1, 1);
    portrait_frame_copy(expected, 96, 80, source, 72, 0, 0, 1, 1);
    if (hardware_copies != 0 || memcmp(actual, expected, sizeof(actual))) errors++;
    portrait_editor_cpu_copy(true);
    portrait_copy(actual, 96, 80, source, 72, 3, 7, 67, 71);
    portrait_frame_copy(expected, 96, 80, source, 72, 3, 7, 67, 71);
    if (hardware_copies != 0 || memcmp(actual, expected, sizeof(actual))) errors++;
    portrait_editor_cpu_region(72);
    portrait_copy(actual, 96, 80, source, 72, 3, 7, 67, 71);
    if (hardware_copies != 0 || memcmp(actual, expected, sizeof(actual))) errors++;
    portrait_editor_cpu_copy(false);
    portrait_copy(actual, 96, 80, source, 72, 3, 7, 67, 71);
    portrait_frame_copy(expected, 96, 80, source, 72, 3, 7, 67, 71);
    if (hardware_copies != 0 || memcmp(actual, expected, sizeof(actual))) errors++;
    if (scanout.ui_cpu_chunks != 5) errors++;
    return errors;
}
__declspec(dllexport) int scanout_case(void) {
    uint16_t front[96], back[96], source[8];
    errors = waits = submitted = ready = copied = dma_mode = 0;
    scanout.ui_flush_done = 0;
    memset(&retirement, 0, sizeof(retirement));
    for (int i = 0; i < 96; ++i) front[i] = back[i] = 100;
    for (int i = 0; i < 8; ++i) source[i] = i + 1;
    scanout.frames[0] = front;
    scanout.frames[1] = back;
    scanout.width = 12;
    scanout.height = 8;
    scanout.back = 1;
    scanout.sync_pending = false;
    scanout.dirty = (lv_area_t){12, 8, -1, -1};
    lv_display_t d = {0};
    lv_area_t a = {1, 2, 2, 3};
    portrait_flush(&d, &a, (uint8_t *)source);
    if (ready != 1 || submitted || waits || scanout.back != 1) errors++;
    for (int i = 0; i < 96; ++i) if (front[i] != 100) errors++;
    d.last = 1;
    a = (lv_area_t){3, 4, 4, 5};
    portrait_flush(&d, &a, (uint8_t *)source);
    if (ready != 2 || submitted != 1 || waits || scanout.back != 0) errors++;
    if (copied) errors++;
    for (int i = 0; i < 96; ++i) if (front[i] != 100) errors++;
    if (back[6 * 12 + 2] != 1 || back[5 * 12 + 3] != 4 || back[0] != 100) errors++;
    a = (lv_area_t){0, 0, 0, 0};
    portrait_flush(&d, &a, (uint8_t *)source);
    if (ready != 3 || submitted != 2 || waits != 2 || scanout.back != 1) errors++;
    if (copied != 32 || front[7 * 12] != 1 || back[7 * 12] != 100) errors++;
    d.last = 0;
    portrait_flush(&d, &a, (uint8_t *)source);
    if (waits != 4 || copied != 34 || memcmp(front, back, sizeof(front))) errors++;
    if (front[6 * 12 + 2] != 1 || front[5 * 12 + 3] != 4 || front[0] != 100) errors++;
    if (scanout.ui_flush_done != 4) errors++;
    return errors;
}
__declspec(dllexport) int native_preview_case(void) {
    uint16_t front[96], back[96], source[16 * 12], ui[96];
    errors = waits = submitted = ready = copied = cache_syncs = hardware_copies = 0;
    memset(&scanout, 0, sizeof(scanout));
    memset(&retirement, 0, sizeof(retirement));
    dma_mode = 0;
    for (int i = 0; i < 96; i++) front[i] = back[i] = 777, ui[i] = 888;
    for (int i = 0; i < 16 * 12; i++) source[i] = i + 1;
    scanout.panel = (void *)1;
    scanout.frames[0] = front; scanout.frames[1] = back;
    scanout.width = 12; scanout.height = 8; scanout.back = 1;
    scanout.dirty = (lv_area_t){12, 8, -1, -1};
    lv_area_t preview = {1, 2, 6, 9};
    if (portrait_preview_frame(source, 16, 12, &preview, 90)) errors++;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 12; x++) {
            uint16_t expected = 777;
            if (y >= 1 && y <= 6 && x >= 2 && x <= 9)
                expected = source[(6 - y) * 2 * 16 + (9 - x) * 2];
            if (back[y * 12 + x] != expected || front[y * 12 + x] != 777) errors++;
        }
    /* Full camera replacement waits for retirement but copies no old image. */
    if (portrait_preview_frame(source, 16, 12, &preview, 90)) errors++;
    if (submitted != 2 || waits != 2 || copied || hardware_copies != 2) errors++;
    /* A full UI background repaint must preserve the current camera layer. */
    lv_display_t d = {1};
    lv_area_t all = {0, 0, 7, 11};
    portrait_flush(&d, &all, (uint8_t *)ui);
    if (waits != 4 || submitted != 3) errors++;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 12; x++) {
            uint16_t expected = 888;
            if (y >= 1 && y <= 6 && x >= 2 && x <= 9)
                expected = source[(6 - y) * 2 * 16 + (9 - x) * 2];
            if (back[y * 12 + x] != expected) errors++;
        }
    portrait_preview_stop();
    portrait_flush(&d, &all, (uint8_t *)ui);
    for (int i = 0; i < 96; i++) if (front[i] != 888) errors++;
    if (portrait_preview_frame(0, 16, 12, &preview, 90) != ESP_ERR_INVALID_ARG) errors++;
    if (hardware_copies != 2) errors++;
    return errors;
}
__declspec(dllexport) int dma_retirement_case(void) {
    errors = waits = signals = driver_called = race_submission = 0;
    memset(&retirement, 0, sizeof(retirement));
    retirement.driver_done = fake_driver_done;
    retirement.done = (void *)2;
    /* No pending generation: continuous LCD DMA callbacks need no wakeup. */
    portrait_dma_done(0, 0, 0);
    if (signals || driver_called != 1) errors++;
    retirement.requested = 1;
    portrait_dma_done(0, 0, 0);
    if (retirement.retired != 1 || signals != 1) errors++;
    /* A newer submission during the driver's callback must NOT be retired. */
    retirement.requested = 2;
    race_submission = 1;
    portrait_dma_done(0, 0, 0);
    if (retirement.requested != 3 || retirement.retired != 2 || signals != 2) errors++;
    race_submission = 0;
    dma_mode = 1;
    scanout.dirty = (lv_area_t){12, 8, -1, -1};
    scanout.sync_pending = true;
    portrait_begin_frame(NULL);
    if (waits != 1 || retirement.retired != 3 || scanout.sync_pending) errors++;
    scanout.sync_pending = true;
    portrait_begin_frame(NULL);
    if (waits != 1) errors++;
    /* 32-bit rollover is equality based, not an ordering comparison. */
    retirement.requested = 0;
    retirement.retired = UINT32_MAX;
    portrait_dma_done(0, 0, 0);
    if (retirement.retired != 0) errors++;
    dma_mode = 0;
    memset(&retirement, 0, sizeof(retirement));
    return errors;
}
'''
        harness += r'''
__declspec(dllexport) int dma_gap_case(void) {
    errors = 0;
    memset(&scanout, 0, sizeof(scanout));
    memset(&retirement, 0, sizeof(retirement));
    retirement.driver_done = fake_driver_done;
    race_submission = 0;
    test_time = UINT32_MAX - 100;
    portrait_dma_done(0, 0, 0);
    if (scanout.dma_late_count || scanout.dma_max_gap_us) errors++;
    test_time = (int64_t)UINT32_MAX + 200;
    portrait_dma_done(0, 0, 0);
    if (scanout.dma_late_count || scanout.dma_max_gap_us != 300) errors++;
    test_time += 49999;
    portrait_dma_done(0, 0, 0);
    if (scanout.dma_late_count) errors++;
    test_time += 50000;
    portrait_dma_done(0, 0, 0);
    if (scanout.dma_late_count != 1 || scanout.dma_late_gap_us != 50001 ||
        scanout.dma_max_gap_us != 50001 || scanout.dma_count != 4) errors++;
    return errors;
}
'''
        harness_path = work / "scanout.c"
        harness_path.write_text(harness, encoding="utf-8")
        compiled = subprocess.run([str(TCC), "-shared", "-I" + str(MAIN),
                                   str(harness_path), "-o", str(work / "scanout.dll")],
                                  capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr)
        cls.scanout_lib = ctypes.CDLL(str(work / "scanout.dll"))
        cls.addClassCleanup(free, cls.scanout_lib._handle)

    def test_touch_is_inverse_of_flush_at_corners_and_interior(self):
        for x in (0, 20, 299, 579, 599):
            for y in (0, 30, 511, 993, 1023):
                area = Area(x, y, x, y)
                self.lib.map_area(ctypes.byref(area))
                self.assertTrue(0 <= area.x1 < 1024)
                self.assertTrue(0 <= area.y1 < 600)
                point = Point(area.x1, area.y1)
                self.lib.map_touch(ctypes.byref(point))
                self.assertEqual((point.x, point.y), (x, y))

    def test_full_canvas_maps_to_physical_panel(self):
        area = Area(0, 0, 599, 1023)
        self.lib.map_area(ctypes.byref(area))
        self.assertEqual((area.x1, area.y1, area.x2, area.y2), (0, 0, 1023, 599))
        self.assertTrue(self.lib.geometry_valid())

    def test_tiled_frame_copy_matches_production_touch_mapping(self):
        # Odd dimensions, padded source rows, tile boundaries and panel edges.
        for x1, y1, width, height in ((0, 0, 19, 21), (17, 31, 33, 35),
                                      (579, 993, 21, 31), (0, 0, 600, 1024)):
            stride = width + 4
            source = (ctypes.c_uint16 * (stride * height))()
            destination = (ctypes.c_uint16 * (600 * 1024))()
            for y in range(height):
                for x in range(width):
                    source[y * stride + x] = (y * width + x) % 65534 + 1
            self.lib.copy_frame(destination, 1024, 600, source, stride,
                                x1, y1, x1 + width - 1, y1 + height - 1)
            for y in range(height):
                for x in range(width):
                    self.assertEqual(destination[(599 - x1 - x) * 1024 + y1 + y],
                                     source[y * stride + x])
            self.assertEqual(sum(value != 0 for value in destination), width * height)

    def test_scanout_only_presents_complete_frames_and_retires_before_reuse(self):
        self.assertEqual(self.scanout_lib.scanout_case(), 0)

    def test_ui_cpu_rotation_preserves_offsets_stride_and_ignores_editor_state(self):
        self.assertEqual(self.scanout_lib.accelerated_case(), 0)

    def test_direct_camera_matches_combined_rotation_and_preserves_ui_lifecycle(self):
        self.assertEqual(self.scanout_lib.native_preview_case(), 0)

    def test_dma_retirement_waits_for_driver_switch_and_handles_racing_submission(self):
        self.assertEqual(self.scanout_lib.dma_retirement_case(), 0)

    def test_dma_gap_diagnostics_handle_rollover_and_only_flag_long_intervals(self):
        self.assertEqual(self.scanout_lib.dma_gap_case(), 0)

    def test_startup_refresh_leaves_font_loading_windows_and_restores_ui_rate(self):
        main = (MAIN / "main.c").read_text(encoding="utf-8")
        startup = function(main, "static void create_startup_placeholder(void)\n{")
        completion = function(main, "static void startup_status_timer(lv_timer_t *timer)")
        self.assertIn("lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()), 80)", startup)
        self.assertIn("lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()), LV_DEF_REFR_PERIOD)", completion)
        self.assertIn('xTaskCreate(startup_resource_task, "startup_resources", 12288, NULL, 2, NULL)', main)

    def test_rotation_uses_partial_rendering_and_keeps_camera_full_view(self):
        config = (MAIN.parent / "sdkconfig.sign").read_text(encoding="utf-8")
        self.assertNotIn("CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR=y", config)
        self.assertNotIn("CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE=y", config)
        main = (MAIN / "main.c").read_text(encoding="utf-8")
        self.assertIn("lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90)", main)
        camera = (MAIN / "sign_language_mode.c").read_text(encoding="utf-8")
        self.assertIn("&area, PPA_SRM_ROTATION_ANGLE_90", camera)
        self.assertIn("#define PREVIEW_WIDTH 540", camera)
        self.assertIn("#define PREVIEW_HEIGHT 960", camera)
        self.assertIn("portrait_preview_frame(completed_frame, SENSOR_WIDTH, SENSOR_HEIGHT", camera)
        self.assertNotIn("lv_image_set_src", camera)
        self.assertNotIn("display_frame_buffers", camera)

    def test_home_status_wraps_without_marquee_and_clears_chat(self):
        main = (MAIN / "main.c").read_text(encoding="utf-8")
        home = function(main, "static void create_diagnostics_ui(void)\n{")
        self.assertIn("lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP)", home)
        self.assertNotIn("LV_LABEL_LONG_SCROLL", home)
        self.assertIn("lv_obj_set_size(status_label, UI_CONTENT_WIDTH, LV_SIZE_CONTENT)", home)
        layout = (MAIN / "ui_layout.h").read_text(encoding="utf-8")
        import re
        values = {key: int(value) for key, value in
                  re.findall(r"#define (UI_\w+) (\d+)", layout)}
        self.assertGreaterEqual(values["UI_STATUS_HEIGHT"], 2 * 54)
        self.assertEqual(values["UI_STATUS_TOP"] + 27 + 16, values["UI_CHAT_TOP"])
        self.assertEqual(values["UI_CHAT_TOP"] + values["UI_CHAT_HEIGHT"], values["UI_HOME_INPUT_TOP"] - 94)
        self.assertEqual(values["UI_HOME_INPUT_TOP"] + values["UI_INPUT_COLLAPSED"] + 20,
                         values["UI_HEIGHT"])
        self.assertEqual(152 + values["UI_TRANSCRIPT_HEIGHT"], values["UI_ASR_INPUT_TOP"] - 158)
        self.assertGreaterEqual(values["UI_HEIGHT"] - values["UI_MARGIN"] -
                                values["UI_INPUT_TOUCH"] - 20 - values["UI_CHAT_TOP"], 100)

    def test_settings_scroll_has_no_elastic_redraw_and_transport_is_optimized(self):
        main = (MAIN / "main.c").read_text(encoding="utf-8")
        start = main.index("lv_obj_set_size(list, UI_CONTENT_WIDTH, UI_SETTINGS_HEIGHT)")
        self.assertIn("lv_obj_set_scroll_elastic(list, false)", main[start:start + 700])
        cmake = (MAIN / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('set_source_files_properties(portrait_display.c PROPERTIES COMPILE_OPTIONS "-O2")', cmake)


if __name__ == "__main__":
    unittest.main()
