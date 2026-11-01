#pragma once
#include "lvgl.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { CHAT_USER, CHAT_ASSISTANT, CHAT_UNKNOWN } chat_role_t;
/* All UI functions below run with the display lock held. */
esp_err_t chat_history_init(void);
void chat_history_create_ui(lv_obj_t *parent, const lv_font_t *font);
void chat_history_set_font(const lv_font_t *font);
void chat_history_set_view_height(int height);
/* Worker opens only the dedicated /sdcard/CHATLOG/H0.BIN and H1.BIN files. */
esp_err_t chat_history_start_storage(void);
uint32_t chat_history_begin_turn(void);
uint32_t chat_history_current_turn(void);
void chat_history_update(chat_role_t role, uint32_t turn, const char *text, int sequence);
void chat_history_request_clear(void);
unsigned chat_history_count(void);
const char *chat_history_storage_status(void);
bool chat_history_loading(void);
