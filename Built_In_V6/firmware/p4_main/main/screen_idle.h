#pragma once
#include <stdbool.h>

bool screen_idle_timeout_valid(unsigned seconds);
void screen_idle_set_timeout(unsigned seconds);
unsigned screen_idle_get_timeout(void);
/* Display lock required. Consume a wake key rather than submit unseen input. */
bool screen_idle_keyboard_activity(void);
/* Initialize under the LVGL display lock. No hardware sleep or RTC teardown. */
void screen_idle_init(bool (*busy)(void), unsigned (*brightness)(void));
