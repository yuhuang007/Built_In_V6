#pragma once
#include <stdbool.h>
#include <stdint.h>
#if defined(CHAT_HOST_TEST) && defined(_WIN32)
#define SCREEN_POLICY_API __declspec(dllexport)
#else
#define SCREEN_POLICY_API
#endif
SCREEN_POLICY_API bool screen_idle_startup_can_arm(bool home_visible, bool welcome_finished,
    bool user_started, bool offline, uint32_t home_age_ms, uint32_t voice_quiet_ms);
SCREEN_POLICY_API bool screen_idle_welcome_pcm_finished(bool played_voice,
    bool queue_allows_idle, uint32_t voice_quiet_ms);
