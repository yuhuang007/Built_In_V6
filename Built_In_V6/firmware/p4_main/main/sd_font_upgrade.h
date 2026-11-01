#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef void (*sd_font_progress_fn)(unsigned percent, void *context);
typedef bool (*sd_font_cancel_fn)(void *context);
/* Only provisions missing V2 font assets; existing files are never replaced. */
esp_err_t sd_font_upgrade_ensure(sd_font_progress_fn progress,
                               sd_font_cancel_fn cancelled, void *context);
/* Boot preparation is read-only for SD: never downloads missing assets. */
esp_err_t sd_font_upgrade_verify_installed(void);
