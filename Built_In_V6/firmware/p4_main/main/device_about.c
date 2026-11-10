#include "device_about.h"
#include "ui_layout.h"
#include "qinghe_ui.h"
#include "asr_display_font.h"
#include "speech_to_text_mode.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "screen_idle.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>

static lv_obj_t *screen, *previous_screen, *list, *bodies[4], *headings[4], *page_title, *hero_title, *notice;
static atomic_int result_state;
static char info[4][2048];
static unsigned section;
static bool wifi_up, cloud_up;
static unsigned brightness_value, volume_value, font_value;
static lv_font_t *cached_fonts[2];
const lv_font_t *settings_cached_font(const lv_font_t *source)
{
#if CONFIG_P4_ASR_MODE
    int slot = source == speech_to_text_mode_font14() ? 0 :
               source == speech_to_text_mode_font28() ? 1 : -1;
    if (!source || slot < 0) return source;
    if (!cached_fonts[slot]) cached_fonts[slot] = asr_display_font_create_native(source);
    return cached_fonts[slot] ? cached_fonts[slot] : source;
#else
    return source;
#endif
}

static void append(size_t *used, const char *format, ...)
{
    if (*used >= sizeof(info[section]) - 1) return;
    va_list args; va_start(args, format);
    int n = vsnprintf(info[section] + *used, sizeof(info[section]) - *used, format, args);
    va_end(args);
    if (n > 0) *used += (size_t)n < sizeof(info[section]) - *used ? (size_t)n : sizeof(info[section]) - *used - 1;
}
static void collect_info(void *argument)
{
    (void)argument;
    size_t used = 0;
    section = 0;
    esp_chip_info_t chip; esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();
    uint32_t flash_size = 0;
    append(&used, "开发板：P4 Function EV Board\n");
    append(&used, "芯片：ESP32-P4，%u核，修订版%u.%u\n", chip.cores, chip.revision / 100, chip.revision % 100);
    append(&used, "屏幕：600 × 1024，竖屏触摸\n摄像头：CSI 预览\n");
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK)
        append(&used, "Flash：%lu MiB\n", (unsigned long)(flash_size / 1048576));
    append(&used, "PSRAM：总计%u KiB，空闲%u KiB\n内部RAM：总计%u KiB，空闲%u KiB",
        (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    section = 1; used = 0;
    append(&used, "固件：%s\n源码提交：%s\n构建：%s %s\nESP-IDF：%s\n",
        app->version, P4_BUILD_GIT, app->date, app->time, esp_get_idf_version());
    append(&used, "功能：AI语音对话、语音转文字、SD对话历史\n手语模型：尚未接入");
    section = 2; used = 0;
    const esp_partition_t *running = esp_ota_get_running_partition();
    append(&used, "运行分区：%s\n分区列表（容量）：\n", running ? running->label : "未知");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        append(&used, "  %s：%lu KiB\n", p->label, (unsigned long)(p->size / 1024));
        it = esp_partition_next(it);
    }
    uint64_t total, free_bytes;
    if (esp_vfs_fat_info("/sdcard", &total, &free_bytes) == ESP_OK)
        append(&used, "SD卡：总计%llu MiB，已用%llu MiB，剩余%llu MiB\n",
            (unsigned long long)(total / 1048576), (unsigned long long)((total - free_bytes) / 1048576),
            (unsigned long long)(free_bytes / 1048576));
    else append(&used, "SD卡：容量暂不可读\n");
    section = 3; used = 0;
    append(&used, "Wi-Fi：%s\nAI连接：%s\n亮度：%u%%，音量：%u%%\n全局字号：",
        wifi_up ? "已连接" : "未连接", cloud_up ? "已连接" : "未连接", brightness_value, volume_value);
    if (font_value) append(&used, "%upx\n", font_value);
    else append(&used, "页面默认\n");
    unsigned timeout = screen_idle_get_timeout();
    if (timeout) append(&used, "自动息屏：%u秒%s\n", timeout, timeout == 5 ? "（测试）" : "");
    else append(&used, "自动息屏：永不\n");
    append(&used, "本次运行：%llu秒",
        (unsigned long long)(esp_timer_get_time() / 1000000));
    atomic_store(&result_state, 2);
    vTaskDelete(NULL);
}
static void back_callback(lv_event_t *event)
{
    (void)event;
    if (previous_screen) qh_screen_load(previous_screen, true);
}
static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (lv_screen_active() == screen && atomic_load(&result_state) == 2) {
        for (unsigned i = 0; i < 4; ++i) lv_label_set_text(bodies[i], info[i]);
        lv_label_set_text(notice, "信息快照 · 重新进入可刷新");
        atomic_store(&result_state, 0);
    }
}
static void bind_font(lv_obj_t *object, const lv_font_t *font)
{
    lv_obj_set_style_text_font(object, font, 0);
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        bind_font(lv_obj_get_child(object, i), font);
}
void device_about_show(lv_obj_t *return_screen, const lv_font_t *font,
                       bool wifi, bool cloud, unsigned brightness, unsigned volume, unsigned font_size)
{
    font = settings_cached_font(font);
    previous_screen = return_screen;
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_scrollable(screen, false);
        lv_obj_set_style_bg_color(screen, qh_color(QH_BG), 0);
        qh_screen(screen);
        page_title=qh_header(screen,"关于设备",font,back_callback,true);
        lv_obj_t *hero = lv_obj_create(screen);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, UI_CONTENT_WIDTH, 160);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 110);
        lv_obj_set_style_bg_color(hero, qh_color(QH_SOFT), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hero, 16, 0);
        lv_obj_set_scrollable(hero, false);
        lv_obj_t *badge = lv_obj_create(hero);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 72, 72);
        lv_obj_set_pos(badge, 8, 4);
        lv_obj_set_style_bg_color(badge, qh_color(QH_SOFT), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, 14, 0);
        lv_obj_set_scrollable(badge, false);
        lv_obj_t *mark = qh_logo(badge); lv_obj_center(mark);
        hero_title = lv_label_create(hero);
        lv_label_set_text(hero_title, "灵译通");
        lv_obj_set_pos(hero_title, 92, 12);
        lv_obj_set_width(hero_title, UI_CONTENT_WIDTH - 110);
        lv_label_set_long_mode(hero_title, LV_LABEL_LONG_WRAP);
        lv_obj_t *caption = lv_label_create(hero);
        lv_label_set_text(caption, "ESP32-P4X");
        lv_obj_set_style_text_color(caption, qh_color(QH_MUTED), 0);
        lv_obj_set_pos(caption, 92, 84);
        lv_obj_set_width(caption, UI_CONTENT_WIDTH - 110);
        list = lv_obj_create(screen);
        lv_obj_remove_style_all(list);
        lv_obj_set_size(list, UI_CONTENT_WIDTH, 590);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 290);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_pad_right(list, 10, 0);
        lv_obj_set_style_pad_column(list, 18, 0);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(list, 16, 0);
        const char *titles[] = {"硬件与内存", "固件与功能", "存储与分区", "连接与当前设置"};
        const uint32_t colors[] = {QH_LINK, QH_LINK, QH_WARN, QH_LINK};
        for (unsigned i = 0; i < 4; ++i) {
            lv_obj_t *card = lv_obj_create(list);
            lv_obj_remove_style_all(card);
            lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(card, qh_color(QH_SURFACE), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(card, 16, 0);
            lv_obj_set_style_border_color(card, qh_color(QH_LINE), 0);
            lv_obj_set_style_border_width(card, 1, 0);
            lv_obj_set_style_pad_all(card, 18, 0);
            lv_obj_set_style_pad_row(card, 12, 0);
            lv_obj_set_scrollable(card, false);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            headings[i] = lv_label_create(card);
            lv_label_set_text(headings[i], titles[i]);
            lv_obj_set_style_text_color(headings[i], qh_color(colors[i]), 0);
            bodies[i] = lv_label_create(card);
            lv_obj_set_width(bodies[i], LV_PCT(100));
            lv_label_set_long_mode(bodies[i], LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(bodies[i], 6, 0);
        }
        notice = lv_label_create(screen);
        lv_obj_set_width(notice, UI_CONTENT_WIDTH);
        lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(notice, qh_color(QH_MUTED), 0);
        lv_obj_align(notice, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_timer_create(refresh, 150, NULL);
    }
    /* Text children inherit this font and color, including the scroll panel. */
    bind_font(screen, font);
#if CONFIG_P4_ASR_MODE
    const lv_font_t *heading_font = settings_cached_font(speech_to_text_mode_font28());
    if (heading_font) {
        lv_obj_set_style_text_font(page_title, heading_font, 0);
        lv_obj_set_style_text_font(hero_title, heading_font, 0);
        for (unsigned i = 0; i < 4; ++i) lv_obj_set_style_text_font(headings[i], heading_font, 0);
    }
#endif
    qh_screen_load(screen, false);
    if (atomic_load(&result_state) == 1) return;
    wifi_up = wifi; cloud_up = cloud;
    brightness_value = brightness; volume_value = volume; font_value = font_size;
    atomic_store(&result_state, 1);
    for (unsigned i = 0; i < 4; ++i) lv_label_set_text(bodies[i], "读取中...");
    lv_label_set_text(notice, "正在读取设备信息...");
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    if (xTaskCreate(collect_info, "device_info", 4096, NULL, 1, NULL) != pdPASS) {
        atomic_store(&result_state, 0);
        lv_label_set_text(notice, "设备信息读取失败，请返回后重试");
    }
}
