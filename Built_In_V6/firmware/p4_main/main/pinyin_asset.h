#pragma once
#include "esp_err.h"
#include <stdbool.h>
#define PINYIN_ASSET_PATH "/sdcard/ime/P4PYV1.DAT"
esp_err_t pinyin_asset_ensure(bool allow_install);
