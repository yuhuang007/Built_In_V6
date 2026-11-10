#include "speech_to_text_mode.h"
#include "ui_layout.h"
#include "qinghe_ui.h"
#include "text_entry.h"
#include "transcript_turns.h"
#include "doubao_asr.h"
#include "asr_display_font.h"
#include "sd_font_upgrade.h"
#include "sd_font_assets.h"
#include "p4_font_loader.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define PRESS_BIT BIT0
#define RETURN_BIT BIT1
#define START_BIT BIT2
#define PCM_FRAME_BYTES 640 /* 20 ms at 16 kHz / s16le / mono */
#define PCM_PACKET_BYTES 6400
#define PCM_LIMIT_BYTES (16000 * 2 * 60) /* Bound each press to 60 seconds. */
static const char *TAG = "speech_text";
static atomic_bool active;
static atomic_bool busy;
static atomic_bool release_requested;
static atomic_bool return_requested;
static lv_obj_t *home_screen;
static lv_obj_t *text_screen;
static lv_obj_t *status_label;
static lv_obj_t *empty_state, *empty_guide, *transcript_heading;
static lv_obj_t *transcript_label;
static lv_obj_t *transcript_content;
static lv_obj_t *transcript_viewport, *hold_control, *clear_control;
static lv_obj_t *latest_button, *latest_label, *clear_dialog;
static bool follow_latest, updating_transcript_view;
static char *transcript_storage;
static transcript_turns_t transcript;
#define TRANSCRIPT_CAPACITY 32768
static lv_font_t *transcript_font;
static bool transcript_font_owned;
static unsigned subtitle_size;
static lv_font_t *native_font_14;
static lv_font_t *native_font_28;
static lv_font_t *cached_native_font;
static SemaphoreHandle_t resources_lock;
static atomic_bool fonts_ready;
static atomic_int fonts_stage;
static atomic_uint font_parse_percent;
static lv_obj_t *control_labels[4];
#define TRANSCRIPT_WIDTH UI_CONTENT_WIDTH
#define TRANSCRIPT_HEIGHT UI_TRANSCRIPT_HEIGHT
static EventGroupHandle_t events;
static esp_codec_dev_handle_t microphone;
static speech_to_text_prepare_fn prepare_voice;
static speech_to_text_resume_fn resume_voice;

typedef struct {
    uint8_t *pcm;
    atomic_size_t captured;
    atomic_bool done;
    atomic_int error;
    bool received_text;
    bool text_full;
} recording_t;

bool speech_to_text_mode_active(void)
{
    return atomic_load(&active);
}

static void set_status(const char *text)
{
    if (bsp_display_lock(0)) {
        lv_label_set_text(status_label, text);
        const bool error=strstr(text,"失败") || strstr(text,"不足") || strstr(text,"超时");
        lv_obj_set_style_text_color(status_label,qh_color(error?QH_ERROR:QH_INK),0);
        bsp_display_unlock();
    }
}

static void update_latest_button(void)
{
    if (latest_button)
        lv_obj_set_hidden(latest_button, follow_latest ||
                          lv_obj_get_scroll_bottom(transcript_viewport) < 32);
}

static void transcript_scroll_event(lv_event_t *event)
{
    (void)event;
    if (updating_transcript_view) return;
    /* Match chat history's near-bottom tolerance, but do not mistake a
     * hypothesis shrinking under the reader for an intentional return. */
    follow_latest = lv_obj_get_scroll_bottom(transcript_viewport) < 32;
    update_latest_button();
}

/* Caller holds the display lock. Preserve the top offset when an in-progress
 * hypothesis grows or shrinks; LVGL bounds it if that offset no longer exists. */
static void update_transcript_view(const char *text)
{
    const int32_t scroll_y = lv_obj_get_scroll_y(transcript_viewport);
    updating_transcript_view = true;
    if (text) lv_label_set_text(transcript_label, text);
    if (empty_state) lv_obj_set_hidden(empty_state,lv_label_get_text(transcript_label)[0]!=0);
    if (latest_label) {
        lv_obj_set_style_text_font(latest_label, transcript_font, 0);
        lv_obj_set_height(latest_button, transcript_font->line_height + 16);
    }
    lv_obj_update_layout(transcript_label);
    const int32_t height = lv_obj_get_height(transcript_label);
    lv_obj_set_height(transcript_content,
                      height > lv_obj_get_height(transcript_viewport) ? height : lv_obj_get_height(transcript_viewport));
    lv_obj_update_layout(transcript_viewport);
    if (follow_latest || lv_obj_get_scroll_y(transcript_viewport) != scroll_y)
        lv_obj_scroll_to_y(transcript_viewport, follow_latest ? LV_COORD_MAX : scroll_y, LV_ANIM_OFF);
    updating_transcript_view = false;
    update_latest_button();
}

