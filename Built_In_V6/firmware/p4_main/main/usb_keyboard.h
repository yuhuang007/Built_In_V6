#pragma once
#include "esp_err.h"
#include "lvgl.h"

/* Callback runs under the LVGL display lock; never log received keystrokes. */
esp_err_t usb_keyboard_start(lv_display_t *display, void (*on_key)(uint32_t));
