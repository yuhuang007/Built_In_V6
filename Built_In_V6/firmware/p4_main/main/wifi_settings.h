#pragma once
#include "esp_wifi.h"
#include "lvgl.h"

/* Credentials remain on-device. No caller logs the station configuration. */
void wifi_settings_load(wifi_config_t *fallback);
esp_err_t wifi_settings_start(void);
void wifi_settings_event(int32_t event_id, bool got_ip);
void wifi_settings_show(lv_obj_t *parent, const lv_font_t *font);
void wifi_settings_set_font(const lv_font_t *font);
