#pragma once
#include <stdbool.h>
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "lvgl.h"

typedef esp_err_t (*speech_to_text_prepare_fn)(void);
typedef void (*speech_to_text_resume_fn)(void);
bool speech_to_text_mode_active(void);
/* Display lock required; no-op while recognition or mode handoff is busy. */
void speech_to_text_mode_open_editor(void);
esp_err_t speech_to_text_mode_resources_init(void);
esp_err_t speech_to_text_mode_preload_fonts(void);
bool speech_to_text_mode_fonts_ready(void);
/* Borrowed, boot-lifetime font; NULL until both native fonts/cache are ready.
 * UI callers hold the display lock and must never destroy this shared font. */
const lv_font_t *speech_to_text_mode_font14(void);
const lv_font_t *speech_to_text_mode_font28(void);
const lv_font_t *speech_to_text_mode_cached_font28(void);
/* Display lock required when a page exists. 0 preserves the original 28px
 * transcription size; explicit choices are 14 or 28. No font ownership transfer. */
void speech_to_text_mode_set_subtitle_size(unsigned pixels);
typedef enum {
    SPEECH_FONTS_IDLE, SPEECH_FONTS_VERIFY, SPEECH_FONTS_SMALL,
    SPEECH_FONTS_LARGE, SPEECH_FONTS_READY, SPEECH_FONTS_FAILED
} speech_fonts_stage_t;
speech_fonts_stage_t speech_to_text_mode_fonts_stage(void);
unsigned speech_to_text_mode_fonts_percent(void);
esp_err_t speech_to_text_mode_enter(esp_codec_dev_handle_t microphone,
    const lv_font_t *font, speech_to_text_prepare_fn prepare,
    speech_to_text_resume_fn resume);
