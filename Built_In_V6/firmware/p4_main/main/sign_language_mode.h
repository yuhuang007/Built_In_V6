#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

typedef esp_err_t (*sign_language_prepare_fn)(void);
typedef void (*sign_language_resume_fn)(void);
bool sign_language_mode_active(void);
esp_err_t sign_language_mode_start(const lv_font_t *font,
                                   sign_language_prepare_fn prepare,
                                   sign_language_resume_fn resume);

/* UI requests the existing worker-owned release path before settings. */
void sign_language_mode_request_return(void);
