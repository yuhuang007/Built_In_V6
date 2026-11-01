#pragma once
#include "lvgl.h"

/* Borrowed bounded glyph cache shared by settings and device information.
 * Call under the display lock; the boot font source remains unchanged. */
const lv_font_t *settings_cached_font(const lv_font_t *source);

/* Public diagnostics only; callers never pass credentials or device identity. */
void device_about_show(lv_obj_t *return_screen, const lv_font_t *font,
                       bool wifi, bool cloud, unsigned brightness, unsigned volume,
                       unsigned font_size);
