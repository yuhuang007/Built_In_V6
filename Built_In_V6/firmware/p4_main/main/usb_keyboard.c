#include "usb_keyboard.h"
#include "bsp/esp-bsp.h"
#include "esp_lvgl_port_usbhid.h"
#include "esp_log.h"

static void (*key_callback)(uint32_t);

static void keyboard_event(lv_event_t *event)
{
    lv_indev_t *input = lv_event_get_target(event);
    if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED && key_callback)
        key_callback(lv_indev_get_key(input));
}

esp_err_t usb_keyboard_start(lv_display_t *display, void (*on_key)(uint32_t))
{
    /* The pinned port logs unsupported key usages at INFO; suppress key data. */
    esp_log_level_set("LVGL", ESP_LOG_WARN);
    esp_err_t error = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (error != ESP_OK) return error;
    const lvgl_port_hid_keyboard_cfg_t config = {.disp = display};
    lv_indev_t *input = lvgl_port_add_usb_hid_keyboard_input(&config);
    if (!input) return ESP_ERR_NO_MEM;
    if (!bsp_display_lock(0)) return ESP_ERR_TIMEOUT;
    key_callback = on_key;
    lv_indev_add_event_cb(input, keyboard_event, LV_EVENT_KEY, NULL);
    bsp_display_unlock();
    ESP_LOGI("usb_keyboard", "ready=1");
    return ESP_OK;
}