static void resize_transcript(void)
{
    update_transcript_view(NULL);
}

static void set_transcript(const char *text)
{
    update_transcript_view(text);
}

static void latest_event(lv_event_t *event)
{
    (void)event;
    follow_latest = true;
    resize_transcript();
}

void speech_to_text_mode_set_subtitle_size(unsigned pixels)
{
    if (pixels != 0 && pixels != 14 && pixels != 28) return;
    subtitle_size = pixels;
    if (!transcript_label || !atomic_load(&fonts_ready)) return;
    /* Transcript stays at native 28px; the global choice affects controls. */
    lv_font_t *selected = cached_native_font;
    lv_obj_set_style_text_font(transcript_label, selected, 0);
    if(empty_guide)lv_obj_set_style_text_font(empty_guide,selected,0);
    if(transcript_heading)lv_obj_set_style_text_font(transcript_heading,selected,0);
    const lv_font_t *controls = pixels == 28 ? native_font_28 : native_font_14;
    if (status_label) lv_obj_set_style_text_font(status_label, controls, 0);
    for (unsigned i = 0; i < 4; ++i)
        if (control_labels[i]) lv_obj_set_style_text_font(control_labels[i], controls, 0);
    if (transcript_font_owned) asr_display_font_destroy(transcript_font);
    transcript_font = selected;
    transcript_font_owned = false;
    resize_transcript();
}

bool speech_to_text_mode_can_settings(void)
{
    return atomic_load(&active) && !atomic_load(&busy) && !atomic_load(&return_requested) && !clear_dialog;
}

/* Called with the display lock, after fonts are no longer referenced by UI. */
static void release_mode_fonts(void)
{
    /* Native indices and bounded glyph cache are shared boot resources. */
    if (transcript_font_owned) asr_display_font_destroy(transcript_font);
    transcript_font = NULL;
    transcript_font_owned = false;
    heap_caps_free(transcript_storage);
    transcript_storage = NULL;
    transcript_viewport = hold_control = empty_state = empty_guide = transcript_heading = NULL;
    latest_button = latest_label = clear_dialog = NULL;
    clear_control = NULL;
    for (unsigned i=0;i<4;i++) control_labels[i] = NULL;
}

