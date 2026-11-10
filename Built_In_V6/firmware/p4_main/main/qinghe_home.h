#ifndef QINGHE_HOME_H
#define QINGHE_HOME_H
#include "lvgl.h"
#include "qinghe_home_layout.h"

typedef struct {
    lv_obj_t *screen, *theme;
    lv_obj_t *objects[QHH_COUNT];
    int readiness_cache;
} qh_home_t;

void qh_home_create(qh_home_t *home, const lv_font_t *body, const lv_font_t *heading,
                    lv_event_cb_t voice, lv_event_cb_t transcript, lv_event_cb_t camera,
                    lv_event_cb_t settings, lv_event_cb_t help);
void qh_home_set_fonts(qh_home_t *home, const lv_font_t *body, const lv_font_t *heading);
void qh_home_set_readiness(qh_home_t *home, bool wifi, bool cloud, bool welcome_playing);
#endif
