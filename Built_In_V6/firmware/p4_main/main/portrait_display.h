#pragma once

#include "driver/ppa.h"
#include "lvgl.h"
/* Numeric health marker only; safe for the existing independent UI observer. */
unsigned portrait_flush_stage(void);

/* Call with the BSP display lock held and pending LVGL updates flushed. */
esp_err_t portrait_preview_frame(const uint16_t *source, int width, int height,
                                 const lv_area_t *logical,
                                 ppa_srm_rotation_angle_t camera_rotation);
void portrait_preview_stop(void);
/* UI task/display lock only; does not change the camera's accelerated path. */
void portrait_editor_cpu_copy(bool enabled);
void portrait_editor_cpu_region(int logical_top);

/* Low-rate timing only: scene 1 chat, 2 settings; no sampling/behavior changes. */
void portrait_observe_touch(lv_indev_t *indev);
void portrait_observe_screen(lv_obj_t *screen,unsigned scene);