esp_err_t speech_to_text_mode_resources_init(void)
{
    if (!resources_lock) resources_lock = xSemaphoreCreateMutex();
    return resources_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

bool speech_to_text_mode_fonts_ready(void)
{
    return atomic_load(&fonts_ready);
}

const lv_font_t *speech_to_text_mode_font14(void)
{
    return atomic_load(&fonts_ready) ? native_font_14 : NULL;
}

const lv_font_t *speech_to_text_mode_font28(void)
{
    return atomic_load(&fonts_ready) ? native_font_28 : NULL;
}

const lv_font_t *speech_to_text_mode_cached_font28(void)
{
    return atomic_load(&fonts_ready) ? cached_native_font : NULL;
}

speech_fonts_stage_t speech_to_text_mode_fonts_stage(void)
{
    return atomic_load(&fonts_stage);
}

unsigned speech_to_text_mode_fonts_percent(void)
{
    return atomic_load(&font_parse_percent);
}

static void font_parse_progress(unsigned percent, void *context)
{
    (void)context;
    atomic_store(&font_parse_percent, percent);
}

/* resources_lock serializes startup and page entry. Only fully built fonts
 * become visible; the cooperative loader never publishes a partial index. */
static bool prepare_native_fonts(void)
{
    if (atomic_load(&fonts_ready)) return true;
    atomic_store(&font_parse_percent, 0);
    atomic_store(&fonts_stage, SPEECH_FONTS_SMALL);
    lv_font_t *small = p4_font_load_with_progress("A:fonts/" SD_FONT_14_NAME, font_parse_progress, NULL);
    if (small) {
        atomic_store(&font_parse_percent, 0);
        atomic_store(&fonts_stage, SPEECH_FONTS_LARGE);
    }
    lv_font_t *large = small ? p4_font_load_with_progress("A:fonts/" SD_FONT_28_NAME,
                                                        font_parse_progress, NULL) : NULL;
    if (!bsp_display_lock(0)) return false;
    lv_font_t *cached = large ? asr_display_font_create_native(large) : NULL;
    const bool success = small && large && cached;
    if (success) {
        native_font_14 = small;
        native_font_28 = large;
        cached_native_font = cached;
        atomic_store(&fonts_ready, true);
        atomic_store(&fonts_stage, SPEECH_FONTS_READY);
    } else {
        asr_display_font_destroy(cached);
        p4_binfont_destroy(small);
        p4_binfont_destroy(large);
        atomic_store(&fonts_stage, SPEECH_FONTS_FAILED);
    }
    ESP_LOGI(TAG, "native_fonts small=%d large=%d cached=%d", small != NULL,
             large != NULL, cached != NULL);
    bsp_display_unlock();
    return success;
}

esp_err_t speech_to_text_mode_preload_fonts(void)
{
    if (!resources_lock || xSemaphoreTake(resources_lock, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_OK;
    if (!atomic_load(&fonts_ready)) {
        atomic_store(&fonts_stage, SPEECH_FONTS_VERIFY);
        error = sd_font_upgrade_verify_installed();
        if (error == ESP_OK && !prepare_native_fonts()) error = ESP_FAIL;
        if (error != ESP_OK) atomic_store(&fonts_stage, SPEECH_FONTS_FAILED);
    }
    xSemaphoreGive(resources_lock);
    return error;
}

static bool bind_native_mode_fonts(void)
{
    if (!bsp_display_lock(0)) return false;
    lv_font_t *cached = cached_native_font;
    if (cached) {
        lv_obj_set_style_text_font(transcript_label, cached, 0);
        if(empty_guide)lv_obj_set_style_text_font(empty_guide,cached,0);
        if(transcript_heading)lv_obj_set_style_text_font(transcript_heading,cached,0);
        if (transcript_font_owned) asr_display_font_destroy(transcript_font);
        transcript_font = cached;
        transcript_font_owned = false;
        resize_transcript();
        text_entry_set_font(text_screen, cached);
    }
    if (native_font_14) {
        const lv_font_t *controls = subtitle_size == 28 ? native_font_28 : native_font_14;
        lv_obj_set_style_text_font(status_label, controls, 0);
        for (unsigned i = 0; i < 4; ++i)
            if (control_labels[i]) lv_obj_set_style_text_font(control_labels[i], controls, 0);
    }
    const bool success = native_font_14 != NULL && cached != NULL;
    bsp_display_unlock();
    return success;
}

static esp_err_t prepare_page_fonts(void)
{
    const uint32_t started = esp_log_timestamp();
    const bool reused = atomic_load(&fonts_ready);
    esp_err_t error = ESP_OK;
    /* Fully published boot resources are immutable for the boot lifetime.
     * Page binding needs no installation, hash pass, or resource mutex. */
    if (!reused) {
        set_status("正在准备转写字库，请稍候");
        bool acquired = false;
        while (!atomic_load(&return_requested) && resources_lock && !acquired &&
               esp_log_timestamp()-started < 1000)
            acquired = xSemaphoreTake(resources_lock, pdMS_TO_TICKS(20)) == pdTRUE;
        if (!acquired) error = ESP_ERR_TIMEOUT;
        else {
            /* Assets missing at boot must be repaired explicitly; entry never
             * starts a private-LAN download or an unbounded install attempt. */
            error = atomic_load(&fonts_ready) ? ESP_OK : sd_font_upgrade_verify_installed();
            if (error == ESP_OK && !atomic_load(&return_requested) && !prepare_native_fonts()) error = ESP_FAIL;
            xSemaphoreGive(resources_lock);
        }
    }
    if (atomic_load(&return_requested)) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && !bind_native_mode_fonts()) error = ESP_FAIL;
    ESP_LOGI(TAG, "page_fonts reused=%d ready=%d stage=%d error=%d duration_ms=%u",
             reused, atomic_load(&fonts_ready), atomic_load(&fonts_stage), (int)error,
             (unsigned)(esp_log_timestamp()-started));
    return error;
}

static void on_text(const char *text, void *arg)
{
    recording_t *recording = arg;
    if (atomic_load(&return_requested)) return;
    recording->received_text = true;
    if (bsp_display_lock(0)) {
        /* Replace only this recording's hypothesis, preserving earlier turns. */
        if (transcript_turns_update(&transcript, text)) set_transcript(transcript.text);
        else {
            recording->text_full = true;
            lv_label_set_text(status_label, "文字已满，请清空后继续；已有内容已保留");
        }
        bsp_display_unlock();
    }
}

static void capture_task(void *arg)
{
    recording_t *recording = arg;
    esp_codec_dev_sample_info_t format = {
        .sample_rate = 16000, .channel = 1, .bits_per_sample = 16,
    };
    esp_err_t error = esp_codec_dev_open(microphone, &format);
    const bool opened = error == ESP_OK;
    size_t bytes = 0;
    while (error == ESP_OK && !atomic_load(&release_requested) && bytes < PCM_LIMIT_BYTES) {
        error = esp_codec_dev_read(microphone, recording->pcm + bytes, PCM_FRAME_BYTES);
        if (error != ESP_OK) break;
        bytes += PCM_FRAME_BYTES;
        /* Release publishes only complete frames to the network worker. */
        atomic_store(&recording->captured, bytes);
    }
    if (opened) {
        const esp_err_t close_error = esp_codec_dev_close(microphone);
        if (close_error != ESP_OK) error = close_error;
    }
    atomic_store(&recording->error, error);
    ESP_LOGI(TAG, "capture_done bytes=%u duration_ms=%u limit=%d error=%d",
        (unsigned)bytes, (unsigned)(bytes / 32), bytes == PCM_LIMIT_BYTES, (int)error);
    atomic_store(&recording->done, true);
    /* No further access to recording after publishing done. */
    vTaskDelete(NULL);
}

static void recognize_press(void)
{
    recording_t recording = {0};
    recording.pcm = heap_caps_malloc(PCM_LIMIT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recording.pcm) {
        set_status("录音内存不足，请返回后重试");
        return;
    }
    /* Capture begins before TLS so words spoken during connection setup are
     * retained in bounded PSRAM, not silently discarded. No audio file exists. */
    if (xTaskCreate(capture_task, "asr_capture", 4096, &recording, 5, NULL) != pdPASS) {
        heap_caps_free(recording.pcm);
        set_status("录音任务启动失败，请重试");
        return;
    }
    doubao_asr_client_t *client = NULL;
    esp_err_t error = ESP_OK;
    if (!atomic_load(&return_requested)) error = doubao_asr_open(&client, on_text, &recording);
    size_t sent = 0;
    bool last_sent = false;
    bool final_received = false;
    int64_t final_deadline = 0;
    while (error == ESP_OK && !atomic_load(&return_requested) && client && !final_received) {
        /* Acquire done first: if it is true, the following count must include
         * the capture task's last frame, not a stale pre-release snapshot. */
        const bool done = atomic_load(&recording.done);
        const size_t captured = atomic_load(&recording.captured);
        if (done && atomic_load(&recording.error) != ESP_OK) {
            error = atomic_load(&recording.error);
            break;
        }
        if (done && captured == 0) { error = ESP_ERR_INVALID_SIZE; break; }
        if (!last_sent && (captured - sent >= PCM_PACKET_BYTES || done)) {
            const size_t amount = captured - sent > PCM_PACKET_BYTES ? PCM_PACKET_BYTES : captured - sent;
            last_sent = done && sent + amount == captured;
            error = doubao_asr_send(client, recording.pcm + sent, amount, last_sent);
            if (error != ESP_OK) break;
            sent += amount;
            if (last_sent) {
                set_status("录音已结束，正在确认最终文字...");
                final_deadline = esp_timer_get_time() + 10000000LL;
                ESP_LOGI(TAG, "last_audio_sent bytes=%u", (unsigned)sent);
            }
        }
        bool last = false;
        error = doubao_asr_receive(client, 10, &last);
        if (error == ESP_ERR_TIMEOUT) error = ESP_OK;
        if (error != ESP_OK) break;
        final_received = last;
        if (last_sent && esp_timer_get_time() >= final_deadline && !final_received) {
            error = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    atomic_store(&release_requested, true);
    while (!atomic_load(&recording.done)) vTaskDelay(pdMS_TO_TICKS(20));
    doubao_asr_close(client);
    if (bsp_display_lock(0)) {
        transcript_turns_commit(&transcript);
        bsp_display_unlock();
    }
    /* Microphone is closed and ASR transport gone before a new press or exit. */
    heap_caps_free(recording.pcm);
    if (!atomic_load(&return_requested)) {
        if (recording.text_full) {
            set_status("文字已满，请清空后继续；已有内容已保留");
        } else if (error == ESP_OK && final_received) {
            set_status(recording.received_text ? "转写完成；可按住继续说话" :
                                                "未识别到语音，请按住后重试");
        } else {
            set_status("识别失败或超时，请检查网络和额度后重试");
        }
    }
    ESP_LOGI(TAG, "request_done bytes=%u final=%d text_received=%d error=%d",
        (unsigned)sent, final_received, recording.received_text, (int)error);
}

static void mode_task(void *arg)
{
    (void)arg;
    /* Reserve the internal stack before page widgets fragment the heap. */
    xEventGroupWaitBits(events, START_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
    const esp_err_t prepared = prepare_voice();
    const esp_err_t font_error = atomic_load(&return_requested) ? ESP_ERR_INVALID_STATE : prepare_page_fonts();
    set_status(font_error != ESP_OK ? "转写字库准备失败，请返回后重试" :
               prepared != ESP_OK ? "可输入文字；录音需网络和音频就绪" :
               "按住下方按钮说话，松开后保留文字（最长60秒）");
    atomic_store(&busy, false);
    ESP_LOGI(TAG, "mode_ready=1 prepared_error=%d", (int)prepared);
    if (bsp_display_lock(0)) {
        text_entry_set_enabled(text_screen, !clear_dialog);
        bsp_display_unlock();
    }
    while (!atomic_load(&return_requested)) {
        const EventBits_t bits = xEventGroupWaitBits(events, PRESS_BIT | RETURN_BIT,
                                                    pdTRUE, pdFALSE, portMAX_DELAY);
        if ((bits & RETURN_BIT) || atomic_load(&return_requested)) break;
        if (bits & PRESS_BIT) {
            const esp_err_t ready = prepare_voice();
            ESP_LOGI(TAG, "record_prepare error=%d", (int)ready);
            if (ready == ESP_OK && !atomic_load(&return_requested)) recognize_press();
            else set_status("网络或音频未就绪；仍可输入文字，请稍后重试录音");
        }
        atomic_store(&busy, false);
        if (bsp_display_lock(0)) {
            text_entry_set_enabled(text_screen, !atomic_load(&return_requested) && !clear_dialog);
            bsp_display_unlock();
        }
    }
    if (bsp_display_lock(0)) {
        qh_screen_load(home_screen, true);
        lv_obj_delete(text_screen);
        release_mode_fonts();
        text_screen = transcript_label = transcript_content = status_label = empty_state = NULL;
        vEventGroupDelete(events);
        events = NULL;
        atomic_store(&active, false);
        resume_voice();
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "mode exited; voice home restored");
    vTaskDelete(NULL);
}

static void hold_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        atomic_store(&release_requested, true);
        if (atomic_load(&busy)) lv_label_set_text(status_label, "录音结束，等待最终文字...");
    } else if (code == LV_EVENT_PRESSED && !atomic_load(&return_requested) && !clear_dialog) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&busy, &expected, true)) {
            atomic_store(&release_requested, false);
            transcript_turns_begin(&transcript);
            text_entry_set_enabled(text_screen, false);
            lv_label_set_text(status_label, "正在录音并连接识别服务...");
            xEventGroupSetBits(events, PRESS_BIT);
        }
    }
}

