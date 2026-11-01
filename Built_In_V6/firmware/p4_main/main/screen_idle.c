#include "screen_idle.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <limits.h>

static atomic_uint timeout_seconds;
static bool (*busy_callback)(void);
static unsigned (*brightness_callback)(void);
static lv_obj_t *wake_surface;

bool screen_idle_timeout_valid(unsigned seconds)
{
    return seconds == 0 || seconds == 30 || seconds == 60 ||
           seconds == 120 || seconds == 300;
}
void screen_idle_set_timeout(unsigned seconds)
{
    if (screen_idle_timeout_valid(seconds)) atomic_store(&timeout_seconds, seconds);
}
unsigned screen_idle_get_timeout(void) { return atomic_load(&timeout_seconds); }

static bool touch_held(void)
{
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input))
        if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) return true;
    return false;
}
static void wake_screen(void)
{
    unsigned brightness = brightness_callback ? brightness_callback() : 70;
    esp_err_t error = bsp_display_brightness_set(brightness);
    if (error != ESP_OK) {
        ESP_LOGW("screen_idle", "wake_error=%d", (int)error);
        return;
    }
    lv_obj_t *surface = wake_surface;
    wake_surface = NULL;
    /* Deleting the event target on release makes LVGL stop this gesture. It
     * cannot become a click or recording press on the screen underneath. */
    lv_obj_delete(surface);
    lv_display_trigger_activity(NULL);
    ESP_LOGI("screen_idle", "awake=1 brightness=%u", brightness);
}
static void wake_touch(lv_event_t *event)
{
    (void)event;
    if (wake_surface) wake_screen();
}

bool screen_idle_keyboard_activity(void)
{
    const bool was_asleep = wake_surface != NULL;
    if (was_asleep) wake_screen();
    lv_display_trigger_activity(NULL);
    return was_asleep;
}
static void check_idle(lv_timer_t *timer)
{
    (void)timer;
    bool busy = busy_callback && busy_callback();
    bool held = touch_held();
    unsigned seconds = screen_idle_get_timeout();
    static unsigned last_seconds = UINT_MAX;
    static bool last_busy, last_held;
    if (seconds != last_seconds || busy != last_busy || held != last_held) {
        ESP_LOGI("screen_idle", "gate_busy=%d held=%d timeout_s=%u", busy, held, seconds);
        last_seconds = seconds; last_busy = busy; last_held = held;
    }
    if (wake_surface) {
        if ((!seconds || busy) && !held) wake_screen();
        return;
    }
    if (busy || held || !seconds) {
        lv_display_trigger_activity(NULL);
        return;
    }
    if (lv_display_get_inactive_time(NULL) < seconds * 1000U) return;
    /* The top layer covers every page and intercepts every wake gesture. */
    wake_surface = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(wake_surface);
    lv_obj_set_size(wake_surface, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(wake_surface, 0, 0);
    lv_obj_set_style_bg_color(wake_surface, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wake_surface, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(wake_surface, false);
    lv_obj_set_clickable(wake_surface, true);
    lv_obj_add_event_cb(wake_surface, wake_touch, LV_EVENT_RELEASED, NULL);
    esp_err_t error = bsp_display_brightness_set(0);
    if (error != ESP_OK) {
        lv_obj_delete(wake_surface); wake_surface = NULL;
        lv_display_trigger_activity(NULL);
        ESP_LOGW("screen_idle", "sleep_error=%d", (int)error);
    } else ESP_LOGI("screen_idle", "asleep=1 timeout_s=%u", seconds);
}
void screen_idle_init(bool (*busy)(void), unsigned (*brightness)(void))
{
    busy_callback = busy; brightness_callback = brightness;
    lv_display_trigger_activity(NULL);
    lv_timer_create(check_idle, 100, NULL);
}
