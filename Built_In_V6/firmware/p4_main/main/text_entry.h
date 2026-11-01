#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* All functions except finish run under the display lock. */
typedef bool (*text_entry_submit_fn)(const char *text, void *context);
typedef void (*text_entry_layout_fn)(int height, void *context);
void text_entry_mount(lv_obj_t *parent, const lv_font_t *font,
                      text_entry_submit_fn submit, void *context,
                      text_entry_layout_fn layout, void *layout_context,
                      int top, lv_obj_t *language_button);
void text_entry_toggle_language(lv_event_t *event);
void text_entry_focus(bool touch_keyboard);
/* Save the draft and release UI allocations before constructing another page. */
void text_entry_release(lv_obj_t *parent);
void text_entry_set_enabled(lv_obj_t *parent, bool enabled);
void text_entry_set_font(lv_obj_t *parent, const lv_font_t *font);
bool text_entry_is_root(lv_obj_t *object);
bool text_entry_active(void);
void text_entry_error(const char *message);
/* Worker-safe: no LVGL calls, and no caller-owned pointers are retained. */
void text_entry_finish(bool success);
void text_entry_key(uint32_t key);
