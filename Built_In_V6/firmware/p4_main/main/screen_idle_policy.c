#include "screen_idle_policy.h"
bool screen_idle_welcome_pcm_finished(bool played_voice,
    bool queue_allows_idle, uint32_t voice_quiet_ms)
{
    /* RTC welcome PCM need not be accompanied by conversation-state events.
     * Require actual playback, then a conservative gap exceeding the entire
     * bounded PCM queue (24 x 64ms), before releasing the startup gate. */
    return played_voice && queue_allows_idle && voice_quiet_ms >= 5000U;
}
bool screen_idle_startup_can_arm(bool home_visible, bool welcome_finished,
    bool user_started, bool offline, uint32_t home_age_ms, uint32_t voice_quiet_ms)
{
    if (!home_visible) return false;
    /* A user-started turn deliberately supersedes the welcome. Offline boots
     * cannot receive one; give initial network preparation 30s before arming. */
    if (user_started || (offline && home_age_ms >= 30000U)) return true;
    /* ANSWER_FINISH can precede playback of queued PCM. Let its audible tail
     * drain before starting a fresh, full user-selected idle interval. */
    return welcome_finished && voice_quiet_ms >= 1500U;
}
