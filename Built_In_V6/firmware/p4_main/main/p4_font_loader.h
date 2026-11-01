#pragma once
#include "lvgl.h"
/* Task context only; acquires/yields the display lock internally. */
lv_font_t *p4_font_load(const char *path);
typedef void (*p4_font_progress_fn)(unsigned percent, void *context);
lv_font_t *p4_font_load_with_progress(const char *path, p4_font_progress_fn progress,
                                    void *context);
lv_font_t *p4_binfont_create(const char *path);
lv_font_t *p4_binfont_create_ex(const lv_binfont_dsc_t *dsc);
lv_font_t *p4_binfont_create_from_buffer(void *buffer, uint32_t size);
void p4_binfont_destroy(lv_font_t *font);