static void return_event(lv_event_t *event)
{
    (void)event;
    atomic_store(&return_requested, true);
    atomic_store(&release_requested, true);
    text_entry_set_enabled(text_screen, false);
    lv_label_set_text(status_label, "正在停止识别并返回...");
    xEventGroupSetBits(events, RETURN_BIT);
}

static bool accept_edited_text(const char *text, void *context)
{
    (void)context;
    if (atomic_load(&busy) || atomic_load(&return_requested) || clear_dialog) {
        text_entry_error("正在识别，请稍候");
        return false;
    }
    if (!transcript_turns_append(&transcript, text)) {
        text_entry_error("文字已满，请先清空记录");
        return false;
    }
    set_transcript(transcript.text);
    lv_label_set_text(status_label, "文字已添加");
    ESP_LOGI(TAG, "local_message_appended=1");
    text_entry_finish(true);
    return true;
}

void speech_to_text_mode_open_editor(void)
{
    if (!atomic_load(&active) || atomic_load(&busy) || atomic_load(&return_requested)) return;
    text_entry_focus(false);
}

static void input_layout(int height, void *context)
{
    (void)context;
    const int limit = UI_HEIGHT - UI_MARGIN - height;
    const int top = UI_ASR_INPUT_TOP < limit ? UI_ASR_INPUT_TOP : limit;
    const int bottom = top - 158;
    if (hold_control) lv_obj_set_y(hold_control, top - 82);
    if (clear_control) lv_obj_set_y(clear_control, top - 82);
    if (latest_button) lv_obj_set_y(latest_button, top - 146);
    if (transcript_viewport) {
        lv_obj_set_height(transcript_viewport, bottom - 152);
        resize_transcript();
    }
}

