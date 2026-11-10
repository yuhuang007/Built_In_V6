#pragma once
#include "lvgl.h"

#define QH_BG 0xF5F6ED
#define QH_SURFACE 0xFFFEF8
#define QH_SOFT 0xE2E9D4
#define QH_INK 0x314633
#define QH_MUTED 0x596A50
#define QH_ACCENT 0x3E5947
#define QH_LINE 0xCFD8C3
#define QH_DANGER 0x9E5148
#define QH_WARN 0x82683C
#define QH_LINK 0x586E4E
#define QH_ERROR 0x8C3C35
#define QH_ON_ACCENT 0xFAFFF6
#define QH_USER 0xDBE6CC
#define QH_ASSISTANT 0xFFFDF4

/* Canonical semantic colors remain stable; resolve at creation/draw time. */
lv_color_t qh_color(uint32_t semantic);
bool qh_theme_dark(void);
void qh_theme_restore(bool dark);
void qh_theme_set_save_callback(void (*callback)(void));
void qh_theme_apply(lv_obj_t *root);
void qh_theme_toggle(lv_event_t *event);
void qh_theme_native(void);
lv_obj_t *qh_theme_button(lv_obj_t *parent);
lv_obj_t *qh_back_button(lv_obj_t *parent, lv_event_cb_t callback);
lv_obj_t *qh_header(lv_obj_t *parent, const char *title, const lv_font_t *font,
                    lv_event_cb_t back, bool settings);
/* 56px hit areas, common 12px top, 12px gutters. IME occupies x=388. */
#define QH_TOOL_SIZE 56
#define QH_TOOL_Y 12
#define QH_LANGUAGE_X 388
enum { QH_ICON_BACK, QH_ICON_THEME, QH_ICON_ROBOT, QH_ICON_USER,
       QH_ICON_DOCUMENT, QH_ICON_KEYBOARD, QH_ICON_CAMERA, QH_ICON_WAKE };
lv_obj_t *qh_icon(lv_obj_t *parent, unsigned kind);

void qh_screen(lv_obj_t *screen);
void qh_panel(lv_obj_t *object, uint32_t color, int radius);
void qh_button(lv_obj_t *object, uint32_t color);
lv_obj_t *qh_label(lv_obj_t *parent, const lv_font_t *font, const char *text, uint32_t color);
lv_obj_t *qh_logo(lv_obj_t *parent);
lv_obj_t *qh_startup_leaf(lv_obj_t *parent);
void qh_startup_leaf_step(lv_obj_t *object, uint32_t elapsed_ms);
/* Shared settings entry; callers retain their own business/lifecycle guards. */
void qh_set_settings_callback(lv_event_cb_t callback);
lv_obj_t *qh_settings_button(lv_obj_t *parent);
void qh_screen_load(lv_obj_t *screen, bool back);
void qh_screen_load_camera(lv_obj_t *screen, bool back);
bool qh_navigation_busy(void);
void qh_reveal(lv_obj_t *object);
