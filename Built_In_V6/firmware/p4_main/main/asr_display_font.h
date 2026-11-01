#pragma once
#include "lvgl.h"

/* Create/destroy under the display lock; only used by the transcription page. */
lv_font_t *asr_display_font_create(const lv_font_t *source);
/* Native SD source, no enlargement or legacy supplemental glyph override. */
lv_font_t *asr_display_font_create_native(const lv_font_t *source);
void asr_display_font_destroy(lv_font_t *font);