static void clear_cancel(lv_event_t *event)
{
    (void)event;
    if (clear_dialog) lv_obj_delete(clear_dialog);
    clear_dialog = NULL;
    text_entry_set_enabled(text_screen, !atomic_load(&busy) && !atomic_load(&return_requested));
}

static void clear_confirm(lv_event_t *event)
{
    if (clear_dialog && !atomic_load(&busy) && !atomic_load(&return_requested)) {
        transcript_turns_init(&transcript, transcript_storage, TRANSCRIPT_CAPACITY);
        follow_latest = true;
        set_transcript("");
        lv_label_set_text(status_label, "文字已清空");
    }
    clear_cancel(event);
}

static void clear_event(lv_event_t *event)
{
    (void)event;
    if (clear_dialog || atomic_load(&busy) || atomic_load(&return_requested)) return;
    /* Same blue card and cancel/confirm colors as settings. The backdrop also
     * prevents a touch from reaching recording or editing behind the dialog. */
    clear_dialog = lv_obj_create(text_screen);
    lv_obj_remove_style_all(clear_dialog);
    lv_obj_set_size(clear_dialog, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_scrollable(clear_dialog, false);
    lv_obj_set_style_bg_color(clear_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(clear_dialog, LV_OPA_50, 0);
    text_entry_set_enabled(text_screen, false);
    const lv_font_t *font = transcript_font;
    const char *message = "清空全部转写文字？\n清空后无法恢复。";
    lv_point_t size;
    lv_text_get_size(&size, message, font, 0, 4, 480, LV_TEXT_FLAG_NONE);
    const int action_height = font->line_height + 16;
    lv_obj_t *card = lv_obj_create(clear_dialog);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 540, size.y + action_height + 80);
    lv_obj_center(card);
    qh_reveal(card);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_style_bg_color(card, qh_color(QH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_t *prompt = lv_label_create(card);
    lv_obj_set_style_text_font(prompt, font, 0);
    lv_obj_set_style_text_color(prompt, qh_color(QH_INK), 0);
    lv_obj_set_style_text_line_space(prompt, 4, 0);
    lv_obj_set_width(prompt, 480);
    lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
    lv_label_set_text(prompt, message);
    lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 20);
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t color = i == 0 ? QH_SOFT : QH_DANGER;
        lv_obj_t *action = lv_button_create(card);
        lv_obj_set_size(action, 160, action_height);
        lv_obj_set_style_pad_all(action, 0, 0);
        lv_obj_set_style_bg_color(action, qh_color(color), 0);
        lv_obj_set_style_bg_color(action, lv_color_darken(qh_color(color), LV_OPA_20),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_align(action, LV_ALIGN_BOTTOM_MID, i == 0 ? -100 : 100, -20);
        lv_obj_t *label = lv_label_create(action);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, qh_color(i == 0 ? QH_INK : QH_ON_ACCENT), 0);
        lv_label_set_text(label, i == 0 ? "取消" : "确认清空");
        lv_obj_center(label);
        lv_obj_add_event_cb(action, i == 0 ? clear_cancel : clear_confirm, LV_EVENT_CLICKED, NULL);
    }
}

static lv_obj_t *button(lv_obj_t *screen, const lv_font_t *font, const char *text,
                         int width, int x, uint32_t color, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(screen);
    qh_button(object, color);
    lv_obj_set_style_bg_color(object, lv_color_darken(qh_color(color), LV_OPA_20),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_size(object, width, 64);
    lv_obj_align(object, LV_ALIGN_BOTTOM_MID, x, -18);
    lv_obj_t *label = lv_label_create(object);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, qh_color(color == QH_ACCENT ? QH_ON_ACCENT : QH_INK), LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (x < 0) control_labels[0] = label;
    else if (x > 0) control_labels[1] = label;
    else control_labels[2] = label;
    if (callback) lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, NULL);
    return object;
}

static esp_err_t entry_failed(unsigned stage, esp_err_t error)
{
    ESP_LOGW(TAG, "entry_failed stage=%u error=%d internal=%u largest=%u", stage, (int)error,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return error;
}

esp_err_t speech_to_text_mode_enter(esp_codec_dev_handle_t mic,
    const lv_font_t *font, speech_to_text_prepare_fn prepare,
    speech_to_text_resume_fn resume)
{
    if (atomic_load(&active) || !mic || !font || !prepare || !resume)
        return entry_failed(1, ESP_ERR_INVALID_STATE);
    /* Avoid retaining the home dock while allocating a second page and task. */
    text_entry_release(lv_screen_active());
    events = xEventGroupCreate();
    if (!events) return entry_failed(2, ESP_ERR_NO_MEM);
    const bool ready = atomic_load(&fonts_ready);
    transcript_font = ready ? cached_native_font :
                             asr_display_font_create(font);
    transcript_font_owned = !ready && transcript_font != NULL;
    if (!transcript_font) {
        vEventGroupDelete(events); events = NULL;
        return entry_failed(3, ESP_ERR_NO_MEM);
    }
    transcript_storage = heap_caps_malloc(TRANSCRIPT_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!transcript_storage) {
        release_mode_fonts();
        vEventGroupDelete(events); events = NULL;
        return entry_failed(4, ESP_ERR_NO_MEM);
    }
    transcript_turns_init(&transcript, transcript_storage, TRANSCRIPT_CAPACITY);
    if (xTaskCreate(mode_task, "speech_text", 12288, NULL, 4, NULL) != pdPASS) {
        release_mode_fonts();
        vEventGroupDelete(events); events = NULL;
        return entry_failed(5, ESP_ERR_NO_MEM);
    }
    microphone = mic; prepare_voice = prepare; resume_voice = resume;
    atomic_store(&active, true); atomic_store(&busy, true);
    atomic_store(&release_requested, true); atomic_store(&return_requested, false);
    home_screen = lv_screen_active();
    text_screen = lv_obj_create(NULL);
    qh_screen(text_screen);
    lv_obj_set_style_bg_color(text_screen, qh_color(QH_BG), 0);
    status_label = lv_label_create(text_screen);
    lv_obj_set_style_text_font(status_label, font, 0);
    lv_obj_set_style_text_color(status_label, qh_color(QH_MUTED), 0);
    lv_label_set_text(status_label, "正在切换到语音转文字...");
    lv_obj_set_width(status_label, UI_CONTENT_WIDTH);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_height(status_label, 54);
    transcript_heading=qh_header(text_screen,"独立转写",transcript_font,return_event,true);
    lv_obj_t *viewport = lv_obj_create(text_screen);
    transcript_viewport = viewport;
    lv_obj_remove_style_all(viewport);
    lv_obj_set_size(viewport, TRANSCRIPT_WIDTH, UI_TRANSCRIPT_HEIGHT);
    lv_obj_align(viewport, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
    lv_obj_set_scroll_elastic(viewport, false);
    lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(viewport, transcript_scroll_event, LV_EVENT_SCROLL, NULL);
    follow_latest = true;
    transcript_content = lv_obj_create(viewport);
    lv_obj_remove_style_all(transcript_content);
    lv_obj_set_scrollable(transcript_content, false);
    lv_obj_set_size(transcript_content, TRANSCRIPT_WIDTH, TRANSCRIPT_HEIGHT);
    transcript_label = lv_label_create(transcript_content);
    lv_obj_set_style_text_font(transcript_label, transcript_font, 0);
    lv_obj_set_style_text_color(transcript_label, qh_color(QH_INK), 0);
    lv_obj_set_style_text_line_space(transcript_label, 4, 0);
    lv_obj_set_size(transcript_label, TRANSCRIPT_WIDTH, LV_SIZE_CONTENT);
    lv_label_set_long_mode(transcript_label, LV_LABEL_LONG_WRAP);
    set_transcript("");
    empty_state=lv_obj_create(transcript_content);lv_obj_remove_style_all(empty_state);
    lv_obj_set_size(empty_state,TRANSCRIPT_WIDTH,230);lv_obj_set_pos(empty_state,0,110);
    lv_obj_set_scrollable(empty_state,false);lv_obj_set_clickable(empty_state,false);
    lv_obj_t *mark=qh_icon(empty_state,QH_ICON_DOCUMENT);lv_obj_align(mark,LV_ALIGN_TOP_MID,0,12);
    lv_obj_t *guide=qh_label(empty_state,transcript_font,"按住说话，松开留字\n也可以点击键盘输入",QH_MUTED);
    empty_guide=guide;
    lv_obj_set_width(guide,TRANSCRIPT_WIDTH);lv_label_set_long_mode(guide,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(guide,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_pos(guide,0,68);
    /* Use the header's empty space, outside transcript and IME geometry. */
    latest_button = lv_button_create(text_screen);
    lv_obj_set_size(latest_button, 180, transcript_font->line_height + 10);
    lv_obj_set_align(latest_button, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(latest_button, UI_WIDTH - UI_MARGIN - 180, UI_ASR_INPUT_TOP - 146);
    lv_obj_set_style_pad_all(latest_button, 0, 0);
    qh_button(latest_button, QH_SOFT);
    latest_label = lv_label_create(latest_button);
    lv_obj_set_style_text_font(latest_label, transcript_font, 0);
    lv_obj_set_style_text_color(latest_label, qh_color(QH_MUTED), 0);
    lv_label_set_text(latest_label, "回到最新");
    lv_obj_center(latest_label);
    lv_obj_add_event_cb(latest_button, latest_event, LV_EVENT_CLICKED, NULL);
    update_latest_button();
    lv_obj_t *clear = button(text_screen, font, "清空", 132, 214, QH_SOFT, clear_event);
    lv_obj_set_size(clear, 132, 70);
    clear_control = clear;
    lv_obj_set_align(clear, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(clear, UI_WIDTH - UI_MARGIN - 132, UI_HOLD_TOP);
    lv_obj_t *language_button = lv_button_create(text_screen);
    qh_button(language_button, QH_SOFT);
    lv_obj_set_size(language_button, QH_TOOL_SIZE, QH_TOOL_SIZE);
    lv_obj_set_pos(language_button,QH_LANGUAGE_X,QH_TOOL_Y);
    lv_obj_t *language_label = lv_label_create(language_button);
    lv_obj_set_style_text_font(language_label, font, 0);
    lv_label_set_text(language_label, "拼");
    lv_obj_center(language_label);
    control_labels[3]=language_label;
    lv_obj_add_event_cb(language_button, text_entry_toggle_language, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hold = button(text_screen, font, "按住说话", UI_CONTENT_WIDTH - 152, 0, QH_ACCENT, NULL);
    hold_control = hold;
    lv_obj_set_height(hold, 70);
    lv_obj_set_align(hold, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(hold, UI_MARGIN, UI_HOLD_TOP);
    lv_obj_add_event_cb(hold, hold_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hold, hold_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(hold, hold_event, LV_EVENT_PRESS_LOST, NULL);
    qh_screen_load(text_screen, false);
    text_entry_mount(text_screen, transcript_font, accept_edited_text, NULL, input_layout, NULL,
                     UI_ASR_INPUT_TOP, language_button);
    text_entry_set_enabled(text_screen, false);
    xEventGroupSetBits(events, START_BIT);
    return ESP_OK;
}
