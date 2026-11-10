#include <inttypes.h>
#include "ui_layout.h"
#include "qinghe_ui.h"
#include "qinghe_home.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "esp_chip_info.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "esp_wifi_default_config.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "lvgl.h"
#include "p4_font_loader.h"
#include "chat_history.h"
#include "text_entry.h"
#include "pinyin_service.h"
#include "usb_keyboard.h"
#include "cloud_text.h"
#include "device_about.h"
#include "wifi_settings.h"
#include "screen_idle.h"
#include "screen_idle_policy.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"
#include "volc_conv_ai.h"
#if CONFIG_P4_SIGN_LANGUAGE_MODE
#include "sign_language_mode.h"
#include "portrait_display.h"
#else
static inline bool sign_language_mode_active(void) { return false; }
#endif
#if CONFIG_P4_ASR_MODE
#include "speech_to_text_mode.h"
#else
static inline bool speech_to_text_mode_active(void) { return false; }
#endif

static bool voice_ui_suspended(void)
{
    return sign_language_mode_active() || speech_to_text_mode_active();
}

/* Keep both credential headers outside compiler include paths.  This avoids a
 * name collision between the local credentials file and the vendor SDK public
 * header, and keeps secrets out of source control. */
#include "../../../secrets/p4x_wifi.h"
#include "../../../secrets/p4x_font_transfer.h"
#include "../../../secrets/volc_conv_ai.h"
#if __has_include("../../../secrets/p4x_device_override.h")
#include "../../../secrets/p4x_device_override.h"
#endif

#define APP_BOARD_PROFILE "p4x_function_ev_board"
#define BYTES_PER_MIB (1024U * 1024U)
/* The official high-quality RTC pipeline resamples captured PCM to 8 kHz
 * before volc_send_audio_data() when the negotiated codec is G711A.  Sending
 * 16 kHz PCM here is accepted by the API but interpreted at the wrong rate by
 * the internal encoder/cloud VAD. */
#define AUDIO_SAMPLE_RATE_HZ 8000
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS 1
#define AUDIO_FRAME_MILLISECONDS 20
#define AUDIO_FRAME_BYTES (AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS * AUDIO_FRAME_MILLISECONDS / 1000)
#define AUDIO_END_TAIL_MILLISECONDS 2000
#define AUDIO_END_TAIL_FRAMES (AUDIO_END_TAIL_MILLISECONDS / AUDIO_FRAME_MILLISECONDS)
#define LOCAL_LOOP_SAMPLE_RATE_HZ 16000
#define LOCAL_LOOP_SECONDS 3
#define LOCAL_LOOP_BUFFER_BYTES (LOCAL_LOOP_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS * LOCAL_LOOP_SECONDS)
#define LOCAL_LOOP_TASK_STACK_BYTES 4096
#define AUDIO_TASK_STACK_BYTES 6144
#define AUDIO_PLAYBACK_TASK_STACK_BYTES 6144
#define AUDIO_IDLE_UPLINK_TASK_STACK_BYTES 3072
#define CLOUD_AUDIO_PLAYBACK_SAMPLE_RATE_HZ 8000
#define CLOUD_AUDIO_MAX_FRAME_BYTES 1024
#define CLOUD_AUDIO_QUEUE_LENGTH 24
#define CLOUD_AUDIO_IDLE_CLOSE_MILLISECONDS 1500
#define CLOUD_TASK_STACK_BYTES 12288
#define CLOUD_RECOVERY_TASK_STACK_BYTES 4096
#define CLOUD_RECONNECT_DELAY_MILLISECONDS 1200
#define CLOUD_MESSAGE_MAX_BYTES 4096
#define WIFI_MAXIMUM_RETRIES 5
#define FONT_SYNC_TASK_STACK_BYTES 8192
#define FONT_SEGMENT_BYTES 4096U
#define SDCARD_FONT_VFS_PATH "/sdcard/fonts/P4ZH14.BIN"
#define SDCARD_FONT_LVGL_PATH "A:fonts/P4ZH14.BIN"
#define UI_FONT_TARGET_MAX 16

static const char *TAG = "p4x_baseline";
extern const lv_font_t lv_font_project_zh;
static lv_font_t *sdcard_font;
static atomic_bool startup_fonts_done;
static atomic_bool startup_base_loading;
static atomic_bool startup_home_visible;
static atomic_bool startup_base_done;
static atomic_bool startup_audio_ready;
static atomic_bool startup_network_prepared;
static lv_obj_t *startup_overlay;
static lv_obj_t *startup_home_screen;
static lv_obj_t *startup_label;
static lv_obj_t *startup_stages[3], *startup_stage_labels[3];
static atomic_bool startup_resources_started, startup_resources_failed;
static atomic_int startup_font_error;
static uint32_t startup_started;
static unsigned startup_last_mask=UINT_MAX;
static bool startup_finishing;
static uint32_t startup_last_diagnostic;
extern const lv_font_t lv_font_startup_zh;

static lv_obj_t *font_targets[UI_FONT_TARGET_MAX];
static size_t font_target_count;
static lv_obj_t *status_label;
static lv_obj_t *user_text_label;
static lv_obj_t *agent_text_label;
static lv_obj_t *settings_screen;
static lv_obj_t *settings_home_screen;
static lv_obj_t *home_ui_screen, *menu_screen, *startup_leaf;
static qh_home_t menu_home;
static lv_obj_t *home_hold_control, *home_subtitle;
static int home_input_height=UI_INPUT_COLLAPSED;
static void home_reading_layout(void);
static lv_obj_t *home_title_label;
static lv_obj_t *settings_icon_label;
static lv_obj_t *typing_icon_label;
static lv_obj_t *settings_history_value;
static lv_obj_t *settings_clear_dialog;
static char chat_user_sender[128], chat_assistant_sender[128];
static atomic_bool chat_user_has_spoken;
static atomic_bool idle_welcome_started, idle_welcome_finished;
static atomic_bool idle_welcome_pcm_played;
static lv_obj_t *settings_brightness_value;
static atomic_int settings_brightness = 70;
static atomic_int settings_volume_requested = 45;
static atomic_int settings_volume_applied = 45;
static atomic_int settings_volume_error;
static lv_obj_t *settings_volume_slider;
static lv_obj_t *settings_volume_value;
static lv_obj_t *settings_save_label;
/* 0 preserves page defaults until the user chooses a size. */
static atomic_int settings_subtitle_size;
static lv_obj_t *settings_subtitle_value;
static lv_obj_t *settings_subtitle_buttons[2];
static lv_obj_t *settings_timeout_dropdown;
static QueueHandle_t settings_save_queue;
static nvs_handle_t settings_nvs;
/* 0: ready without saved values, 1: pending, 2: saved, 3: failed. */
static atomic_int settings_save_status;
static void settings_schedule_save(void);
static void settings_relayout(void);
static lv_obj_t *settings_rows[8];
static atomic_bool settings_test_enabled;
/* 0: idle, 1: queued, 2: playing, 3: finished, 4: busy, 5: failed, 6: stopped. */
static atomic_int settings_test_status;
static bool settings_volume_changed;
static uint32_t settings_test_requested_at;
static bool settings_return_pending, settings_after_camera;
static void settings_button_callback(lv_event_t *event);
static atomic_uint settings_last_audible_pcm_ms;
#define SETTINGS_TEST_FRAME_LENGTH SIZE_MAX
static esp_codec_dev_handle_t microphone;
static esp_codec_dev_handle_t speaker;
static QueueHandle_t cloud_audio_queue;
static EventGroupHandle_t audio_event_group;
static SemaphoreHandle_t cloud_engine_lock;
/*
 * RTC invokes callbacks from its own small VolcRTCMain task stack (about
 * 1.5 KiB on the current prebuilt SDK).  Keep subtitle decode scratch space
 * out of that stack: a received subtitle can legitimately be several hundred
 * bytes, and CLOUD_MESSAGE_MAX_BYTES is intentionally 4 KiB.
 *
 * The SDK serializes message callbacks on this task, so one shared buffer is
 * sufficient.  It is consumed synchronously before the callback returns.
 */
static char cloud_message_buffer[CLOUD_MESSAGE_MAX_BYTES + 1];
static char cloud_screen_text[512];
/* The hosted agent can deliver a complete subtitle before its first PCM
 * packet.  Hold that reply briefly so a listener never sees a finished
 * answer in silence; release it at the first playable PCM frame. */
static char cloud_reply_text_pending[512];
static portMUX_TYPE cloud_reply_text_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool conversation_recording;
static volatile bool conversation_finish_requested;
static volatile bool local_loop_active;
static volatile bool cloud_playback_suppressed;
static volatile bool cloud_connected;
/* Subtitle packets do not expose a documented, stable speaker role.  The
 * conversation state is the safe local discriminator: ASR arrives before the
 * hosted agent enters ANSWERING, while its reply subtitles arrive afterwards.
 * Do not log raw userId values: they may be service identifiers. */
static volatile bool cloud_agent_answering;
static volatile bool cloud_reply_audio_started;
static volatile bool wifi_link_up;
static volatile bool cloud_recovery_in_progress;
static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count;
static volatile bool cloud_start_requested;
static volatile bool font_sync_requested;
static volatile bool font_sync_in_progress;
static volc_engine_t cloud_engine;
static volatile bool camera_capture_active;
static volatile bool camera_stop_requested;
static lv_obj_t *camera_button_label;
static lv_obj_t *camera_preview_image;
static uint8_t *camera_preview_buffer;
static lv_image_dsc_t camera_preview_desc;
static uint32_t camera_preview_width;
static uint32_t camera_preview_height;

typedef enum {
    CAMERA_STATE_OFF = 0,
    CAMERA_STATE_STARTING,
    CAMERA_STATE_PREVIEW,
    CAMERA_STATE_STOPPING,
    CAMERA_STATE_ERROR,
} camera_state_t;

static volatile camera_state_t camera_state = CAMERA_STATE_OFF;

typedef enum {
    VOICE_STATE_IDLE = 0,
    VOICE_STATE_RECORDING,
    VOICE_STATE_FINISHING,
    VOICE_STATE_THINKING,
    VOICE_STATE_ANSWERING,
    VOICE_STATE_RECOVERING,
} voice_state_t;

typedef struct {
    uint32_t session_id;
    uint32_t started_at_ms;
    bool first_asr_logged;
    bool first_reply_pcm_logged;
    bool first_reply_text_logged;
} voice_timing_t;

static volatile voice_state_t voice_state = VOICE_STATE_RECOVERING;
static voice_timing_t voice_timing;
static void set_status_text(const char *text);

#if 0 /* Camera preview experiment is isolated from the stable voice baseline. */
/* Camera is demand-driven: voice mode owns no CSI/DMA buffers.  This first
 * milestone only verifies sensor detection and stable frame capture. */
#define CAMERA_BUFFER_COUNT 2
#define CAMERA_CAPTURE_SECONDS 5

static void camera_capture_task(void *arg)
{
    (void)arg;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int fd = -1;
    struct v4l2_buffer buf;
    struct v4l2_requestbuffers req;
    struct v4l2_format format;
    struct v4l2_capability capability;
    void *mapped[CAMERA_BUFFER_COUNT] = {0};
    size_t mapped_len[CAMERA_BUFFER_COUNT] = {0};
    uint32_t frames = 0;
    uint32_t bytes = 0;
    bool streaming = false;

    if (camera_capture_active) {
        set_status_text("摄像头测试已在进行中");
        vTaskDelete(NULL);
        return;
    }
    camera_capture_active = true;
    camera_stop_requested = false;
    camera_state = CAMERA_STATE_STARTING;
    set_status_text("正在启动摄像头，请稍候...");
    ESP_LOGI(TAG, "camera: starting demand-driven MIPI-CSI capture");

    esp_err_t result = bsp_camera_start(NULL);
    if (result != ESP_OK) {
        camera_state = CAMERA_STATE_ERROR;
        ESP_LOGE(TAG, "camera: BSP start failed: %s", esp_err_to_name(result));
        char camera_error[96];
        snprintf(camera_error, sizeof(camera_error), "摄像头启动失败：%s", esp_err_to_name(result));
        set_status_text(camera_error);
        goto cleanup;
    }
    /* The CSI V4L2 node is opened read/write by the current ESP-Video
     * implementation; O_RDONLY can return ENODEV on some P4 BSP revisions. */
    /* SC2336 is a RAW sensor.  The P4 ISP output node is the display-ready
     * path; keep the CSI node as a fallback for boards/configurations where
     * ISP output is not exposed. */
    const char *device_name = ESP_VIDEO_ISP1_DEVICE_NAME;
    fd = open(device_name, O_RDWR);
    if (fd < 0) {
        camera_state = CAMERA_STATE_ERROR;
        device_name = ESP_VIDEO_MIPI_CSI_DEVICE_NAME;
        fd = open(device_name, O_RDWR);
    }
    if (fd < 0) {
        ESP_LOGE(TAG, "camera: open %s failed, errno=%d (%s)",
                 device_name, errno, strerror(errno));
        set_status_text("摄像头设备节点打开失败，正在记录诊断");
        goto cleanup_video;
    }
    memset(&capability, 0, sizeof(capability));
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        camera_state = CAMERA_STATE_ERROR;
        ESP_LOGE(TAG, "camera: VIDIOC_QUERYCAP failed, errno=%d", errno);
        set_status_text("摄像头传感器识别失败");
        goto cleanup_fd;
    }
    ESP_LOGI(TAG, "camera: driver=%s card=%s bus=%s caps=0x%" PRIx32,
             capability.driver, capability.card, capability.bus_info, capability.capabilities);

    memset(&format, 0, sizeof(format));
    format.type = type;
    /* Ask the ISP for a modest RGB565 frame so it can be copied directly to
     * the 7-inch LVGL display.  If this node is the raw CSI fallback, retain
     * its native format and keep the diagnostic capture path working. */
    format.fmt.pix.width = 320;
    format.fmt.pix.height = 240;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0 || ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        camera_state = CAMERA_STATE_ERROR;
        ESP_LOGE(TAG, "camera: VIDIOC_G_FMT failed, errno=%d", errno);
        set_status_text("摄像头格式读取失败");
        goto cleanup_fd;
    }
    ESP_LOGI(TAG, "camera: format=%" PRIu32 "x%" PRIu32 " pixfmt=0x%" PRIx32 " bytesperline=%" PRIu32,
             format.fmt.pix.width, format.fmt.pix.height, format.fmt.pix.pixelformat,
             format.fmt.pix.bytesperline);

    const bool displayable_rgb565 = format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565;
    if (displayable_rgb565) {
        camera_preview_width = format.fmt.pix.width;
        camera_preview_height = format.fmt.pix.height;
        const size_t preview_bytes = (size_t)camera_preview_width * camera_preview_height * 2U;
        camera_preview_buffer = heap_caps_malloc(preview_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (camera_preview_buffer == NULL) {
            ESP_LOGE(TAG, "camera: unable to allocate %u-byte preview buffer", (unsigned)preview_bytes);
            set_status_text("摄像头预览内存不足");
            goto cleanup_fd;
        }
        memset(camera_preview_buffer, 0, preview_bytes);
        memset(&camera_preview_desc, 0, sizeof(camera_preview_desc));
        camera_preview_desc.header.cf = LV_COLOR_FORMAT_RGB565;
        camera_preview_desc.header.w = camera_preview_width;
        camera_preview_desc.header.h = camera_preview_height;
        camera_preview_desc.header.stride = camera_preview_width * 2U;
        camera_preview_desc.data_size = preview_bytes;
        camera_preview_desc.data = camera_preview_buffer;
        if (bsp_display_lock(pdMS_TO_TICKS(1000))) {
            if (camera_preview_image == NULL) {
                camera_preview_image = lv_image_create(lv_screen_active());
            }
            lv_image_set_src(camera_preview_image, &camera_preview_desc);
            lv_obj_align(camera_preview_image, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(camera_preview_image, LV_OBJ_FLAG_HIDDEN);
            if (status_label != NULL) lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
            if (user_text_label != NULL) lv_obj_add_flag(user_text_label, LV_OBJ_FLAG_HIDDEN);
            if (agent_text_label != NULL) lv_obj_add_flag(agent_text_label, LV_OBJ_FLAG_HIDDEN);
            bsp_display_unlock();
        }
        set_status_text("摄像头实时预览中，点击测试按钮可停止");
    }

    memset(&req, 0, sizeof(req));
    req.count = CAMERA_BUFFER_COUNT;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count < CAMERA_BUFFER_COUNT) {
        ESP_LOGE(TAG, "camera: VIDIOC_REQBUFS failed, errno=%d count=%" PRIu32, errno, req.count);
        set_status_text("摄像头缓冲区申请失败");
        goto cleanup_fd;
    }
    for (uint32_t index = 0; index < CAMERA_BUFFER_COUNT; ++index) {
        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = index;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "camera: VIDIOC_QUERYBUF[%" PRIu32 "] failed, errno=%d", index, errno);
            goto cleanup_fd;
        }
        mapped_len[index] = buf.length;
        mapped[index] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (mapped[index] == MAP_FAILED) {
            mapped[index] = NULL;
            ESP_LOGE(TAG, "camera: mmap[%" PRIu32 "] failed, errno=%d", index, errno);
            goto cleanup_fd;
        }
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "camera: initial QBUF[%" PRIu32 "] failed, errno=%d", index, errno);
            goto cleanup_fd;
        }
    }
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "camera: VIDIOC_STREAMON failed, errno=%d", errno);
        set_status_text("摄像头开始采集失败");
        goto cleanup_fd;
    }
    streaming = true;
    camera_state = CAMERA_STATE_PREVIEW;
    set_status_text("摄像头实时预览中，再次点击按钮停止");
    const int64_t started_us = esp_timer_get_time();
    int64_t last_report_us = started_us;
    while (!camera_stop_requested) {
        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "camera: VIDIOC_DQBUF failed, errno=%d", errno);
            break;
        }
        if ((buf.flags & V4L2_BUF_FLAG_ERROR) == 0) {
            ++frames;
            bytes += buf.bytesused;
            if (displayable_rgb565 && camera_preview_buffer != NULL && buf.index < CAMERA_BUFFER_COUNT) {
                const size_t copy_bytes = MIN((size_t)buf.bytesused,
                                              (size_t)camera_preview_width * camera_preview_height * 2U);
                memcpy(camera_preview_buffer, mapped[buf.index], copy_bytes);
                if (bsp_display_lock(0)) {
                    if (camera_preview_image != NULL) {
                        lv_obj_invalidate(camera_preview_image);
                    }
                    bsp_display_unlock();
                }
            }
        }
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "camera: QBUF after frame failed, errno=%d", errno);
            break;
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_report_us >= 1000000LL) {
            ESP_LOGI(TAG, "camera: frames=%" PRIu32 " fps~%" PRIu32 " avg_bytes=%" PRIu32 " free_psram=%" PRIu32,
                     frames, (uint32_t)(frames * 1000000ULL / (uint64_t)(now_us - started_us)),
                     frames == 0 ? 0U : bytes / frames,
                     (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            last_report_us = now_us;
        }
    }
    result = frames > 0 ? ESP_OK : ESP_FAIL;
    camera_state = CAMERA_STATE_STOPPING;
    if (streaming) {
        (void)ioctl(fd, VIDIOC_STREAMOFF, &type);
        streaming = false;
    }
    ESP_LOGI(TAG, "camera: capture complete frames=%" PRIu32 " avg_bytes=%" PRIu32,
             frames, frames == 0 ? 0U : bytes / frames);
    set_status_text(camera_stop_requested ? "正在停止摄像头预览..." :
                    (result == ESP_OK ? "摄像头采集正常，测试完成" : "摄像头未采到有效画面"));

cleanup_fd:
    if (streaming) {
        (void)ioctl(fd, VIDIOC_STREAMOFF, &type);
    }
    for (uint32_t index = 0; index < CAMERA_BUFFER_COUNT; ++index) {
        if (mapped[index] != NULL) {
            (void)munmap(mapped[index], mapped_len[index]);
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    if (camera_preview_buffer != NULL) {
        heap_caps_free(camera_preview_buffer);
        camera_preview_buffer = NULL;
    }
    if (bsp_display_lock(pdMS_TO_TICKS(1000))) {
        if (camera_preview_image != NULL) {
            lv_obj_add_flag(camera_preview_image, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_label != NULL) lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        if (user_text_label != NULL) lv_obj_clear_flag(user_text_label, LV_OBJ_FLAG_HIDDEN);
        if (agent_text_label != NULL) lv_obj_clear_flag(agent_text_label, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();
    }
cleanup_video:
    (void)esp_video_deinit();
cleanup:
    camera_capture_active = false;
    camera_stop_requested = false;
    camera_state = CAMERA_STATE_OFF;
    if (result == ESP_OK) {
        set_status_text("摄像头预览已停止，语音功能可用");
    }
    vTaskDelete(NULL);
}

static void camera_button_event_callback(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (camera_capture_active) {
        camera_stop_requested = true;
        camera_state = CAMERA_STATE_STOPPING;
        set_status_text("正在停止摄像头预览...");
        return;
    }
    if (conversation_recording || voice_state == VOICE_STATE_ANSWERING) {
        set_status_text("请先结束当前语音对话，再测试摄像头");
        return;
    }
    if (xTaskCreate(camera_capture_task, "camera_capture", 8192, NULL, 4, NULL) != pdPASS) {
        set_status_text("摄像头测试任务启动失败");
    }
}
#endif

static void start_font_synchronization(void);
static void start_cloud_registration(void);
static void request_cloud_recovery(const char *reason);

typedef struct {
    size_t length;
    uint8_t data[CLOUD_AUDIO_MAX_FRAME_BYTES];
} cloud_audio_frame_t;
static bool audio_frame_has_sound(const cloud_audio_frame_t *frame);

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define AUDIO_PLAYBACK_IDLE_BIT BIT0

static const char VOLC_RTC_CONFIG_FORMAT[] =
    "{"
    "\"ver\":1,"
    "\"iot\":{"
    "\"instance_id\":\"%s\","
    "\"product_key\":\"%s\","
    "\"product_secret\":\"%s\","
    "\"device_name\":\"%s\"},"
    "\"rtc\":{"
    /* The vendor RTC binary logs the complete room token at INFO level.
     * Use FATAL only in field builds so serial output cannot disclose an
     * active session credential.  Application-level connection events above
     * remain available through ESP_LOG. */
    "\"log_level\":4,"
    "\"audio\":{\"publish\":true,\"subscribe\":true,\"codec\":3},"
    "\"video\":{\"publish\":false,\"subscribe\":false,\"codec\":1},"
    /* These parameters are required by Volcano Engine's official ESP32
     * high-quality RTC example.  Without them a peer can join the room while
     * the agent never consumes the published microphone track. */
    "\"params\":["
    "\"{\\\"debug\\\":{\\\"log_to_console\\\":1}}\","
    "\"{\\\"audio\\\":{\\\"codec\\\":{\\\"internal\\\":{\\\"enable\\\":1}}}}\","
    "\"{\\\"rtc\\\":{\\\"access\\\":{\\\"concurrent_requests\\\":1}}}\","
    "\"{\\\"rtc\\\":{\\\"ice\\\":{\\\"concurrent_agents\\\":1}}}\""
    "]}}";

static void set_status_text(const char *text)
{
    if (bsp_display_lock(pdMS_TO_TICKS(1000))) {
        if(strcmp(lv_label_get_text(status_label),text))lv_label_set_text(status_label, text);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "could not acquire LVGL lock to update audio status");
    }
}

static const lv_font_t *project_ui_font(void)
{
#if CONFIG_P4_ASR_MODE
    if (atomic_load(&settings_subtitle_size) == 28) {
        const lv_font_t *large = speech_to_text_mode_font28();
        if (large != NULL) return large;
    }
    const lv_font_t *shared = speech_to_text_mode_font14();
    if (shared != NULL) return shared;
#endif
    return sdcard_font != NULL ? sdcard_font : &lv_font_project_zh;
}

static void apply_project_font(lv_obj_t *object)
{
    const lv_font_t *font = settings_cached_font(project_ui_font());
    lv_obj_set_style_text_font(object, font, LV_PART_MAIN);
    for (size_t index = 0; index < font_target_count; ++index) {
        if (font_targets[index] == object) {
            return;
        }
    }
    if (font_target_count < UI_FONT_TARGET_MAX) {
        font_targets[font_target_count++] = object;
    }
}

static void apply_global_font_tree(lv_obj_t *object, const lv_font_t *font)
{
    if (!object) return;
    if (text_entry_is_root(object)) return;
    if (!lv_obj_has_flag(object, LV_OBJ_FLAG_USER_1) && object != settings_icon_label && object != typing_icon_label &&
        lv_obj_check_type(object, &lv_label_class))
        lv_obj_set_style_text_font(object, font, LV_PART_MAIN);
    const uint32_t count = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < count; ++i)
        apply_global_font_tree(lv_obj_get_child(object, i), font);
}

static void home_reading_layout(void)
{
    if(!home_subtitle || !status_label)return;
    static bool busy;if(busy)return;busy=true;
    lv_point_t sub,status;
    const lv_font_t *sf=lv_obj_get_style_text_font(home_subtitle,0);
    const lv_font_t *tf=lv_obj_get_style_text_font(status_label,0);
    lv_text_get_size(&sub,lv_label_get_text(home_subtitle),sf,0,0,UI_CONTENT_WIDTH,LV_TEXT_FLAG_NONE);
    lv_text_get_size(&status,lv_label_get_text(status_label),tf,0,0,UI_CONTENT_WIDTH,LV_TEXT_FLAG_NONE);
    const int y=82+sub.y+8;
    lv_obj_set_align(home_subtitle,LV_ALIGN_TOP_LEFT);lv_obj_set_align(status_label,LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(home_subtitle,UI_MARGIN,82);lv_obj_set_pos(status_label,UI_MARGIN,y);
    const int limit=UI_HEIGHT-UI_MARGIN-home_input_height;
    const int dock_top=UI_HOME_INPUT_TOP<limit?UI_HOME_INPUT_TOP:limit;
    if(home_hold_control)lv_obj_set_y(home_hold_control,dock_top-82);
    chat_history_set_view_bounds(y+status.y+16,dock_top-94);
    busy=false;
}
static void home_header_changed(lv_event_t *event)
{
    (void)event;home_reading_layout();
}

static void apply_subtitle_font(void)
{
    const int size = atomic_load(&settings_subtitle_size);
    const lv_font_t *font = project_ui_font();
#if CONFIG_P4_ASR_MODE
    if (size == 28 && speech_to_text_mode_font28() != NULL)
        font = speech_to_text_mode_font28();
    speech_to_text_mode_set_subtitle_size(size);
#else
    (void)size;
#endif
    if (user_text_label) lv_obj_set_style_text_font(user_text_label, font, LV_PART_MAIN);
    if (agent_text_label) lv_obj_set_style_text_font(agent_text_label, font, LV_PART_MAIN);
    if (size != 0) {
        apply_global_font_tree(home_ui_screen, settings_cached_font(font));
        apply_global_font_tree(menu_screen, settings_cached_font(font));
        apply_global_font_tree(settings_screen, settings_cached_font(font));
    }
    /* Apply chat's cached font after the global walk, which binds SD sources. */
    chat_history_set_font(font);
    wifi_settings_set_font(font);
    if (settings_timeout_dropdown) {
        const lv_font_t *settings_font = settings_cached_font(font);
        lv_obj_set_style_text_font(settings_timeout_dropdown, settings_font, LV_PART_MAIN);
        lv_obj_t *list = lv_dropdown_get_list(settings_timeout_dropdown);
        lv_obj_set_style_text_font(list, settings_font, LV_PART_MAIN);
        apply_global_font_tree(list, settings_font);
    }
    const lv_font_t *menu_heading = settings_cached_font(font);
#if CONFIG_P4_ASR_MODE
    /* The product title keeps its heading size independently of global text. */
    const lv_font_t *title_font = speech_to_text_mode_cached_font28();
    if (home_title_label && title_font)
        lv_obj_set_style_text_font(home_title_label, title_font, LV_PART_MAIN);
    if (title_font) {
        text_entry_set_font(home_ui_screen, title_font);
        menu_heading = title_font;
    }
#endif
    qh_home_set_fonts(&menu_home, settings_cached_font(font), menu_heading);
    settings_relayout();
    home_reading_layout();
}

static void refresh_ui_font_from_sdcard(void)
{
    const lv_font_t *font = settings_cached_font(project_ui_font());
    for (size_t index = 0; index < font_target_count; ++index) {
        lv_obj_set_style_text_font(font_targets[index], font, LV_PART_MAIN);
    }
    apply_subtitle_font();
}

static void set_conversation_text(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    if (bsp_display_lock(pdMS_TO_TICKS(1000))) {
        lv_label_set_text(label, text);
        bsp_display_unlock();
    }
}

static void hold_agent_reply_text(const char *text)
{
    portENTER_CRITICAL(&cloud_reply_text_lock);
    snprintf(cloud_reply_text_pending, sizeof(cloud_reply_text_pending), "%s", text);
    portEXIT_CRITICAL(&cloud_reply_text_lock);
}

static bool take_agent_reply_text(char *destination, size_t destination_size)
{
    bool available;

    if (destination == NULL || destination_size == 0) {
        return false;
    }
    portENTER_CRITICAL(&cloud_reply_text_lock);
    available = cloud_reply_text_pending[0] != '\0';
    if (available) {
        snprintf(destination, destination_size, "%s", cloud_reply_text_pending);
    }
    portEXIT_CRITICAL(&cloud_reply_text_lock);
    return available;
}

static void clear_agent_reply_text(void)
{
    portENTER_CRITICAL(&cloud_reply_text_lock);
    cloud_reply_text_pending[0] = '\0';
    portEXIT_CRITICAL(&cloud_reply_text_lock);
}

static uint32_t voice_elapsed_ms(void)
{
    return esp_log_timestamp() - voice_timing.started_at_ms;
}

static void reset_voice_session_metrics(void)
{
    ++voice_timing.session_id;
    voice_timing.started_at_ms = esp_log_timestamp();
    voice_timing.first_asr_logged = false;
    voice_timing.first_reply_pcm_logged = false;
    voice_timing.first_reply_text_logged = false;
    ESP_LOGI(TAG, "metric: session=%" PRIu32 " capture_start_ms=%" PRIu32,
             voice_timing.session_id, voice_timing.started_at_ms);
}

static void abort_voice_session(const char *status)
{
    conversation_recording = false;
    conversation_finish_requested = false;
    cloud_agent_answering = false;
    cloud_reply_audio_started = false;
    clear_agent_reply_text();
    if (cloud_audio_queue != NULL) {
        xQueueReset(cloud_audio_queue);
    }
    if (status != NULL) {
        set_status_text(status);
    }
}

static int send_cloud_audio_frame(const void *data, size_t length,
                                  volc_audio_frame_info_t *info)
{
    int result = VOLC_ERR_FAILED;

    if (cloud_engine_lock == NULL || xSemaphoreTake(cloud_engine_lock, 0) != pdTRUE) {
        return result;
    }
    if (cloud_connected && cloud_engine != NULL) {
        result = volc_send_audio_data(cloud_engine, data, length, info);
    }
    xSemaphoreGive(cloud_engine_lock);
    return result;
}

static int interrupt_cloud_answer(void)
{
    int result = VOLC_ERR_FAILED;

    if (cloud_engine_lock == NULL ||
        xSemaphoreTake(cloud_engine_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return result;
    }
    if (cloud_connected && cloud_engine != NULL) {
        result = volc_interrupt(cloud_engine);
    }
    xSemaphoreGive(cloud_engine_lock);
    return result;
}

static void cloud_event_callback(volc_engine_t handle, volc_event_t *event, void *user_data)
{
    if (voice_ui_suspended()) {
        /* Preserve transport truth while camera mode suppresses voice UI.
         * A real disconnect must not look like a healthy retained session. */
        if (event != NULL && event->code == VOLC_EV_CONNECTED) {
            cloud_connected = true;
        } else if (event != NULL && event->code == VOLC_EV_DISCONNECTED) {
            cloud_connected = false;
        }
        return;
    }
    (void)handle;
    (void)user_data;
    if (event == NULL) {
        return;
    }
    if (event->code == VOLC_EV_CONNECTED) {
        ESP_LOGI(TAG, "cloud: Volcano Engine RTC connected");
        cloud_connected = true;
        cloud_recovery_in_progress = false;
        cloud_agent_answering = false;
        cloud_reply_audio_started = false;
        voice_state = VOICE_STATE_IDLE;
        ESP_LOGI(TAG, "metric: cloud_reconnect_complete_ms=%" PRIu32, esp_log_timestamp());
        set_status_text("网络和音频已就绪：请按住下方按钮说话");
    } else if (event->code == VOLC_EV_DISCONNECTED) {
        ESP_LOGW(TAG, "cloud: Volcano Engine RTC disconnected");
        cloud_connected = false;
        voice_state = VOICE_STATE_RECOVERING;
        abort_voice_session("云端语音已断开，正在自动重连...");
        request_cloud_recovery("rtc_disconnected");
    }
}

static void cloud_conversation_status_callback(volc_engine_t handle, volc_conv_status_e status, void *user_data)
{
    if (voice_ui_suspended()) {
        return;
    }
    (void)handle;
    (void)user_data;
    ESP_LOGI(TAG, "cloud: conversation state=%d", (int)status);
    switch (status) {
    case VOLC_CONV_STATUS_LISTENING:
        if (atomic_load(&idle_welcome_started) && !atomic_load(&chat_user_has_spoken))
            atomic_store(&idle_welcome_finished, true);
        cloud_agent_answering = false;
        cloud_reply_audio_started = false;
        if (voice_state == VOICE_STATE_RECORDING) {
            set_status_text("正在聆听，请按住说话，松开后发送");
        }
        break;
    case VOLC_CONV_STATUS_THINKING:
        cloud_agent_answering = false;
        cloud_reply_audio_started = false;
        if (voice_state == VOICE_STATE_FINISHING || voice_state == VOICE_STATE_RECORDING) {
            voice_state = VOICE_STATE_THINKING;
        }
        set_status_text("正在理解你说的话...");
        break;
    case VOLC_CONV_STATUS_ANSWERING:
        if (!atomic_load(&chat_user_has_spoken)) {
            atomic_store(&idle_welcome_started, true);
            atomic_store(&idle_welcome_finished, false);
        }
        cloud_agent_answering = true;
        cloud_reply_audio_started = false;
        voice_state = VOICE_STATE_ANSWERING;
        clear_agent_reply_text();
        set_status_text("正在通过扬声器回复...");
        break;
    case VOLC_CONV_STATUS_INTERRUPTED:
        cloud_agent_answering = false;
        cloud_reply_audio_started = false;
        voice_state = VOICE_STATE_IDLE;
        set_status_text("对话已打断，点击开始即可重新说话");
        break;
    case VOLC_CONV_STATUS_ANSWER_FINISH:
        if (atomic_load(&idle_welcome_started) && !atomic_load(&chat_user_has_spoken))
            atomic_store(&idle_welcome_finished, true);
        cloud_agent_answering = false;
        cloud_reply_audio_started = false;
        voice_state = VOICE_STATE_IDLE;
        ESP_LOGI(TAG, "metric: session=%" PRIu32 " answer_finish_elapsed_ms=%" PRIu32,
                 voice_timing.session_id, voice_elapsed_ms());
        set_status_text("回复结束，点击开始可继续对话");
        break;
    default:
        break;
    }
}

static void cloud_audio_callback(volc_engine_t handle, const void *data, size_t length,
                                 volc_audio_frame_info_t *info, void *user_data)
{
    if (voice_ui_suspended()) {
        return;
    }
    static unsigned received_frame_count;
    static int last_data_type = -1;

    (void)handle;
    (void)user_data;
    ++received_frame_count;
    if (info == NULL) {
        ESP_LOGW(TAG, "cloud: audio frame received without format metadata (length=%u)",
                 (unsigned)length);
    } else if (received_frame_count == 1 || last_data_type != (int)info->data_type) {
        last_data_type = (int)info->data_type;
        ESP_LOGI(TAG, "cloud: first/change audio frame (length=%u, data_type=%d, commit=%s)",
                 (unsigned)length, last_data_type, info->commit ? "yes" : "no");
    }
    if (data == NULL || info == NULL || info->data_type != VOLC_AUDIO_DATA_TYPE_PCM ||
        length == 0 || length > CLOUD_AUDIO_MAX_FRAME_BYTES ||
        cloud_audio_queue == NULL || cloud_playback_suppressed) {
        return;
    }
    if (cloud_agent_answering && !cloud_reply_audio_started) {
        cloud_reply_audio_started = true;
        ESP_LOGI(TAG, "cloud: first playable reply PCM received; releasing reply subtitle");
        if (!voice_timing.first_reply_pcm_logged) {
            voice_timing.first_reply_pcm_logged = true;
            ESP_LOGI(TAG, "metric: session=%" PRIu32 " first_reply_pcm_elapsed_ms=%" PRIu32,
                     voice_timing.session_id, voice_elapsed_ms());
        }
    }
    cloud_audio_frame_t frame = {.length = length};
    memcpy(frame.data, data, length);
    if (audio_frame_has_sound(&frame)) {
        atomic_store(&settings_last_audible_pcm_ms, (uint32_t)(esp_timer_get_time() / 1000));
    }
    if (xQueueSend(cloud_audio_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cloud: speaker queue full; dropping %u-byte PCM frame",
                 (unsigned)length);
    }
}

/* Only the playback owner changes output codec settings, never the LVGL
 * callback. Preserve open/close and microphone handoff ordering. */
static void apply_requested_speaker_volume(void)
{
    const int requested = atomic_load(&settings_volume_requested);
    const int previous = atomic_load(&settings_volume_applied);
    if (requested == previous || speaker == NULL || local_loop_active) return;
    int error = esp_codec_dev_set_out_vol(speaker, requested);
    if (error == ESP_OK) error = esp_codec_dev_set_out_mute(speaker, requested == 0);
    if (error == ESP_OK) {
        atomic_store(&settings_volume_applied, requested);
        atomic_store(&settings_volume_error, 0);
        settings_schedule_save();
    } else {
        esp_codec_dev_set_out_vol(speaker, previous);
        esp_codec_dev_set_out_mute(speaker, previous == 0);
        int expected = requested;
        atomic_compare_exchange_strong(&settings_volume_requested, &expected, previous);
        atomic_store(&settings_volume_error, error);
        ESP_LOGW(TAG, "settings: speaker volume error=%d", error);
    }
}

static bool audio_frame_has_sound(const cloud_audio_frame_t *frame)
{
    if (frame->length == SETTINGS_TEST_FRAME_LENGTH) return true;
    for (size_t index = 0; index + 1 < frame->length; index += 2) {
        int16_t sample;
        memcpy(&sample, frame->data + index, sizeof(sample));
        /* G711 decoded digital silence can be +/-8 rather than exact zero. */
        if (sample > 8 || sample < -8) return true;
    }
    return false;
}

static bool feedback_voice_recent(void)
{
    const uint32_t last = atomic_load(&settings_last_audible_pcm_ms);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return last != 0 && now - last < 300;
}

static bool feedback_queue_allows_tone(void)
{
    cloud_audio_frame_t pending;
    /* Read only: queue reset/handoff can race a peek, so never remove frames.
     * Incoming real speech also updates the timestamp before being queued. */
    return xQueuePeek(cloud_audio_queue, &pending, 0) != pdTRUE ||
           (pending.length != 0 && !audio_frame_has_sound(&pending));
}

static void play_settings_test_tone(bool *output_open, esp_codec_dev_sample_info_t *format)
{
    int expected = 1;
    if (!atomic_compare_exchange_strong(&settings_test_status, &expected, 2)) return;
    if (!atomic_load(&settings_test_enabled) || voice_ui_suspended() ||
        conversation_recording || conversation_finish_requested || local_loop_active ||
        feedback_voice_recent() || atomic_load(&settings_volume_error) != 0 ||
        !feedback_queue_allows_tone()) {
        atomic_store(&settings_test_status, 4);
        ESP_LOGI(TAG, "settings: feedback skipped before playback");
        return;
    }
    /* 500 Hz, 120 ms, 8 kHz mono PCM; fade both ends over 20 ms. No file,
     * network request, microphone capture or additional codec owner. */
    static const int16_t wave[16] = {
        0, 3135, 5793, 7568, 8192, 7568, 5793, 3135,
        0, -3135, -5793, -7568, -8192, -7568, -5793, -3135
    };
    int16_t samples[160];
    const bool retained_output = *output_open;
    xEventGroupClearBits(audio_event_group, AUDIO_PLAYBACK_IDLE_BIT);
    esp_err_t error = retained_output ? ESP_OK : esp_codec_dev_open(speaker, format);
    int outcome = 3;
    if (error == ESP_OK) {
        *output_open = true;
        /* BSP uses 6 * 240 DMA frames (180 ms at 8 kHz). Append 200 ms
         * silence so close cannot cut off the audible tail in the DMA ring. */
        for (unsigned block = 0; block < 16; ++block) {
            if (!atomic_load(&settings_test_enabled) || voice_ui_suspended() ||
                conversation_recording || conversation_finish_requested || local_loop_active ||
                feedback_voice_recent() || !feedback_queue_allows_tone()) {
                outcome = 6;
                break;
            }
            apply_requested_speaker_volume();
            for (unsigned index = 0; index < 160; ++index) {
                if (block >= 6) {
                    samples[index] = 0;
                    continue;
                }
                const unsigned position = block * 160 + index;
                unsigned gain = position < 160 ? position : 160;
                if (959 - position < gain) gain = 959 - position;
                samples[index] = (int16_t)((int32_t)wave[position % 16] * (int)gain / 160);
            }
            error = esp_codec_dev_write(speaker, samples, sizeof(samples));
            if (error != ESP_OK) break;
        }
        if (!retained_output) {
            const esp_err_t close_error = esp_codec_dev_close(speaker);
            if (close_error == ESP_OK) *output_open = false;
            if (error == ESP_OK) error = close_error;
        }
    }
    if (!*output_open) xEventGroupSetBits(audio_event_group, AUDIO_PLAYBACK_IDLE_BIT);
    if (error != ESP_OK) {
        outcome = 5;
        ESP_LOGW(TAG, "settings: test tone error=%d", (int)error);
    }
    atomic_store(&settings_test_status, outcome);
    ESP_LOGI(TAG, "settings: feedback result=%d retained_output=%d", outcome, (int)retained_output);
}

static void cloud_audio_playback_task(void *arg)
{
    cloud_audio_frame_t frame;
    bool output_open = false;
    esp_codec_dev_sample_info_t format = {
        .sample_rate = CLOUD_AUDIO_PLAYBACK_SAMPLE_RATE_HZ,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };
    esp_err_t result;

    (void)arg;
    for (;;) {
        apply_requested_speaker_volume();
        if (xQueueReceive(cloud_audio_queue, &frame,
                          pdMS_TO_TICKS(CLOUD_AUDIO_IDLE_CLOSE_MILLISECONDS)) == pdTRUE) {
            apply_requested_speaker_volume();
            if (frame.length == SETTINGS_TEST_FRAME_LENGTH) {
                play_settings_test_tone(&output_open, &format);
                continue;
            }
            if (frame.length == 0 || cloud_playback_suppressed) {
                if (output_open) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_close(speaker));
                    output_open = false;
                    xEventGroupSetBits(audio_event_group, AUDIO_PLAYBACK_IDLE_BIT);
                    ESP_LOGI(TAG, "audio: cloud PCM playback stopped for microphone handoff");
                }
                continue;
            }
            if (!output_open) {
                xEventGroupClearBits(audio_event_group, AUDIO_PLAYBACK_IDLE_BIT);
                result = esp_codec_dev_open(speaker, &format);
                if (result != ESP_OK) {
                    xEventGroupSetBits(audio_event_group, AUDIO_PLAYBACK_IDLE_BIT);
                    ESP_LOGE(TAG, "audio: cloud speaker open failed: %s",
                             esp_err_to_name(result));
                    continue;
                }
                output_open = true;
                ESP_LOGI(TAG, "audio: cloud PCM playback started (8 kHz, 16-bit, mono)");
                if (cloud_agent_answering) {
                    char pending_text[sizeof(cloud_reply_text_pending)];
                    if (take_agent_reply_text(pending_text, sizeof(pending_text))) {
                        set_conversation_text(agent_text_label, pending_text);
                    }
                }
            }
            if (audio_frame_has_sound(&frame)) {
                atomic_store(&settings_last_audible_pcm_ms, (uint32_t)(esp_timer_get_time() / 1000));
            }
            result = esp_codec_dev_write(speaker, frame.data, (int)frame.length);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "audio: cloud speaker write failed: %s",
                         esp_err_to_name(result));
            } else if (audio_frame_has_sound(&frame) && !atomic_load(&chat_user_has_spoken)) {
                atomic_store(&idle_welcome_pcm_played, true);
            }
        } else if (output_open) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_close(speaker));
            output_open = false;
            xEventGroupSetBits(audio_event_group, AUDIO_PLAYBACK_IDLE_BIT);
            ESP_LOGI(TAG, "audio: cloud PCM playback stopped after idle timeout");
            /* Some hosted-agent sessions omit ANSWER_FINISH.  Once the PCM
             * queue has been empty for the idle window, it is safe to admit a
             * new push-to-talk turn instead of leaving the UI permanently
             * locked in ANSWERING. */
            if (voice_state == VOICE_STATE_ANSWERING && !conversation_recording) {
                cloud_agent_answering = false;
                voice_state = VOICE_STATE_IDLE;
                ESP_LOGW(TAG, "metric: session=%" PRIu32 " answer_finish_fallback_elapsed_ms=%" PRIu32,
                         voice_timing.session_id, voice_elapsed_ms());
                set_status_text("回复结束，点击开始可继续对话");
            }
        }
    }
}

static void stop_cloud_audio_playback(void)
{
    cloud_audio_frame_t stop_frame = {0};
    EventBits_t bits;

    cloud_playback_suppressed = true;
    if (cloud_audio_queue != NULL) {
        xQueueReset(cloud_audio_queue);
        (void)xQueueSend(cloud_audio_queue, &stop_frame, 0);
    }
    if (audio_event_group != NULL) {
        bits = xEventGroupWaitBits(audio_event_group, AUDIO_PLAYBACK_IDLE_BIT,
                                   pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
        if ((bits & AUDIO_PLAYBACK_IDLE_BIT) == 0) {
            ESP_LOGW(TAG, "audio: timed out waiting for speaker-to-microphone handoff");
        }
    }
}

/* Hardware-only diagnostic retained from the original audio bring-up.  It
 * deliberately bypasses RTC, ASR, TTS, and every camera path: three seconds
 * of board-microphone PCM are stored in PSRAM, then written unchanged to the
 * board speaker path. */
static void local_loopback_task(void *arg)
{
    uint8_t *recording = NULL;
    size_t captured = 0;
    esp_err_t result = ESP_OK;
    esp_codec_dev_sample_info_t format = {
        .sample_rate = LOCAL_LOOP_SAMPLE_RATE_HZ,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };

    (void)arg;
    local_loop_active = true;
    stop_cloud_audio_playback();
    set_status_text("本地录音回放：正在录音 3 秒...");
    recording = heap_caps_malloc(LOCAL_LOOP_BUFFER_BYTES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (recording == NULL) {
        result = ESP_ERR_NO_MEM;
        goto finished;
    }
    result = esp_codec_dev_open(microphone, &format);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "local-loop: microphone open failed: %s", esp_err_to_name(result));
        goto finished;
    }
    while (captured < LOCAL_LOOP_BUFFER_BYTES) {
        const size_t chunk = MIN((size_t)1024, LOCAL_LOOP_BUFFER_BYTES - captured);
        result = esp_codec_dev_read(microphone, recording + captured, (int)chunk);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "local-loop: microphone read failed at %u: %s",
                     (unsigned)captured, esp_err_to_name(result));
            break;
        }
        captured += chunk;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_close(microphone));
    if (result != ESP_OK || captured != LOCAL_LOOP_BUFFER_BYTES) {
        goto finished;
    }

    set_status_text("本地录音回放：正在通过扬声器播放...");
    result = esp_codec_dev_open(speaker, &format);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "local-loop: speaker open failed: %s", esp_err_to_name(result));
        goto finished;
    }
    for (size_t offset = 0; offset < captured; offset += 1024) {
        const size_t chunk = MIN((size_t)1024, captured - offset);
        result = esp_codec_dev_write(speaker, recording + offset, (int)chunk);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "local-loop: speaker write failed at %u: %s",
                     (unsigned)offset, esp_err_to_name(result));
            break;
        }
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_close(speaker));

finished:
    if (recording != NULL) {
        heap_caps_free(recording);
    }
    cloud_playback_suppressed = !cloud_connected;
    local_loop_active = false;
    if (result == ESP_OK && captured == LOCAL_LOOP_BUFFER_BYTES) {
        ESP_LOGI(TAG, "local-loop: record and playback completed (%u bytes)",
                 (unsigned)captured);
        set_status_text("本地录音回放完成：请确认是否清晰听到刚才的话");
    } else {
        set_status_text("本地录音回放失败，请查看 COM20 日志");
    }
    vTaskDelete(NULL);
}

static void local_loopback_button_event_callback(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (local_loop_active || conversation_recording || voice_state == VOICE_STATE_ANSWERING) {
        set_status_text("当前音频任务未结束，请稍后再测试本地录音回放");
        return;
    }
    if (xTaskCreate(local_loopback_task, "local_loopback", LOCAL_LOOP_TASK_STACK_BYTES,
                    NULL, 4, NULL) != pdPASS) {
        set_status_text("本地录音回放任务启动失败");
        ESP_LOGE(TAG, "local-loop: task creation failed");
    }
}

/* RTC callbacks run on a vendor-owned task.  Teardown is deliberately moved
 * to this task so the SDK never destroys the callback's own transport thread. */
static void cloud_recovery_task(void *arg)
{
    volc_engine_t old_engine = NULL;

    (void)arg;
    ESP_LOGI(TAG, "metric: cloud_reconnect_start_ms=%" PRIu32, esp_log_timestamp());
    stop_cloud_audio_playback();
    abort_voice_session("网络重连中，请稍候...");

    if (cloud_engine_lock != NULL &&
        xSemaphoreTake(cloud_engine_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
        old_engine = cloud_engine;
        cloud_engine = NULL;
        cloud_connected = false;
        cloud_start_requested = false;
        xSemaphoreGive(cloud_engine_lock);
    } else {
        ESP_LOGE(TAG, "cloud: recovery could not obtain engine lifecycle lock");
    }
    if (old_engine != NULL) {
        (void)volc_stop(old_engine);
        volc_destroy(old_engine);
        ESP_LOGI(TAG, "cloud: previous RTC session destroyed for recovery");
    }

    while (!wifi_link_up) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    vTaskDelay(pdMS_TO_TICKS(CLOUD_RECONNECT_DELAY_MILLISECONDS));
    cloud_playback_suppressed = false;
    cloud_recovery_in_progress = false;
    start_cloud_registration();
    vTaskDelete(NULL);
}

static void request_cloud_recovery(const char *reason)
{
    if (voice_ui_suspended()) {
        return;
    }
    if (cloud_recovery_in_progress) {
        return;
    }
    cloud_recovery_in_progress = true;
    voice_state = VOICE_STATE_RECOVERING;
    ESP_LOGW(TAG, "cloud: recovery requested (%s)", reason == NULL ? "unknown" : reason);
    if (xTaskCreate(cloud_recovery_task, "cloud_recovery", CLOUD_RECOVERY_TASK_STACK_BYTES,
                    NULL, 5, NULL) != pdPASS) {
        cloud_recovery_in_progress = false;
        ESP_LOGE(TAG, "cloud: unable to create recovery task");
        set_status_text("自动重连任务启动失败，请稍后重试");
    }
}

static void show_subtitle_text(const cJSON *root)
{
    cJSON *items;
    cJSON *item;
    cJSON *text;
    const char *value;

    if (!cJSON_IsObject(root) || !cJSON_IsString(cJSON_GetObjectItem(root, "type")) ||
        strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "type")), "subtitle") != 0) {
        return;
    }
    items = cJSON_GetObjectItem(root, "data");
    cJSON_ArrayForEach(item, items) {
        text = cJSON_GetObjectItem(item, "text");
        value = cJSON_GetStringValue(text);
        if (value == NULL || value[0] == '\0') {
            continue;
        }
        /* Role metadata wins over advisory ANSWERING timing. Never persist
         * sender identifiers, raw JSON, keys or tokens into chat history. */
        const char *role_name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "role"));
        const char *sender = cJSON_GetStringValue(cJSON_GetObjectItem(item, "userId"));
        if (!sender) sender = cJSON_GetStringValue(cJSON_GetObjectItem(item, "user_id"));
        chat_role_t role = CHAT_UNKNOWN;
        if (role_name && strcmp(role_name, "user") == 0) role = CHAT_USER;
        else if (role_name && (strcmp(role_name, "assistant") == 0 || strcmp(role_name, "bot") == 0))
            role = CHAT_ASSISTANT;
        else if (sender && sender[0] && strlen(sender) < sizeof(chat_user_sender)) {
            if (chat_assistant_sender[0] && strcmp(sender, chat_assistant_sender) == 0) role = CHAT_ASSISTANT;
            else if (chat_user_sender[0] && strcmp(sender, chat_user_sender) == 0) role = CHAT_USER;
            else if (!chat_user_has_spoken) {
                snprintf(chat_assistant_sender, sizeof(chat_assistant_sender), "%s", sender);
                role = CHAT_ASSISTANT;
            } else if (conversation_recording && !conversation_finish_requested) {
                snprintf(chat_user_sender, sizeof(chat_user_sender), "%s", sender);
                role = CHAT_USER;
            }
        } else if (!chat_user_has_spoken || cloud_agent_answering || cloud_reply_audio_started) role = CHAT_ASSISTANT;
        else if (conversation_recording && !conversation_finish_requested) role = CHAT_USER;
        const cJSON *sequence_item = cJSON_GetObjectItem(item, "sequence");
        int sequence = cJSON_IsNumber(sequence_item) && sequence_item->valuedouble >= 0 &&
                       sequence_item->valuedouble <= INT32_MAX ? sequence_item->valueint : -1;
        chat_history_update(role, chat_history_current_turn(), value, sequence);
        ESP_LOGI(TAG, "chat: subtitle role=%u sender_field=%d sequence_field=%d", role,
                 sender != NULL, sequence >= 0);
        if (!cloud_agent_answering) {
            if (!voice_timing.first_asr_logged &&
                (voice_state == VOICE_STATE_RECORDING || voice_state == VOICE_STATE_FINISHING ||
                 voice_state == VOICE_STATE_THINKING)) {
                voice_timing.first_asr_logged = true;
                ESP_LOGI(TAG, "metric: session=%" PRIu32 " first_asr_elapsed_ms=%" PRIu32,
                         voice_timing.session_id, voice_elapsed_ms());
            }
            snprintf(cloud_screen_text, sizeof(cloud_screen_text), "%s", value);
            set_conversation_text(user_text_label, cloud_screen_text);
        } else {
            hold_agent_reply_text(value);
            if (!voice_timing.first_reply_text_logged && cloud_reply_audio_started) {
                voice_timing.first_reply_text_logged = true;
                ESP_LOGI(TAG, "metric: session=%" PRIu32 " first_reply_text_elapsed_ms=%" PRIu32,
                         voice_timing.session_id, voice_elapsed_ms());
            }
            if (cloud_reply_audio_started) {
                set_conversation_text(agent_text_label, value);
            }
        }
    }
}

static void cloud_message_callback(volc_engine_t handle, const void *data, size_t length,
                                   volc_message_info_t *info, void *user_data)
{
    if (voice_ui_suspended()) {
        return;
    }
    char magic[5] = "----";
    cJSON *root;
    (void)handle;
    (void)info;
    (void)user_data;
    /* Vendor framing is: "subv" + 4-byte length + JSON UTF-8 payload. */
    if (data != NULL && length >= 4) {
        const uint8_t *bytes = data;
        for (size_t index = 0; index < 4; ++index) {
            magic[index] = (bytes[index] >= 0x20 && bytes[index] <= 0x7e) ? (char)bytes[index] : '?';
        }
    }
    if (data == NULL || length <= 8 || length - 8 > CLOUD_MESSAGE_MAX_BYTES ||
        memcmp(data, "subv", 4) != 0) {
        ESP_LOGW(TAG, "cloud: ignored message (length=%u, magic=%s)",
                 (unsigned)length, magic);
        return;
    }
    memcpy(cloud_message_buffer, (const uint8_t *)data + 8, length - 8);
    cloud_message_buffer[length - 8] = '\0';
    root = cJSON_Parse(cloud_message_buffer);
    if (root != NULL) {
        cJSON *type = cJSON_GetObjectItem(root, "type");
        ESP_LOGI(TAG, "cloud: subtitle message received (length=%u, type=%s)",
                 (unsigned)length, cJSON_IsString(type) ? cJSON_GetStringValue(type) : "missing");
        show_subtitle_text(root);
        cJSON_Delete(root);
    } else {
        ESP_LOGW(TAG, "cloud: subtitle message JSON parse failed (length=%u)", (unsigned)length);
    }
}

static bool wait_for_network_time(void)
{
    time_t now = 0;
    struct tm time_info = {0};

    /* POSIX CST-8 represents China Standard Time (UTC+8). */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* A cloud-session recovery does not invalidate system time. Calling the
     * SNTP configuration APIs while their client is already running triggers
     * an ESP-IDF assertion, so reuse a previously synchronized clock. */
    time(&now);
    localtime_r(&now, &time_info);
    if (time_info.tm_year >= (2025 - 1900)) {
        ESP_LOGI(TAG, "network: reusing synchronized clock for RTC recovery");
        return true;
    }

    /* A timed-out sync leaves the client running even while the clock is
     * still invalid. Reuse it on retry instead of reconfiguring a live client. */
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "cn.pool.ntp.org");
        esp_sntp_setservername(1, "ntp1.aliyun.com");
        esp_sntp_init();
    }
    for (int retry = 0; retry < 15; ++retry) {
        time(&now);
        localtime_r(&now, &time_info);
        if (time_info.tm_year >= (2025 - 1900)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

/* This deliberately checks only DNS plus a TCP three-way handshake.  It does
 * not send an HTTP request, perform cloud authentication, or emit an address.
 * It isolates LAN/C6 reachability from the vendor RTC library's TLS layer. */
static bool probe_rtc_tcp_endpoint(const char *hostname)
{
    struct addrinfo hints = {0};
    struct addrinfo *address = NULL;
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    fd_set writable;
    int socket_fd = -1;
    int original_flags;
    int socket_error = 0;
    socklen_t socket_error_length = sizeof(socket_error);
    int result;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    result = getaddrinfo(hostname, "443", &hints, &address);
    if (result != 0 || address == NULL) {
        ESP_LOGW(TAG, "rtc-probe: DNS failed for %s (code=%d)", hostname, result);
        return false;
    }
    socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_fd < 0) {
        ESP_LOGW(TAG, "rtc-probe: socket allocation failed for %s", hostname);
        freeaddrinfo(address);
        return false;
    }
    original_flags = fcntl(socket_fd, F_GETFL, 0);
    if (original_flags < 0 || fcntl(socket_fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "rtc-probe: cannot configure nonblocking socket for %s", hostname);
        close(socket_fd);
        freeaddrinfo(address);
        return false;
    }
    result = connect(socket_fd, address->ai_addr, address->ai_addrlen);
    if (result < 0 && errno == EINPROGRESS) {
        FD_ZERO(&writable);
        FD_SET(socket_fd, &writable);
        result = select(socket_fd + 1, NULL, &writable, NULL, &timeout);
        if (result > 0 && FD_ISSET(socket_fd, &writable)) {
            result = getsockopt(socket_fd, SOL_SOCKET, SO_ERROR,
                                &socket_error, &socket_error_length);
            result = (result == 0 && socket_error == 0) ? 0 : -1;
        } else {
            result = -1;
        }
    }
    (void)fcntl(socket_fd, F_SETFL, original_flags);
    close(socket_fd);
    freeaddrinfo(address);
    if (result == 0) {
        ESP_LOGI(TAG, "rtc-probe: TCP/443 reachable for %s", hostname);
        return true;
    }
    ESP_LOGW(TAG, "rtc-probe: TCP/443 unavailable for %s", hostname);
    return false;
}

static void run_rtc_network_probe(void)
{
    static const char *const endpoints[] = {
        "access.rtc.volcvideo.com",
        "access2-hl.rtc.volcvideo.com",
        "rtcg-access.volcvideos.com",
    };

    set_status_text("正在检查云端语音网络连接...");
    for (size_t index = 0; index < sizeof(endpoints) / sizeof(endpoints[0]); ++index) {
        (void)probe_rtc_tcp_endpoint(endpoints[index]);
    }
}

static void cloud_registration_task(void *arg)
{
    char config[1024];
    int config_length;
    int result;
    volc_event_handler_t handlers = {
        .on_volc_event = cloud_event_callback,
        .on_volc_conversation_status = cloud_conversation_status_callback,
        .on_volc_audio_data = cloud_audio_callback,
        .on_volc_video_data = NULL,
        .on_volc_message_data = cloud_message_callback,
    };
    volc_opt_t options = {
        .mode = VOLC_MODE_RTC,
        .bot_id = VOLC_BOT_ID,
        .params = NULL,
    };

    (void)arg;
    if (!wifi_link_up) {
        goto finished;
    }
    set_status_text("Wi-Fi 已连接，正在同步时间以登录云端...");
    if (!wait_for_network_time()) {
        ESP_LOGE(TAG, "cloud: SNTP time synchronization failed");
        set_status_text("云端连接暂停：网络时间同步失败");
        goto finished;
    }
    run_rtc_network_probe();
    atomic_store(&startup_network_prepared, true);
    config_length = snprintf(config, sizeof(config), VOLC_RTC_CONFIG_FORMAT,
                             VOLC_INSTANCE_ID, VOLC_PRODUCT_KEY,
                             VOLC_PRODUCT_SECRET, VOLC_DEVICE_NAME);
    if (config_length < 0 || config_length >= (int)sizeof(config)) {
        ESP_LOGE(TAG, "cloud: local configuration is too large");
        set_status_text("云端连接失败：本地配置无效");
        goto finished;
    }

    set_status_text("正在向云端语音服务注册此设备...");
    /* Network preparation may run behind startup UI, but starting the agent
     * can immediately emit its welcome. Wait without holding lifecycle/UI
     * locks until home has had a display cycle to become visible. */
    while (!atomic_load(&startup_home_visible)) {
        if (!wifi_link_up) goto finished;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (cloud_engine_lock == NULL ||
        xSemaphoreTake(cloud_engine_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "cloud: registration could not obtain engine lifecycle lock");
        set_status_text("云端连接繁忙，正在重试...");
        goto finished;
    }
    if (!wifi_link_up || cloud_engine != NULL) {
        xSemaphoreGive(cloud_engine_lock);
        goto finished;
    }
    result = volc_create(&cloud_engine, config, &handlers, NULL);
    if (result != VOLC_ERR_NO_ERROR) {
        xSemaphoreGive(cloud_engine_lock);
        ESP_LOGE(TAG, "cloud: device registration failed, error=%d", result);
        set_status_text("云端注册失败，请查看 COM20 日志");
        goto finished;
    }
    result = volc_start(cloud_engine, &options);
    if (result != VOLC_ERR_NO_ERROR) {
        ESP_LOGE(TAG, "cloud: RTC session start failed, error=%d", result);
        volc_destroy(cloud_engine);
        cloud_engine = NULL;
        xSemaphoreGive(cloud_engine_lock);
        set_status_text("云端 RTC 启动失败，请查看 COM20 日志");
        goto finished;
    }
    xSemaphoreGive(cloud_engine_lock);
    ESP_LOGI(TAG, "cloud: device registered and RTC session requested");
    set_status_text("云端注册完成，正在建立安全语音连接...");
finished:
    cloud_start_requested = false;
    vTaskDelete(NULL);
}

static void start_cloud_registration(void)
{
    if (voice_ui_suspended() || cloud_start_requested || cloud_recovery_in_progress || !wifi_link_up || cloud_engine != NULL) {
        return;
    }
    cloud_start_requested = true;
    if (xTaskCreate(cloud_registration_task, "cloud_register", CLOUD_TASK_STACK_BYTES,
                    NULL, 5, NULL) != pdPASS) {
        cloud_start_requested = false;
        ESP_LOGE(TAG, "cloud: unable to create registration task");
        set_status_text("云端连接失败：无法启动任务");
    }
}

/* Read-only compatibility probe.  This does not write to, reboot, or alter
 * the C6 co-processor; it only asks the official ESP-Hosted RPC for the
 * running slave firmware version. */
static void report_coprocessor_firmware_version(void)
{
    esp_hosted_coprocessor_fwver_t version = {0};
    esp_err_t result = esp_hosted_get_coprocessor_fwversion(&version);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "hosted: C6 firmware version=%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 version.major1, version.minor1, version.patch1);
    } else {
        ESP_LOGW(TAG, "hosted: C6 firmware version RPC unavailable (error=%s)",
                 esp_err_to_name(result));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* The settings worker owns association, scan and switch retries. */
        wifi_settings_event(event_id, false);
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        ESP_LOGW(TAG, "network_disconnect reason=%u", disconnected ? (unsigned)disconnected->reason : 0);
        wifi_settings_event(event_id, false);
        wifi_link_up = false;
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        voice_state = VOICE_STATE_RECOVERING;
        abort_voice_session("Wi-Fi 已断开，正在自动重连...");
        request_cloud_recovery("wifi_disconnected");
        ++wifi_retry_count;
        if (wifi_retry_count > WIFI_MAXIMUM_RETRIES) {
            wifi_retry_count = 1;
            ESP_LOGW(TAG, "network: reconnect attempts continue after backoff");
        }
        ESP_LOGW(TAG, "network: disconnected; reconnecting (%d/%d)",
                 wifi_retry_count, WIFI_MAXIMUM_RETRIES);
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        (void)event_data;
        wifi_settings_event(event_id, true);
        wifi_retry_count = 0;
        wifi_link_up = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "network: connected to Wi-Fi; address omitted");
        set_status_text("Wi-Fi 已连接，正在准备云端语音服务...");
        report_coprocessor_firmware_version();
        start_font_synchronization();
        if (!cloud_recovery_in_progress) {
            start_cloud_registration();
        }
    }
}

static void start_wifi_station(void)
{
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t station_config = {
        .sta = {
            .ssid = P4X_WIFI_SSID,
            .password = P4X_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_err_t result = nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    wifi_settings_load(&station_config);
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "network: unable to create Wi-Fi event group");
        set_status_text("Wi-Fi 初始化失败，请查看 COM20 日志");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    /* Failed candidates must never replace the last successful NVS record. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(wifi_settings_start());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "network: ESP-Hosted station started (SSID is intentionally not logged)");
}

/* RTC audio tracks are continuous in the vendor high-quality example.  Keep
 * the publish cadence alive with 20 ms zero-PCM frames while push-to-talk is
 * idle; the capture task becomes the sole sender while real speech is active.
 * This also gives the cloud VAD unbounded post-speech silence without keeping
 * the physical microphone open. */
static void cloud_idle_audio_uplink_task(void *arg)
{
    const uint8_t silence_frame[AUDIO_FRAME_BYTES] = {0};
    volc_audio_frame_info_t info = {
        .data_type = VOLC_AUDIO_DATA_TYPE_PCM,
        .commit = false,
    };
    TickType_t next_wake = xTaskGetTickCount();
    unsigned failed_count = 0;

    (void)arg;
    for (;;) {
        /* Camera mode keeps the same RTC session alive using silence only.
         * It never opens the microphone or publishes camera video. */
        if (cloud_connected && !conversation_recording) {
            int result = send_cloud_audio_frame(silence_frame, sizeof(silence_frame), &info);
            if (result != VOLC_ERR_NO_ERROR) {
                ++failed_count;
                if (failed_count == 1 || failed_count % 100 == 0) {
                    ESP_LOGW(TAG, "audio: idle RTC silence upload failed (count=%u, error=%d)",
                             failed_count, result);
                }
            }
        }
        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(AUDIO_FRAME_MILLISECONDS));
    }
}

static void conversation_capture_task(void *arg)
{
    uint8_t frame[AUDIO_FRAME_BYTES];
    const uint8_t silence_frame[AUDIO_FRAME_BYTES] = {0};
    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_SAMPLE_RATE_HZ,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };
    volc_audio_frame_info_t info = {
        .data_type = VOLC_AUDIO_DATA_TYPE_PCM,
        .commit = false,
    };
    esp_err_t result;
    int send_result;
    unsigned frame_count = 0;
    unsigned failed_frame_count = 0;
    unsigned tail_frames_remaining = 0;
    uint64_t absolute_sample_sum = 0;
    uint64_t sample_count = 0;
    unsigned nonzero_sample_count = 0;
    unsigned peak_absolute_sample = 0;
    bool microphone_read_failed = false;
    (void)arg;
    result = esp_codec_dev_open(microphone, &format);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "audio: microphone open failed: %s", esp_err_to_name(result));
        conversation_recording = false;
        conversation_finish_requested = false;
        set_status_text("麦克风异常，请点击开始后重试");
        vTaskDelete(NULL);
        return;
    }

    /* The RTC adapter relies on VAD even though the public API exposes a
     * commit flag.  After "结束", publish a short zero-PCM tail so cloud VAD
     * can close the utterance.  Do not keep sending live microphone audio for
     * seconds: the agent may already stream TTS while that tail is running,
     * and local playback is deliberately paused during capture to prevent
     * acoustic feedback. */
    while (cloud_connected && cloud_engine != NULL && conversation_recording) {
        result = esp_codec_dev_read(microphone, frame, sizeof(frame));
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "audio: microphone read failed: %s", esp_err_to_name(result));
            microphone_read_failed = true;
            break;
        }
        const int16_t *samples = (const int16_t *)frame;
        const size_t samples_in_frame = sizeof(frame) / sizeof(samples[0]);
        for (size_t sample_index = 0; sample_index < samples_in_frame; ++sample_index) {
            int32_t value = samples[sample_index];
            unsigned absolute_value = (unsigned)(value < 0 ? -value : value);
            absolute_sample_sum += absolute_value;
            ++sample_count;
            if (absolute_value != 0) {
                ++nonzero_sample_count;
            }
            if (absolute_value > peak_absolute_sample) {
                peak_absolute_sample = absolute_value;
            }
        }
        if (conversation_finish_requested && tail_frames_remaining == 0) {
            tail_frames_remaining = AUDIO_END_TAIL_FRAMES;
            ESP_LOGI(TAG, "audio: end requested; sending %u ms VAD tail",
                     AUDIO_END_TAIL_MILLISECONDS);
        }
        /* Retain commit=true on the final frame for API compatibility with
         * transports that support it, although RTC currently ignores it. */
        info.commit = (tail_frames_remaining == 1);
        send_result = send_cloud_audio_frame(tail_frames_remaining > 0 ? silence_frame : frame,
                                             sizeof(frame), &info);
        ++frame_count;
        if (send_result != VOLC_ERR_NO_ERROR) {
            ++failed_frame_count;
            ESP_LOGW(TAG, "audio: cloud upload failed (frame=%u, commit=%s, error=%d)",
                     frame_count, info.commit ? "yes" : "no", send_result);
        }
        if (tail_frames_remaining > 0) {
            --tail_frames_remaining;
        }
        if (conversation_finish_requested && tail_frames_remaining == 0) {
            ESP_LOGI(TAG, "audio: RTC VAD tail completed (frames=%u, bytes=%u, failed=%u)",
                     frame_count, frame_count * (unsigned)sizeof(frame), failed_frame_count);
            break;
        }
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_close(microphone));
    ESP_LOGI(TAG,
             "audio: capture levels peak=%u, mean_abs=%u, nonzero=%u/%u samples",
             peak_absolute_sample,
             sample_count == 0 ? 0U : (unsigned)(absolute_sample_sum / sample_count),
             nonzero_sample_count, (unsigned)sample_count);
    conversation_recording = false;
    conversation_finish_requested = false;
    cloud_playback_suppressed = !cloud_connected;
    if (cloud_connected && !microphone_read_failed && frame_count > 0) {
        voice_state = VOICE_STATE_THINKING;
    }
    if (microphone_read_failed || frame_count == 0) {
        set_status_text("麦克风录音失败，语音未发送；请点击开始后重试");
    }
    vTaskDelete(NULL);
}

static char typed_message[1501];

static void typed_message_task(void *argument)
{
    (void)argument;
    int result = -1;
    if (cloud_engine_lock && xSemaphoreTake(cloud_engine_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (cloud_connected && cloud_engine)
            result = cloud_text_send(cloud_engine, typed_message);
        xSemaphoreGive(cloud_engine_lock);
    }
    if (bsp_display_lock(0)) {
        if (result == VOLC_ERR_NO_ERROR) {
            chat_history_update(CHAT_USER, chat_history_current_turn(), typed_message, -1);
        } else {
            voice_state = cloud_connected ? VOICE_STATE_IDLE : VOICE_STATE_RECOVERING;
        }
        bsp_display_unlock();
    }
    memset(typed_message, 0, sizeof(typed_message));
    ESP_LOGI(TAG, "text_input: submit_result=%d", result);
    text_entry_finish(result == VOLC_ERR_NO_ERROR);
    vTaskDelete(NULL);
}

static bool submit_home_text(const char *text, void *context)
{
    (void)context;
    if (!cloud_connected || !cloud_engine || voice_state != VOICE_STATE_IDLE ||
        conversation_recording || local_loop_active || cloud_agent_answering || chat_history_loading()) {
        text_entry_error("会话未就绪或正在回复，请稍后重试");
        return false;
    }
    if (strlen(text) >= sizeof(typed_message)) {
        text_entry_error("文字过长，请缩短后发送");
        return false;
    }
    strcpy(typed_message, text);
    stop_cloud_audio_playback();
    cloud_playback_suppressed = false;
    cloud_agent_answering = false;
    cloud_reply_audio_started = false;
    clear_agent_reply_text();
    reset_voice_session_metrics();
    voice_state = VOICE_STATE_THINKING;
    chat_user_has_spoken = true;
    chat_history_begin_turn();
    if (xTaskCreate(typed_message_task, "text_submit", 8192, NULL, 4, NULL) != pdPASS) {
        voice_state = VOICE_STATE_IDLE;
        memset(typed_message, 0, sizeof(typed_message));
        text_entry_error("无法创建发送任务，请重试");
        return false;
    }
    return true;
}

static void home_input_layout(int height, void *context)
{
    (void)context;
    home_input_height=height;
    home_reading_layout();
}

static void open_home_text(lv_event_t *event)
{
    (void)event;
    if (conversation_recording || local_loop_active || chat_history_loading()) return;
    text_entry_toggle_language(event);
}

static void physical_keyboard_key(uint32_t key)
{
    if (screen_idle_keyboard_activity()) return;
    text_entry_key(key);
}

static void touch_event_callback(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point = {0};

    if (indev != NULL) {
        lv_indev_get_point(indev, &point);
    }
    ESP_LOGI(TAG, "touch: event=%d, x=%ld, y=%ld", (int)lv_event_get_code(event),
             (long)point.x, (long)point.y);

    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED && chat_history_loading()) {
        lv_label_set_text(status_label, "正在读取对话历史，请稍候");
        return;
    }
    if (local_loop_active) {
        lv_label_set_text(status_label, "本地录音回放进行中，请稍候");
        return;
    }
    if (!cloud_connected || cloud_engine == NULL || voice_state == VOICE_STATE_RECOVERING) {
        lv_label_set_text(status_label, "云端连接尚未就绪，请稍候");
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (voice_state == VOICE_STATE_RECORDING && conversation_recording && !conversation_finish_requested) {
            conversation_finish_requested = true;
            voice_state = VOICE_STATE_FINISHING;
            ESP_LOGI(TAG, "metric: session=%" PRIu32 " capture_release_elapsed_ms=%" PRIu32,
                     voice_timing.session_id, voice_elapsed_ms());
            lv_label_set_text(status_label, "语音已发送，正在理解你说的话...");
        }
        return;
    }

    /* A press only starts a new utterance.  Release ends that same utterance. */
    if (conversation_recording) {
        lv_label_set_text(status_label, "正在录音，请松开后发送");
        return;
    }
    const bool interrupting_remote_turn = voice_state == VOICE_STATE_ANSWERING ||
        voice_state == VOICE_STATE_THINKING || cloud_agent_answering;
    if (interrupting_remote_turn) {
        const int interrupt_result = interrupt_cloud_answer();
        ESP_LOGI(TAG, "voice: user interrupted stale/active remote turn (state=%d, result=%d)",
                 (int)voice_state, interrupt_result);
    } else if (voice_state != VOICE_STATE_IDLE) {
        /* Cloud conversation-state events are advisory and may remain in
         * THINKING after a completed turn.  A new physical press wins over
         * that stale state; the old local playback is still cleared below. */
        ESP_LOGW(TAG, "voice: user overrides stale state=%d", (int)voice_state);
    }
    stop_cloud_audio_playback();
    conversation_recording = true;
    conversation_finish_requested = false;
    voice_state = VOICE_STATE_RECORDING;
    cloud_agent_answering = false;
    cloud_reply_audio_started = false;
    clear_agent_reply_text();
    reset_voice_session_metrics();
    chat_user_has_spoken = true;
    chat_history_begin_turn();
    lv_label_set_text(status_label, "正在聆听，请按住说话，松开后发送");
    if (xTaskCreate(conversation_capture_task, "cloud_capture", AUDIO_TASK_STACK_BYTES, NULL, 5, NULL) != pdPASS) {
        conversation_recording = false;
        conversation_finish_requested = false;
        lv_label_set_text(status_label, "麦克风任务未能启动，请点击开始后重试");
        ESP_LOGE(TAG, "audio: cloud capture task creation failed");
    }
}

#if CONFIG_P4_SIGN_LANGUAGE_MODE || CONFIG_P4_ASR_MODE
/* This handoff runs on the camera worker, never inside LVGL or an RTC callback. */
static esp_err_t prepare_sign_language_mode(void)
{
    stop_cloud_audio_playback();
    if (audio_event_group == NULL ||
        (xEventGroupGetBits(audio_event_group) & AUDIO_PLAYBACK_IDLE_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (cloud_engine_lock == NULL ||
        xSemaphoreTake(cloud_engine_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* End a stale/current answer without destroying the retained RTC session.
     * Local audio and subtitles remain suppressed by the camera-mode guards. */
    int interrupt_result = VOLC_ERR_NO_ERROR;
    if (cloud_connected && cloud_engine != NULL) {
        interrupt_result = volc_interrupt(cloud_engine);
    }
    xSemaphoreGive(cloud_engine_lock);
    if (interrupt_result != VOLC_ERR_NO_ERROR) {
        ESP_LOGW(TAG, "mode: retained RTC interrupt returned error=%d", interrupt_result);
    }
    ESP_LOGI(TAG, "mode: local voice paused; RTC session retained for camera preview");
    return ESP_OK;
}

static void resume_voice_after_sign_language(void)
{
    /* Camera worker calls this only after its DMA/ISP resources are released. */
    cloud_agent_answering = false;
    cloud_reply_audio_started = false;
    const bool session_retained = wifi_link_up && cloud_connected && cloud_engine != NULL;
    cloud_playback_suppressed = !session_retained;
    voice_state = session_retained ? VOICE_STATE_IDLE : VOICE_STATE_RECOVERING;
    set_status_text(session_retained ? "网络和音频已就绪：请按住下方按钮说话" :
                                      "已返回语音，网络已断开，正在恢复...");
    if (!session_retained) {
        /* Recovery is reserved for an actual connection loss, not mode exit. */
        if (cloud_engine != NULL) {
            request_cloud_recovery("camera_session_disconnected");
        } else {
            start_cloud_registration();
        }
    }
    ESP_LOGI(TAG, "mode: voice home resumed; retained_session=%d", (int)session_retained);
    if (settings_after_camera) {
        settings_after_camera = false;
        settings_button_callback(NULL);
    }
}
#endif

#if CONFIG_P4_SIGN_LANGUAGE_MODE
static void sign_language_button_callback(lv_event_t *event)
{
    (void)event;
    if (sign_language_mode_active()) {
        return;
    }
    /* Cloud turn states are advisory and can outlive the local conversation.
     * An open speaker can also be receiving idle PCM after the welcome line.
     * Neither prevents an explicit mode change: the worker stops playback and
     * verifies the hardware handoff before it initializes CSI/ISP. Only actual
     * capture or lifecycle work must finish before the mode can change. */
    if (audio_event_group == NULL || cloud_engine_lock == NULL ||
        conversation_recording || conversation_finish_requested || local_loop_active ||
        cloud_start_requested || cloud_recovery_in_progress || font_sync_in_progress) {
        ESP_LOGI(TAG, "mode: entry deferred capture=%d finish=%d loop=%d register=%d recovery=%d font=%d ready=%d",
                 (int)conversation_recording, (int)conversation_finish_requested,
                 (int)local_loop_active, (int)cloud_start_requested,
                 (int)cloud_recovery_in_progress, (int)font_sync_in_progress,
                 (int)(audio_event_group != NULL && cloud_engine_lock != NULL));
        lv_label_set_text(status_label, "请等待当前语音任务结束，再进入手语翻译");
        return;
    }
    ESP_LOGI(TAG, "mode: entry accepted; advisory_voice_state=%d speaker_idle=%d",
             (int)voice_state,
             (int)((xEventGroupGetBits(audio_event_group) & AUDIO_PLAYBACK_IDLE_BIT) != 0));
    const lv_font_t *font = project_ui_font();
    const esp_err_t err = sign_language_mode_start(font, prepare_sign_language_mode,
                                                  resume_voice_after_sign_language);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode: camera entry failed (%s)", esp_err_to_name(err));
    }
}
#endif

#if CONFIG_P4_ASR_MODE
static esp_err_t prepare_speech_to_text_recording(void)
{
    /* Local typing is independent of RTC registration; audio handoff is not. */
    if (!wifi_link_up) return ESP_ERR_WIFI_NOT_CONNECT;
    if (cloud_start_requested || cloud_recovery_in_progress || conversation_recording ||
        conversation_finish_requested || local_loop_active || font_sync_in_progress)
        return ESP_ERR_INVALID_STATE;
    return prepare_sign_language_mode();
}

static void speech_to_text_button_callback(lv_event_t *event)
{
    (void)event;
    if (voice_ui_suspended()) return;
    ESP_LOGI(TAG, "asr_entry wifi=%d mic=%d audio=%d lock=%d capture=%d finish=%d loop=%d register=%d recovery=%d font=%d",
             (int)wifi_link_up, microphone != NULL, audio_event_group != NULL,
             cloud_engine_lock != NULL, (int)conversation_recording, (int)conversation_finish_requested,
             (int)local_loop_active, (int)cloud_start_requested, (int)cloud_recovery_in_progress,
             (int)font_sync_in_progress);
    if (microphone == NULL || audio_event_group == NULL ||
        cloud_engine_lock == NULL || conversation_recording || conversation_finish_requested ||
        local_loop_active || font_sync_in_progress) {
        lv_label_set_text(status_label, "当前录音或资源初始化未结束，请稍后进入");
        return;
    }
    const lv_font_t *font = project_ui_font();
    const esp_err_t error = speech_to_text_mode_enter(microphone, font,
        prepare_speech_to_text_recording, resume_voice_after_sign_language);
    if (error != ESP_OK) {
        lv_label_set_text(status_label, "语音转文字页面启动失败，请重试");
        ESP_LOGW(TAG, "ASR mode entry failed error=%d", (int)error);
    }
}
#endif

/* Explicit local styles avoid the ASR button's brighter default theme/shadow.
 * All three controls keep their existing geometry, events and mode behavior. */
static void style_home_button(lv_obj_t *object, uint32_t color)
{
    lv_obj_remove_style_all(object);
    qh_button(object, color);
}

/* Settings navigation only: retain the home objects and the current voice
 * session. Lazy construction avoids extra SD glyph reads during startup. */
static lv_obj_t *settings_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, settings_cached_font(project_ui_font()), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, qh_color(color), LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

/* One versioned blob keeps brightness and volume together; volume zero also
 * represents mute. NVS owns integrity checking; validate schema and ranges. */
static uint8_t settings_saved_values[3] = {1, 70, 45};
static bool settings_have_saved_values;
static uint8_t settings_saved_subtitle_size;
static bool settings_have_saved_subtitle_size;
static unsigned settings_saved_timeout;
static bool settings_have_saved_timeout;
static bool settings_saved_dark, settings_have_saved_dark;

static void settings_save_task(void *argument)
{
    (void)argument;
    uint8_t notification;
    for (;;) {
        if (xQueueReceive(settings_save_queue, &notification, portMAX_DELAY) != pdTRUE) continue;
        /* Coalesce slider changes without blocking LVGL or audio playback. */
        while (xQueueReceive(settings_save_queue, &notification, pdMS_TO_TICKS(800)) == pdTRUE) {}
        /* Sample after debounce: competing UI/audio producers must not queue
         * an older snapshot after a newer one and persist stale values. */
        const uint8_t values[3] = {1, (uint8_t)atomic_load(&settings_brightness),
                                    (uint8_t)atomic_load(&settings_volume_applied)};
        const uint8_t subtitle_size = atomic_load(&settings_subtitle_size);
        const unsigned timeout = screen_idle_get_timeout();
        const bool dark=qh_theme_dark();
        if (settings_have_saved_values && memcmp(values, settings_saved_values, sizeof(values)) == 0 &&
            settings_have_saved_subtitle_size && subtitle_size == settings_saved_subtitle_size &&
            settings_have_saved_timeout && timeout == settings_saved_timeout &&
            settings_have_saved_dark && dark == settings_saved_dark) {
            if (uxQueueMessagesWaiting(settings_save_queue) == 0) atomic_store(&settings_save_status, 2);
            continue;
        }
        esp_err_t error = nvs_set_blob(settings_nvs, "av_v1", values, sizeof(values));
        if (error == ESP_OK) error = nvs_set_u8(settings_nvs, "ui_font_v1", subtitle_size);
        if (error == ESP_OK) error = nvs_set_u16(settings_nvs, "sleep_s_v1", timeout);
        if (error == ESP_OK) error = nvs_set_u8(settings_nvs,"ui_dark_v1",dark?1:0);
        if (error == ESP_OK) error = nvs_commit(settings_nvs);
        if (error == ESP_OK) {
            memcpy(settings_saved_values, values, sizeof(values));
            settings_have_saved_values = true;
            settings_saved_subtitle_size = subtitle_size;
            settings_have_saved_subtitle_size = true;
            settings_saved_dark=dark;settings_have_saved_dark=true;
            settings_saved_timeout = timeout;
            settings_have_saved_timeout = true;
            if (uxQueueMessagesWaiting(settings_save_queue) == 0) atomic_store(&settings_save_status, 2);
        } else {
            atomic_store(&settings_save_status, 3);
            ESP_LOGW(TAG, "settings: save error=%d", (int)error);
        }
    }
}

static void settings_schedule_save(void)
{
    if (settings_save_queue == NULL) return;
    const uint8_t notification = 1;
    atomic_store(&settings_save_status, 1);
    xQueueOverwrite(settings_save_queue, &notification);
}

static void settings_restore_theme(void)
{
    qh_theme_restore(false);
    settings_have_saved_dark=false;
    uint8_t dark=0;
    esp_err_t error=nvs_get_u8(settings_nvs,"ui_dark_v1",&dark);
    if(error==ESP_OK && dark<=1){
        qh_theme_restore(dark!=0);settings_saved_dark=dark!=0;settings_have_saved_dark=true;
    }
    ESP_LOGI(TAG,"ui_theme dark=%d restored=%d",qh_theme_dark(),settings_have_saved_dark);
}

static void settings_storage_start(void)
{
    qh_theme_restore(false);
    qh_theme_set_save_callback(settings_schedule_save);
    /* Do not erase NVS on a settings error. Keep default runtime controls;
     * the existing Wi-Fi initialization/recovery path remains unchanged. */
    esp_err_t error = nvs_flash_init();
    if (error == ESP_OK) error = nvs_open("p4_settings", NVS_READWRITE, &settings_nvs);
    if (error != ESP_OK) {
        atomic_store(&settings_save_status, 3);
        ESP_LOGW(TAG, "settings: storage init error=%d", (int)error);
        return;
    }
    settings_restore_theme();
    uint8_t values[3];
    size_t length = sizeof(values);
    error = nvs_get_blob(settings_nvs, "av_v1", values, &length);
    if (error == ESP_OK && length == sizeof(values) && values[0] == 1 &&
        values[1] >= 10 && values[1] <= 100 && values[2] <= 100) {
        atomic_store(&settings_brightness, values[1]);
        atomic_store(&settings_volume_requested, values[2]);
        atomic_store(&settings_volume_applied, values[2]);
        memcpy(settings_saved_values, values, sizeof(values));
        settings_have_saved_values = true;
        atomic_store(&settings_save_status, 2);
        ESP_LOGI(TAG, "settings: saved display and volume restored");
    } else if (error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "settings: invalid saved values; using defaults (error=%d)", (int)error);
    }
    uint8_t subtitle_size = 0;
    /* The former subtitle-only choice does not override the new page defaults. */
    error = nvs_get_u8(settings_nvs, "ui_font_v1", &subtitle_size);
    if (error == ESP_OK && (subtitle_size == 0 || subtitle_size == 14 || subtitle_size == 28)) {
        atomic_store(&settings_subtitle_size, subtitle_size);
        settings_saved_subtitle_size = subtitle_size;
        settings_have_saved_subtitle_size = true;
    } else if (error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "settings: invalid subtitle size; keeping page defaults (error=%d)", (int)error);
    }
#if CONFIG_P4_ASR_MODE
    speech_to_text_mode_set_subtitle_size(atomic_load(&settings_subtitle_size));
#endif
    uint16_t timeout = 0;
    error = nvs_get_u16(settings_nvs, "sleep_s_v1", &timeout);
    const bool migrate_test_timeout = error == ESP_OK && timeout == 5;
    if (migrate_test_timeout) timeout = 30;
    if (error == ESP_OK && screen_idle_timeout_valid(timeout)) {
        screen_idle_set_timeout(timeout);
        /* Retain the old persisted value until the background worker commits
         * the replacement; its ordinary comparison will detect the change. */
        settings_saved_timeout = migrate_test_timeout ? 5 : timeout;
        settings_have_saved_timeout = true;
    }
    settings_save_queue = xQueueCreate(1, sizeof(uint8_t));
    if (settings_save_queue == NULL ||
        xTaskCreate(settings_save_task, "settings_save", 4096, NULL, 2, NULL) != pdPASS) {
        if (settings_save_queue != NULL) vQueueDelete(settings_save_queue);
        settings_save_queue = NULL;
        nvs_close(settings_nvs);
        atomic_store(&settings_save_status, 3);
        ESP_LOGW(TAG, "settings: storage worker unavailable");
    } else if (migrate_test_timeout) {
        settings_schedule_save();
    }
}

static void settings_return_callback(lv_event_t *event)
{
    (void)event;
    if (settings_clear_dialog) { lv_obj_delete(settings_clear_dialog); settings_clear_dialog = NULL; }
    atomic_store(&settings_test_enabled, false);
    const int test_status = atomic_load(&settings_test_status);
    if (test_status == 1 || test_status == 2) {
        /* Keep home recording/camera controls inaccessible until the playback
         * owner acknowledges cancellation and releases the output codec. */
        settings_return_pending = true;
        lv_label_set_text(settings_save_label, "正在返回首页...");
        return;
    }
    if (settings_home_screen != NULL) {
        qh_screen_load(settings_home_screen, true);
    }
}

static void settings_brightness_callback(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target_obj(event);
    const int value = (int)lv_slider_get_value(slider);
    const esp_err_t error = bsp_display_brightness_set(value);
    if (error == ESP_OK) {
        settings_brightness = value;
        lv_label_set_text_fmt(settings_brightness_value, "%d%%", value);
        settings_schedule_save();
    } else {
        lv_slider_set_value(slider, settings_brightness, LV_ANIM_OFF);
        lv_label_set_text(settings_brightness_value, "调节失败");
        ESP_LOGW(TAG, "settings: brightness error=%d", (int)error);
    }
}

static lv_obj_t *settings_slider(lv_obj_t *row, int minimum, int value,
                                 lv_event_cb_t callback)
{
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, 440, 18);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_slider_set_range(slider, minimum, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, qh_color(QH_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, qh_color(QH_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, qh_color(QH_INK), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 18);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, NULL);
    return slider;
}

static void settings_volume_callback(lv_event_t *event)
{
    const int value = (int)lv_slider_get_value(lv_event_get_target_obj(event));
    if (value != atomic_load(&settings_volume_requested)) settings_volume_changed = true;
    atomic_store(&settings_volume_error, 0);
    atomic_store(&settings_volume_requested, value);
    lv_label_set_text_fmt(settings_volume_value, "%d%%...", value);
}

static void settings_volume_release_callback(lv_event_t *event)
{
    (void)event;
    if (!settings_volume_changed) return;
    settings_volume_changed = false;
    if (atomic_load(&settings_volume_requested) == 0) return;
    const int status = atomic_load(&settings_test_status);
    if (settings_return_pending) return;
    if (status == 1 || status == 2) return;
    if (speaker == NULL || cloud_audio_queue == NULL || audio_event_group == NULL ||
        conversation_recording || conversation_finish_requested || local_loop_active ||
        feedback_voice_recent()) {
        atomic_store(&settings_test_status, 4);
        ESP_LOGI(TAG, "settings: feedback release skipped busy=%d", (int)feedback_voice_recent());
        return;
    }
    cloud_audio_frame_t command = {.length = SETTINGS_TEST_FRAME_LENGTH};
    settings_test_requested_at = lv_tick_get();
    ESP_LOGI(TAG, "settings: feedback requested on volume release");
    atomic_store(&settings_test_status, 1);
    if (xQueueSend(cloud_audio_queue, &command, 0) != pdTRUE) {
        atomic_store(&settings_test_status, 4);
    }
}

static void settings_value_text(lv_obj_t *label,const char *text)
{
    if(label && strcmp(lv_label_get_text(label),text))lv_label_set_text(label,text);
}

static void settings_volume_refresh(lv_timer_t *timer)
{
    (void)timer;
    if (lv_screen_active() != settings_screen) return;
    if (settings_history_value) {
        char text[64];snprintf(text,sizeof(text),"%u条 %s",chat_history_count(),chat_history_storage_status());
        settings_value_text(settings_history_value,text);
    }
    int test_status = atomic_load(&settings_test_status);
    /* Existing microphone/recovery handoffs may reset the PCM queue and
     * discard a queued command. Allow the next volume adjustment to notify. */
    if (test_status == 1 && lv_tick_elaps(settings_test_requested_at) >= 2000) {
        int expected = 1;
        if (atomic_compare_exchange_strong(&settings_test_status, &expected, 6)) test_status = 6;
        else test_status = expected;
    }
    if (settings_return_pending) {
        if (test_status != 1 && test_status != 2 &&
            (test_status == 3 || test_status == 4 || test_status == 6 ||
             (xEventGroupGetBits(audio_event_group) & AUDIO_PLAYBACK_IDLE_BIT) != 0)) {
            settings_return_pending = false;
            qh_screen_load(settings_home_screen, true);
        }
        return;
    }
    int save_status = atomic_load(&settings_save_status);
    if (save_status != 3 && atomic_load(&settings_volume_requested) != atomic_load(&settings_volume_applied)) {
        save_status = 1;
    }
    const char *save_text = save_status == 1 ? "正在保存设置..." :
                            save_status == 2 ? "设置已保存" :
                            save_status == 3 ? "设置保存失败" : "调节后自动保存";
    if (strcmp(lv_label_get_text(settings_save_label), save_text) != 0) {
        lv_label_set_text(settings_save_label, save_text);
    }
    const int requested = atomic_load(&settings_volume_requested);
    char volume_text[32];
    if (atomic_load(&settings_volume_error) != 0) {
        lv_slider_set_value(settings_volume_slider, requested, LV_ANIM_OFF);
        snprintf(volume_text,sizeof(volume_text),"调节失败");
    } else if (requested != atomic_load(&settings_volume_applied)) {
        snprintf(volume_text,sizeof(volume_text),"%d%%...",requested);
    } else if (requested == 0) {
        snprintf(volume_text,sizeof(volume_text),"静音");
    } else {
        snprintf(volume_text,sizeof(volume_text),"%d%%",requested);
    }
    settings_value_text(settings_volume_value,volume_text);
}

static void settings_relayout(void)
{
    const lv_font_t *font=settings_cached_font(project_ui_font());
    const int line=font->line_height;
    const int hit=line+12>56?line+12:56;
    for(unsigned i=0;i<8;++i){
        lv_obj_t *row=settings_rows[i];if(!row)continue;
        int height=hit+16;
        if(i==1 || i==2)height=line+UI_SETTINGS_SLIDER_EXTRA;
        if(i==3)height=line+hit+UI_SETTINGS_CHOICE_EXTRA;
        if(i==4)height=line+UI_SETTINGS_HISTORY_EXTRA;
        lv_obj_set_height(row,height);
        lv_obj_t *name=lv_obj_get_child(row,0),*value=lv_obj_get_child(row,1);
        lv_obj_set_width(name,246);
        if(i==1 || i==2 || i==3 || i==4){
            lv_obj_align(name,LV_ALIGN_TOP_LEFT,20,10);
            if(i!=4)lv_obj_align(value,LV_ALIGN_TOP_RIGHT,-20,10);
        }
        for(uint32_t c=2;c<lv_obj_get_child_count(row);++c){
            lv_obj_t *control=lv_obj_get_child(row,c);
            if(lv_obj_check_type(control,&lv_slider_class)) {
                lv_obj_align(control,LV_ALIGN_BOTTOM_MID,0,-26);
            } else lv_obj_set_height(control,hit);
        }
    }
}

static void settings_subtitle_refresh(void)
{
    const int size = atomic_load(&settings_subtitle_size);
    lv_label_set_text(settings_subtitle_value, size == 14 ? "14px" : size == 28 ? "28px" : "默认");
    for (unsigned i = 0; i < 2; ++i) {
        const bool selected = size == (i == 0 ? 14 : 28);
        lv_obj_set_style_bg_color(settings_subtitle_buttons[i],
            qh_color(selected ? QH_SOFT : QH_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_width(settings_subtitle_buttons[i],selected?2:1,0);
        lv_obj_set_style_border_color(settings_subtitle_buttons[i],qh_color(selected?QH_LINK:QH_LINE),0);
    }
}

static void settings_subtitle_callback(lv_event_t *event)
{
    const int size = (int)(intptr_t)lv_event_get_user_data(event);
#if CONFIG_P4_ASR_MODE
    if (size == 28 && speech_to_text_mode_font28() == NULL) {
        lv_label_set_text(settings_subtitle_value, "字库未就绪");
        return;
    }
#endif
    if (size == atomic_load(&settings_subtitle_size)) return;
    atomic_store(&settings_subtitle_size, size);
    apply_subtitle_font();
    settings_subtitle_refresh();
    settings_schedule_save();
}

static void settings_clear_cancel(lv_event_t *event)
{
    (void)event;
    if (settings_clear_dialog) lv_obj_delete(settings_clear_dialog);
    settings_clear_dialog = NULL;
}

static void settings_clear_confirm(lv_event_t *event)
{
    chat_history_request_clear();
    settings_clear_cancel(event);
}

static void settings_clear_history(lv_event_t *event)
{
    (void)event;
    if (settings_clear_dialog) return;
    settings_clear_dialog = lv_obj_create(settings_screen);
    lv_obj_remove_style_all(settings_clear_dialog);
    lv_obj_set_size(settings_clear_dialog, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_scrollable(settings_clear_dialog, false);
    lv_obj_set_style_bg_color(settings_clear_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(settings_clear_dialog, LV_OPA_50, 0);
    const lv_font_t *font = settings_cached_font(project_ui_font());
    const char *message = "清除全部对话历史？清除后无法恢复。";
    lv_point_t measured; lv_text_get_size(&measured, message, font, 0, 4, 480, LV_TEXT_FLAG_NONE);
    const int action_height = font->line_height + 16 > 70 ? font->line_height + 16 : 70;
    lv_obj_t *card = lv_obj_create(settings_clear_dialog);
    qh_panel(card, QH_SURFACE, 28); lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_size(card, 540, measured.y + action_height + 90); lv_obj_center(card);
    qh_reveal(card);
    lv_obj_t *prompt = settings_label(card, message, QH_INK);
    lv_obj_set_width(prompt, 480); lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(prompt, 4, 0);
    lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 24);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *button = lv_button_create(card);
        qh_button(button, i == 0 ? QH_SOFT : QH_DANGER);
        lv_obj_set_size(button, 220, action_height);
        lv_obj_align(button, LV_ALIGN_BOTTOM_MID, i == 0 ? -122 : 122, -24);
        lv_obj_t *label = settings_label(button, i == 0 ? "取消" : "确认清除", i == 0 ? QH_INK : QH_ON_ACCENT);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, i == 0 ? settings_clear_cancel : settings_clear_confirm,
                            LV_EVENT_CLICKED, NULL);
    }
}

static const unsigned screen_timeout_choices[] = {0, 30, 60, 120, 300};
static void settings_timeout_callback(lv_event_t *event)
{
    unsigned index = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (index >= sizeof(screen_timeout_choices) / sizeof(screen_timeout_choices[0])) return;
    screen_idle_set_timeout(screen_timeout_choices[index]);
    lv_display_trigger_activity(NULL);
    settings_schedule_save();
}
static void settings_about_callback(lv_event_t *event)
{
    (void)event;
    device_about_show(settings_screen, project_ui_font(), wifi_link_up, cloud_connected,
        atomic_load(&settings_brightness), atomic_load(&settings_volume_applied),
        atomic_load(&settings_subtitle_size));
}

static void settings_wifi_callback(lv_event_t *event)
{
    (void)event;
    wifi_settings_show(settings_screen, project_ui_font());
}

static lv_obj_t *help_screen, *help_parent_screen;
static void help_back(lv_event_t *event) { (void)event; qh_screen_load(help_parent_screen, true); }
static void settings_help_callback(lv_event_t *event)
{
    (void)event;
    help_parent_screen = lv_screen_active();
    if (!help_screen) {
        help_screen=lv_obj_create(NULL); qh_screen(help_screen);
        const lv_font_t *font=settings_cached_font(project_ui_font());
        qh_header(help_screen,"使用帮助",font,help_back,true);
        lv_obj_t *intro=qh_label(help_screen,font,"按你的方式交流，随时返回",QH_MUTED);
        lv_obj_set_pos(intro,24,86);lv_obj_set_width(intro,552);
        lv_obj_t *list=lv_obj_create(help_screen);lv_obj_remove_style_all(list);
        lv_obj_set_pos(list,20,150);lv_obj_set_size(list,560,850);
        lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(list,12,0);
        lv_obj_set_scroll_dir(list,LV_DIR_VER);
        const char *titles[]={"语音对话","独立转写","键盘输入","相机预览","息屏唤醒"};
        const char *actions[]={"按住说话，松开发送","上翻阅读，回到最新","点击键盘，拼/E切换","进入预览，返回退出","首次唤醒，再次操作"};
        const char *steps[]={
            "1. 按住下方按钮说话，松开后等待回复。\n2. 回复时按住可打断并继续说话。",
            "1. 按住说话，松开后保留文字。\n2. 上翻保留位置；清空前需确认。\n3. 录音结束后再返回或进入设置。",
            "1. 候选可横向滑动、翻页。\n2. USB键盘可直接输入，不自动展开软键盘。\n3. 提交失败时保留草稿。",
            "1. 当前提供取景预览。\n2. 点击返回退出，识别模型尚未接入。",
            "1. 首次触摸或按键只唤醒屏幕。\n2. 再次操作即可继续使用。"};
        const unsigned icons[]={QH_ICON_ROBOT,QH_ICON_DOCUMENT,QH_ICON_KEYBOARD,QH_ICON_CAMERA,QH_ICON_WAKE};
        for(unsigned i=0;i<5;++i){
            lv_obj_t *card=lv_obj_create(list);qh_panel(card,QH_SURFACE,20);
            lv_obj_set_size(card,552,LV_SIZE_CONTENT);lv_obj_set_scrollable(card,false);
            lv_obj_set_style_pad_all(card,16,0);lv_obj_set_style_pad_row(card,8,0);
            lv_obj_set_flex_flow(card,LV_FLEX_FLOW_COLUMN);
            lv_obj_t *row=lv_obj_create(card);lv_obj_remove_style_all(row);
            lv_obj_set_size(row,520,LV_SIZE_CONTENT);lv_obj_set_flex_flow(row,LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(row,12,0);lv_obj_set_scrollable(row,false);
            qh_icon(row,icons[i]);
            qh_label(row,font,titles[i],QH_INK);
            lv_obj_t *label=qh_label(card,font,actions[i],QH_LINK);
            lv_obj_set_width(label,520);lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
            label=qh_label(card,font,steps[i],QH_INK);
            lv_obj_set_width(label,520);lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(label,4,0);
        }
    }
    apply_global_font_tree(help_screen,settings_cached_font(project_ui_font()));
    qh_screen_load(help_screen,false);
}

static void settings_button_callback(lv_event_t *event)
{
    (void)event;
#if CONFIG_P4_SIGN_LANGUAGE_MODE
    if (sign_language_mode_active()) {
        settings_after_camera = true;
        sign_language_mode_request_return();
        return;
    }
#endif
    if (conversation_recording || conversation_finish_requested || local_loop_active) {
        lv_label_set_text(status_label, "请等待录音结束，再进入设置");
        return;
    }
#if CONFIG_P4_ASR_MODE
    if (speech_to_text_mode_active() && !speech_to_text_mode_can_settings()) return;
#endif
    if (qh_navigation_busy() && event != NULL) return;
    settings_home_screen = lv_screen_active();
    if (settings_screen == NULL) {
        settings_screen = lv_obj_create(NULL);qh_screen(settings_screen);
#if CONFIG_P4_SIGN_LANGUAGE_MODE
        portrait_observe_screen(settings_screen,2);
#endif
        qh_header(settings_screen,"设置",settings_cached_font(project_ui_font()),settings_return_callback,false);

        lv_obj_t *list = lv_obj_create(settings_screen);
        lv_obj_remove_style_all(list);
        lv_obj_set_size(list, UI_CONTENT_WIDTH, UI_SETTINGS_HEIGHT);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, UI_SETTINGS_TOP);
        lv_obj_set_style_pad_row(list, 8, 0);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_elastic(list, false);
        const char *names[] = {"Wi-Fi", "屏幕亮度", "扬声器音量", "全局字号", "对话历史", "自动息屏", "关于设备", "使用帮助"};
        for (unsigned index = 0; index < 8; ++index) {
            if (index==0 || index==1 || index==3 || index==5) {
                const char *group=index==0?"连接":index==1?"屏幕与声音":index==3?"文字与会话":"设备";
                lv_obj_t *heading=settings_label(list,group,QH_MUTED);
                lv_obj_set_width(heading,UI_CONTENT_WIDTH-28);
            }
            lv_obj_t *row = lv_obj_create(list);
            settings_rows[index]=row;
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, UI_CONTENT_WIDTH - 12, 104);
            lv_obj_set_style_bg_color(row, qh_color(QH_SURFACE), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(row, 20, LV_PART_MAIN);
            lv_obj_set_style_border_width(row,1,0);
            lv_obj_set_style_border_color(row,qh_color(QH_LINE),0);
            lv_obj_set_clickable(row, false);
            lv_obj_set_scrollable(row, false);
            lv_obj_t *name = settings_label(row, names[index], QH_INK);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 28, 0);
            lv_obj_t *availability = settings_label(row, "暂不可用", QH_MUTED);
            lv_obj_align(availability, LV_ALIGN_RIGHT_MID, -28, 0);
            if (index == 0) {
                lv_obj_set_hidden(availability, true);
                lv_obj_t *button = lv_button_create(row);
                style_home_button(button, QH_SOFT);
                lv_obj_set_size(button, 180, 70);
                lv_obj_align(button, LV_ALIGN_RIGHT_MID, -28, 0);
                lv_obj_t *label = settings_label(button, "管理网络", QH_INK);
                lv_obj_center(label);
                lv_obj_add_event_cb(button, settings_wifi_callback, LV_EVENT_CLICKED, NULL);
            } else if (index == 1) {
                lv_obj_set_height(row, 160);
                lv_obj_align(name, LV_ALIGN_TOP_LEFT, 24, 16);
                lv_obj_align(availability, LV_ALIGN_TOP_RIGHT, -24, 16);
                settings_brightness_value = availability;
                lv_label_set_text_fmt(availability, "%d%%", settings_brightness);
                settings_slider(row, 10, settings_brightness, settings_brightness_callback);
            } else if (index == 2) {
                lv_obj_set_height(row, 160);
                lv_obj_align(name, LV_ALIGN_TOP_LEFT, 24, 16);
                lv_obj_align(availability, LV_ALIGN_TOP_RIGHT, -24, 16);
                settings_volume_value = availability;
                lv_label_set_text_fmt(availability, "%d%%", atomic_load(&settings_volume_requested));
                settings_volume_slider = settings_slider(row, 0,
                    atomic_load(&settings_volume_requested), settings_volume_callback);
                lv_obj_add_event_cb(settings_volume_slider, settings_volume_release_callback,
                                    LV_EVENT_RELEASED, NULL);
            } else if (index == 3) {
                lv_obj_set_height(row, 190);
                lv_obj_align(name, LV_ALIGN_TOP_LEFT, 24, 12);
                lv_obj_align(availability, LV_ALIGN_TOP_RIGHT, -24, 12);
                settings_subtitle_value = availability;
                for (unsigned i = 0; i < 2; ++i) {
                    lv_obj_t *choice = lv_button_create(row);
                    settings_subtitle_buttons[i] = choice;
                    style_home_button(choice, QH_LINE);
                    lv_obj_set_size(choice, 180, 70);
                    lv_obj_align(choice, LV_ALIGN_BOTTOM_MID, i == 0 ? -110 : 110, -16);
                    lv_obj_t *label = settings_label(choice, i == 0 ? "14px" : "28px", QH_INK);
                    lv_obj_center(label);
                    lv_obj_add_event_cb(choice, settings_subtitle_callback, LV_EVENT_CLICKED,
                                        (void *)(intptr_t)(i == 0 ? 14 : 28));
                }
                settings_subtitle_refresh();
            } else if (index == 5) {
                lv_obj_set_hidden(availability, true);
                lv_obj_t *dropdown = lv_dropdown_create(row);
                settings_timeout_dropdown = dropdown;
                lv_obj_set_size(dropdown, 250, 70);
                lv_obj_set_style_pad_ver(dropdown, 0, 0);
                lv_obj_align(dropdown, LV_ALIGN_RIGHT_MID, -28, 0);
                lv_dropdown_set_options(dropdown, "永不\n30秒\n1分钟\n2分钟\n5分钟");
                for (unsigned i = 0; i < sizeof(screen_timeout_choices) / sizeof(screen_timeout_choices[0]); ++i)
                    if (screen_timeout_choices[i] == screen_idle_get_timeout()) lv_dropdown_set_selected(dropdown, i);
                const lv_font_t *font = settings_cached_font(project_ui_font());
                lv_obj_set_style_text_font(dropdown, font, 0);
                lv_obj_set_style_text_font(dropdown, LV_FONT_DEFAULT, LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(dropdown, qh_color(QH_SOFT), 0);
                lv_obj_set_style_text_color(dropdown, qh_color(QH_INK), 0);
                lv_obj_t *options = lv_dropdown_get_list(dropdown);
                lv_obj_set_style_text_font(options, font, 0);
                lv_obj_set_style_bg_color(options, qh_color(QH_SOFT), 0);
                lv_obj_set_style_text_color(options, qh_color(QH_INK), 0);
                lv_obj_set_style_bg_color(options, qh_color(QH_SOFT), LV_PART_SELECTED);
                lv_obj_set_style_text_color(options, qh_color(QH_INK), LV_PART_SELECTED);
                lv_obj_add_event_cb(dropdown, settings_timeout_callback, LV_EVENT_VALUE_CHANGED, NULL);
            } else if (index == 6) {
                lv_obj_set_hidden(availability, true);
                lv_obj_t *button = lv_button_create(row);
                style_home_button(button, QH_SOFT);
                lv_obj_set_size(button, 160, 70);
                lv_obj_align(button, LV_ALIGN_RIGHT_MID, -28, 0);
                lv_obj_t *label = settings_label(button, "查看信息", QH_INK);
                lv_obj_center(label);
                lv_obj_add_event_cb(button, settings_about_callback, LV_EVENT_CLICKED, NULL);
            } else if (index == 7) {
                lv_obj_set_hidden(availability, true);
                lv_obj_t *button = lv_button_create(row); qh_button(button, QH_SOFT);
                lv_obj_set_size(button, 180, 70); lv_obj_align(button, LV_ALIGN_RIGHT_MID, -28, 0);
                lv_obj_t *label = settings_label(button, "查看帮助", QH_INK); lv_obj_center(label);
                lv_obj_add_event_cb(button, settings_help_callback, LV_EVENT_CLICKED, NULL);
            } else if (index == 4) {
                lv_obj_set_height(row, 190);
                lv_obj_align(name, LV_ALIGN_TOP_LEFT, 24, 12);
                settings_history_value = availability;
                lv_obj_set_width(availability, 290);
                lv_label_set_long_mode(availability, LV_LABEL_LONG_WRAP);
                lv_obj_align(availability, LV_ALIGN_BOTTOM_LEFT, 24, -16);
                lv_label_set_text(availability, "读取中");
                lv_obj_t *clear = lv_button_create(row);
                style_home_button(clear, QH_DANGER);
                lv_obj_set_size(clear, 160, 70);
                lv_obj_align(clear, LV_ALIGN_BOTTOM_RIGHT, -24, -16);
                lv_obj_t *label = settings_label(clear, "清除历史", QH_ON_ACCENT);
                lv_obj_center(label);
                lv_obj_add_event_cb(clear, settings_clear_history, LV_EVENT_CLICKED, NULL);
            }
        }
        settings_save_label = settings_label(settings_screen, "调节后自动保存", QH_MUTED);
        lv_obj_align(settings_save_label, LV_ALIGN_BOTTOM_MID, 0, -18);
        lv_timer_create(settings_volume_refresh, 250, NULL);
    }
    if (!atomic_load(&settings_test_enabled)) atomic_store(&settings_test_status, 0);
    atomic_store(&settings_test_enabled, true);
    settings_volume_changed = false;
    settings_relayout();
    qh_screen_load(settings_screen, false);
}

static void network_health(lv_timer_t *timer)
{
    (void)timer;
    time_t now = time(NULL);
    ESP_LOGI(TAG, "network_health wifi=%d rtc=%d engine=%d register=%d recovery=%d suspended=%d clock=%d internal=%u largest=%u",
             (int)wifi_link_up, (int)cloud_connected, cloud_engine != NULL,
             (int)cloud_start_requested, (int)cloud_recovery_in_progress,
             voice_ui_suspended(), now >= 1735689600,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void menu_return(lv_event_t *event)
{
    (void)event;
    if (qh_navigation_busy() || conversation_recording || conversation_finish_requested || local_loop_active) return;
    text_entry_release(home_ui_screen);
    qh_screen_load(menu_screen, true);
}
static void menu_voice(lv_event_t *event)
{
    (void)event;
    if (qh_navigation_busy()) return;
    qh_screen_load(home_ui_screen, false);
}
static void menu_readiness(lv_timer_t *timer)
{
    (void)timer;
    if (lv_screen_active()!=menu_screen) return;
    qh_home_set_readiness(&menu_home, wifi_link_up, cloud_connected,
        atomic_load(&idle_welcome_started) && !atomic_load(&idle_welcome_finished) && !atomic_load(&chat_user_has_spoken));
}
static void create_menu_screen(void)
{
    qh_set_settings_callback(settings_button_callback);
    lv_event_cb_t transcript = NULL, camera = NULL;
#if CONFIG_P4_ASR_MODE
    transcript = speech_to_text_button_callback;
#endif
#if CONFIG_P4_SIGN_LANGUAGE_MODE
    camera = sign_language_button_callback;
#endif
    qh_home_create(&menu_home, project_ui_font(), project_ui_font(), menu_voice,
        transcript, camera, settings_button_callback, settings_help_callback);
    menu_screen = menu_home.screen;
    apply_subtitle_font();
    menu_readiness(NULL);
    lv_timer_create(menu_readiness, 500, NULL);
}

static void create_diagnostics_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    home_ui_screen = screen;
#if CONFIG_P4_SIGN_LANGUAGE_MODE
    portrait_observe_screen(screen,1);
#endif
    lv_timer_create(network_health, 10000, NULL);
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *touch_surface;

    lv_obj_set_style_bg_color(screen, qh_color(QH_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    title = qh_header(screen, "语音对话", project_ui_font(), menu_return, true);
    home_title_label = title;
    apply_project_font(title);

    lv_obj_t *typing_button = lv_button_create(screen);
    style_home_button(typing_button, QH_SOFT);
    lv_obj_set_size(typing_button, QH_TOOL_SIZE, QH_TOOL_SIZE);
    lv_obj_set_pos(typing_button, QH_LANGUAGE_X, QH_TOOL_Y);
    lv_obj_t *typing_icon = lv_label_create(typing_button);
    typing_icon_label = typing_icon;
    lv_obj_set_style_text_font(typing_icon, project_ui_font(), 0);
    lv_label_set_text(typing_icon, "拼");
    lv_obj_center(typing_icon);
    lv_obj_add_event_cb(typing_button, open_home_text, LV_EVENT_CLICKED, NULL);

    subtitle = lv_label_create(screen);
    home_subtitle=subtitle;
    apply_project_font(subtitle);
    lv_label_set_text(subtitle, "声音与文字，按你的方式");
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(subtitle, qh_color(QH_MUTED), LV_PART_MAIN);
    lv_obj_set_width(subtitle, UI_CONTENT_WIDTH);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 82);

    status_label = lv_label_create(screen);
    apply_project_font(status_label);
    lv_label_set_text(status_label, "正在连接云端语音服务...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_size(status_label, UI_CONTENT_WIDTH, LV_SIZE_CONTENT);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(status_label, qh_color(QH_INK), LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, UI_STATUS_TOP);

    chat_history_create_ui(screen, project_ui_font());
    lv_obj_add_event_cb(status_label,home_header_changed,LV_EVENT_SIZE_CHANGED,NULL);
    lv_obj_add_event_cb(subtitle,home_header_changed,LV_EVENT_SIZE_CHANGED,NULL);
    apply_subtitle_font();
    touch_surface = lv_obj_create(screen);
    home_hold_control = touch_surface;
    style_home_button(touch_surface, QH_ACCENT);
    lv_obj_set_size(touch_surface, UI_CONTENT_WIDTH, 70);
    lv_obj_set_pos(touch_surface, UI_MARGIN, UI_HOLD_TOP);
    lv_obj_t *button_label = lv_label_create(touch_surface);
    apply_project_font(button_label);
    lv_label_set_text(button_label, "按住说话，松开发送");
    lv_obj_set_style_text_color(button_label, qh_color(QH_ON_ACCENT), LV_PART_MAIN);
    lv_obj_center(button_label);
    lv_obj_add_event_cb(touch_surface, touch_event_callback, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch_surface, touch_event_callback, LV_EVENT_RELEASED, NULL);

    const lv_font_t *input_font = project_ui_font();
#if CONFIG_P4_ASR_MODE
    if (speech_to_text_mode_cached_font28()) input_font = speech_to_text_mode_cached_font28();
#endif
    text_entry_mount(screen, input_font, submit_home_text, NULL, home_input_layout, NULL,
                     UI_HOME_INPUT_TOP, typing_button);
    create_menu_screen();
    lv_screen_load(menu_screen);
}

static void log_memory_summary(void)
{
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "heap: default_free=%" PRIu32, (uint32_t)esp_get_free_heap_size());
    ESP_LOGI(TAG, "PSRAM: total=%" PRIu32 " MiB, free=%" PRIu32 " bytes",
             (uint32_t)(psram_total / BYTES_PER_MIB), (uint32_t)psram_free);
}

/* The SD card is deliberately mounted without formatting.  Its font files will
 * later be read through the normal VFS path, but this first step only verifies
 * the hardware and reports free space; it never creates, deletes or changes a
 * file on the card. */
static void verify_sdcard_read_only(void)
{
    const esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s); card contents were not modified", esp_err_to_name(err));
        set_status_text("存储卡未识别：不影响当前功能");
        return;
    }

    sdmmc_card_t *card = bsp_sdcard_get_handle();
    uint64_t total = 0;
    uint64_t free = 0;
    if (card != NULL) {
        ESP_LOGI(TAG, "SD card: %s, capacity=%" PRIu64 " MiB",
                 card->cid.name,
                 ((uint64_t)card->csd.capacity * card->csd.sector_size) / BYTES_PER_MIB);
    }
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free) == ESP_OK) {
        ESP_LOGI(TAG, "SD card VFS mounted at %s: total=%" PRIu64 " MiB, free=%" PRIu64 " MiB",
                 BSP_SD_MOUNT_POINT, total / BYTES_PER_MIB, free / BYTES_PER_MIB);
        set_status_text("存储卡已识别，字库将从存储卡按需读取");
    } else {
        ESP_LOGW(TAG, "SD card mounted but free-space query failed: errno=%d", errno);
        set_status_text("存储卡已识别，容量查询失败");
    }
}

/* The binary font's tables stay on SD.  LVGL retains only metadata and a
 * single glyph-sized scratch buffer, then reads each bitmap on demand. */
static void try_load_sdcard_font(void)
{
    if (access(SDCARD_FONT_VFS_PATH, R_OK) != 0) {
        ESP_LOGI(TAG, "SD font is not present yet: %s", SDCARD_FONT_VFS_PATH);
        return;
    }
    lv_font_t *loaded = p4_font_load(SDCARD_FONT_LVGL_PATH);
    if (!bsp_display_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "cannot acquire LVGL lock to load SD font");
        if (loaded && bsp_display_lock(0)) {
            p4_binfont_destroy(loaded);
            bsp_display_unlock();
        }
        return;
    }
    sdcard_font = loaded;
    if (sdcard_font != NULL) {
        refresh_ui_font_from_sdcard();
    }
    bsp_display_unlock();

    if (sdcard_font != NULL) {
        ESP_LOGI(TAG, "external Chinese font loaded from SD with dynamic glyph reads");
        set_status_text("完整中文字库已从存储卡加载");
    } else {
        ESP_LOGE(TAG, "SD font exists but LVGL could not load it; keeping bootstrap font");
        set_status_text("存储卡字库无效，已保留基础字库");
    }
}

static int hex_nibble(const char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static bool sha256_matches_font(const uint8_t digest[32])
{
    for (size_t index = 0; index < 32; ++index) {
        const int high = hex_nibble(P4X_FONT_TRANSFER_SHA256[index * 2]);
        const int low = hex_nibble(P4X_FONT_TRANSFER_SHA256[index * 2 + 1]);
        if (high < 0 || low < 0 || digest[index] != (uint8_t)((high << 4) | low)) {
            return false;
        }
    }
    return P4X_FONT_TRANSFER_SHA256[64] == '\0';
}

/* Downloads only a missing font. Data is written to a temporary name,
 * authenticated with the known SHA-256, and renamed only after all checks
 * pass. Existing user files and an existing valid font are untouched. */
static void font_sync_task(void *arg)
{
    static const char temporary_path[] = "/sdcard/fonts/P4ZH14.TMP";
    uint8_t read_buffer[1024];
    uint8_t digest[32];
    size_t bytes_written = 0;
    bool success = false;
    FILE *file = NULL;
    mbedtls_sha256_context sha;

    (void)arg;
    ESP_LOGI(TAG, "font sync: task started");
    mbedtls_sha256_init(&sha);
    if (access(SDCARD_FONT_VFS_PATH, R_OK) == 0) {
        try_load_sdcard_font();
        goto finished;
    }
    if (mkdir("/sdcard/fonts", 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "font sync: cannot create SD font directory (errno=%d)", errno);
        goto finished;
    }

    set_status_text("正在把完整中文字库保存到存储卡...");
    file = fopen(temporary_path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "font sync: cannot open temporary SD file (errno=%d)", errno);
        goto cleanup;
    }
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        ESP_LOGE(TAG, "font sync: SHA-256 initialization failed");
        goto cleanup;
    }
    while (bytes_written < P4X_FONT_TRANSFER_SIZE) {
        const size_t segment_size = (P4X_FONT_TRANSFER_SIZE - bytes_written > FONT_SEGMENT_BYTES) ?
                                    FONT_SEGMENT_BYTES : P4X_FONT_TRANSFER_SIZE - bytes_written;
        char url[160];
        const int url_length = snprintf(url, sizeof(url), "%s?offset=%u&length=%u",
                                        P4X_FONT_TRANSFER_URL, (unsigned)bytes_written, (unsigned)segment_size);
        esp_http_client_config_t configuration = {
            .url = url,
            .timeout_ms = 15000,
            .buffer_size = sizeof(read_buffer),
        };
        esp_http_client_handle_t client = (url_length > 0 && url_length < (int)sizeof(url)) ?
                                          esp_http_client_init(&configuration) : NULL;
        if (client == NULL || esp_http_client_open(client, 0) != ESP_OK ||
            esp_http_client_fetch_headers(client) != (int)segment_size ||
            esp_http_client_get_status_code(client) != 200) {
            ESP_LOGE(TAG, "font sync: segment request failed at %u bytes", (unsigned)bytes_written);
            if (client != NULL) { esp_http_client_cleanup(client); }
            goto cleanup;
        }
        size_t segment_read = 0;
        while (segment_read < segment_size) {
            const int read_length = esp_http_client_read(client, (char *)read_buffer,
                                                          segment_size - segment_read > sizeof(read_buffer) ?
                                                          sizeof(read_buffer) : segment_size - segment_read);
            if (read_length <= 0 || fwrite(read_buffer, 1, (size_t)read_length, file) != (size_t)read_length ||
                mbedtls_sha256_update(&sha, read_buffer, (size_t)read_length) != 0) {
                ESP_LOGE(TAG, "font sync: segment transfer failed at %u bytes", (unsigned)bytes_written);
                esp_http_client_cleanup(client);
                goto cleanup;
            }
            segment_read += (size_t)read_length;
            bytes_written += (size_t)read_length;
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (mbedtls_sha256_finish(&sha, digest) != 0 || !sha256_matches_font(digest)) {
        ESP_LOGE(TAG, "font sync: integrity check failed; discarding temporary file");
        goto cleanup;
    }
    if (fclose(file) != 0 || rename(temporary_path, SDCARD_FONT_VFS_PATH) != 0) {
        file = NULL;
        ESP_LOGE(TAG, "font sync: cannot finalize verified font (errno=%d)", errno);
        goto cleanup;
    }
    file = NULL;
    success = true;
    ESP_LOGI(TAG, "font sync: verified %u-byte Chinese font saved to SD", (unsigned)bytes_written);

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    mbedtls_sha256_free(&sha);
    if (!success) {
        (void)remove(temporary_path);
    } else {
        try_load_sdcard_font();
    }
finished:
    font_sync_in_progress = false;
    vTaskDelete(NULL);
}

static void start_font_synchronization(void)
{
    if (atomic_load(&startup_base_loading)) return;
    if (font_sync_requested || sdcard_font != NULL) {
        ESP_LOGI(TAG, "font sync: not scheduled (already requested or loaded)");
        return;
    }
    ESP_LOGI(TAG, "font sync: scheduling local SD font transfer");
    font_sync_requested = true;
    font_sync_in_progress = true;
    /* Run this one-time local transfer ahead of RTC registration so the C6
     * transport is not contending with DNS/TLS traffic while filling the SD. */
    if (xTaskCreate(font_sync_task, "sd_font_sync", FONT_SYNC_TASK_STACK_BYTES, NULL, 6, NULL) != pdPASS) {
        font_sync_requested = false;
        font_sync_in_progress = false;
        ESP_LOGE(TAG, "font sync: unable to create task");
    }
}

/* No audio/session lifecycle changes: existing Wi-Fi/RTC startup runs beside
 * this low-priority task. Startup never downloads missing V2 fonts. */
static void startup_resource_task(void *arg)
{
    (void)arg;
    atomic_store(&startup_resources_started,true);
    const uint32_t started = esp_log_timestamp();
    if (atomic_load(&startup_base_loading)) try_load_sdcard_font();
    atomic_store(&startup_base_loading, false);
    atomic_store(&startup_base_done, true);
#if CONFIG_P4_ASR_MODE
    const esp_err_t error = speech_to_text_mode_preload_fonts();
    atomic_store(&startup_font_error,error);
    if (error == ESP_OK && bsp_display_lock(0)) {
        /* Reuse the fully published V2 14px index for homepage punctuation.
         * The original SD font remains intact as a fallback; this borrowed
         * shared font is never freed or copied by the homepage. Normally the
         * home screen is still inactive, so binding cannot redraw it under GIF. */
        refresh_ui_font_from_sdcard();
        ESP_LOGI(TAG, "home_font_v2 bound=%d", speech_to_text_mode_font14() != NULL);
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "startup_fonts error=%d duration_ms=%" PRIu32,
             (int)error, esp_log_timestamp() - started);
#endif
    /* Installed Pinyin resources load under the startup animation, without
     * downloading or delaying the existing bounded startup indefinitely. */
    if (pinyin_service_preload()) {
        const uint32_t pinyin_started = esp_log_timestamp();
        while (pinyin_service_state() == PINYIN_LOADING &&
               esp_log_timestamp() - pinyin_started < 3000)
            vTaskDelay(pdMS_TO_TICKS(20));
        ESP_LOGI(TAG, "startup_pinyin state=%d duration_ms=%u", pinyin_service_state(),
                 (unsigned)(esp_log_timestamp() - pinyin_started));
    }
    atomic_store(&startup_fonts_done, true);
    vTaskDelete(NULL);
}

/* Weighted completed stages, not bytes downloaded or a time-based estimate.
 * Timer does no SD/network I/O; timeout never pretends resources are complete. */
static void startup_set_label(const char *text)
{
    if(strcmp(lv_label_get_text(startup_label),text))lv_label_set_text(startup_label,text);
}
static void startup_status_timer(lv_timer_t *timer)
{
    if (startup_overlay == NULL) {
        if (qh_navigation_busy()) return;
        /* One timer interval after removal lets LVGL present the homepage. */
        atomic_store(&startup_home_visible, true);
        ESP_LOGI(TAG, "startup_home_visible=1; cloud_welcome_allowed=1");
        lv_timer_delete(timer);
        return;
    }
    const uint32_t elapsed = lv_tick_get() - startup_started;
    /* Local low-frequency loop; resource flags decide when to leave. */
    if (startup_leaf) qh_startup_leaf_step(startup_leaf, elapsed);
    const bool fonts_done = atomic_load(&startup_fonts_done);
    const bool network_prepared = atomic_load(&startup_network_prepared);
    bool fonts_ready = true;
    unsigned font_stage = 0;
    bool font_failed=false;
#if CONFIG_P4_ASR_MODE
    fonts_ready = speech_to_text_mode_fonts_ready();
    font_stage = speech_to_text_mode_fonts_stage();
    font_failed=font_stage==SPEECH_FONTS_FAILED;
#endif
    /* Independent task states; completion order is intentionally not forced. */
    const int pinyin=pinyin_service_state();
    const bool failed=atomic_load(&startup_resources_failed);
    const unsigned stage[3]={
        failed || atomic_load(&startup_font_error) || font_failed ? 3 :
            fonts_ready ? 2 : atomic_load(&startup_resources_started) ? 1 : 0,
        failed || pinyin==PINYIN_FAILED ? 3 : pinyin==PINYIN_READY ? 2 : pinyin==PINYIN_LOADING ? 1 : 0,
        wifi_link_up && network_prepared ? 2 : elapsed>=20000 ? 3 : 1};
    const unsigned mask=stage[0] | (stage[1]<<2) | (stage[2]<<4);
    if(mask!=startup_last_mask){
        const char *names[]={"字库","输入法","联网"};
        const char *states[]={"等待","准备中","完成","失败"};
        for(unsigned i=0;i<3;++i){
            char text[64];
            const char *value=i==2 && stage[i]==3 ? (wifi_link_up?"未就绪":"离线") : states[stage[i]];
            snprintf(text,sizeof(text),"%s\n%s",names[i],value);
            lv_label_set_text(startup_stage_labels[i],text);
            const uint32_t color=stage[i]==2?QH_ACCENT:stage[i]==3?QH_DANGER:stage[i]==1?QH_MUTED:QH_LINE;
            lv_obj_set_style_bg_color(startup_stages[i],qh_color(color),0);
            lv_obj_set_style_text_color(startup_stage_labels[i],qh_color(stage[i]==3?QH_ERROR:QH_INK),0);
        }
        startup_last_mask=mask;
    }
    if(elapsed-startup_last_diagnostic>=5000){
        ESP_LOGI(TAG,"startup_states fonts=%u pinyin=%u network=%u elapsed_ms=%u",
            stage[0],stage[1],stage[2],elapsed);startup_last_diagnostic=elapsed;
    }
    const bool preparation_ended = fonts_done &&
        ((wifi_link_up && network_prepared) || elapsed >= 20000);
    const bool abnormal_timeout = elapsed >= 60000;
    if (startup_finishing) {
        /* Switch away before deleting the active startup screen. The original
         * homepage stays alive, with its prepared font bindings and callbacks. */
        qh_screen_load(startup_home_screen, false);
        lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()), LV_DEF_REFR_PERIOD);
        startup_home_screen = NULL;
        lv_obj_delete(startup_overlay);
        startup_overlay = startup_label = NULL;
        startup_leaf = NULL;
        ESP_LOGI(TAG, "startup_home duration_ms=%" PRIu32 " fonts_done=%d cloud_ready=%d",
                 elapsed, fonts_done, (int)cloud_connected);
        return;
    }
    if (preparation_ended || abnormal_timeout) {
        /* Present the real completion/fallback value for one UI interval.
         * Do not jump away mid-font at the previous unconditional 20s limit. */
        startup_finishing = true;
        startup_set_label( abnormal_timeout ?
            "准备超时，未完成资源将继续后台加载" :
            !fonts_ready ? "字库未就绪，暂用基础字库" :
            !wifi_link_up || !network_prepared ? "联网未就绪，进入首页后可继续操作" :
            "资源准备完成，正在进入首页...");
        return;
    }
    const char *font_status = !atomic_load(&startup_base_done) ? "正在加载首页字库..." :
        fonts_ready ? "已加载字库" : fonts_done ? "字库未就绪，暂用基础字库" :
        font_stage == 1 ? "正在校验14和28字号字库..." :
        font_stage == 3 ? "已加载14字号字库，正在加载28字号字库..." :
                         "正在加载14字号字库...";
    startup_set_label( stage[0]==3 ? "字库准备失败，基础界面仍可使用" :
        stage[1]==3 ? "输入法准备失败，可使用英文输入" : !fonts_done ? font_status :
        !fonts_ready ? "字库未就绪，暂用基础字库" :
        !wifi_link_up || !network_prepared ? "已加载字库，正在联网获取资源..." :
                                             "资源准备完成，正在进入首页...");
}

static void create_startup_placeholder(void)
{
    lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()), 80);
    startup_started = lv_tick_get();
    startup_home_screen = lv_screen_active();
    /* Separate active screen: never redraw the covered SD-font homepage. */
    startup_overlay = lv_obj_create(NULL); qh_screen(startup_overlay);
    startup_leaf = qh_startup_leaf(startup_overlay);
    lv_obj_align(startup_leaf, LV_ALIGN_TOP_MID, 0, 208);
    lv_obj_t *brand = qh_label(startup_overlay, &lv_font_startup_zh, "灵译通", QH_INK);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 466);
    lv_obj_t *caption = qh_label(startup_overlay, &lv_font_startup_zh, "每一种表达，都被听见。", QH_MUTED);
    lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 524);
    startup_label = qh_label(startup_overlay, &lv_font_startup_zh, "正在启动...", QH_MUTED);
    lv_obj_set_width(startup_label, 520);
    lv_label_set_long_mode(startup_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(startup_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(startup_label,84);
    lv_obj_set_pos(startup_label,40,748);
    for (unsigned i=0;i<3;i++) {
        startup_stages[i] = lv_obj_create(startup_overlay);
        lv_obj_remove_style_all(startup_stages[i]);
        qh_panel(startup_stages[i], QH_LINE, 2);
        lv_obj_set_size(startup_stages[i], 160, 4);
        lv_obj_set_pos(startup_stages[i], 44+i*176, 930);
        startup_stage_labels[i]=qh_label(startup_overlay,&lv_font_startup_zh,"等待",QH_INK);
        lv_obj_set_pos(startup_stage_labels[i],44+i*176,856);
        lv_obj_set_size(startup_stage_labels[i],160,64);
        lv_obj_set_style_text_align(startup_stage_labels[i],LV_TEXT_ALIGN_CENTER,0);
    }
    lv_screen_load(startup_overlay);
    lv_timer_create(startup_status_timer, 200, NULL);
}

static bool display_busy(void)
{
    static bool startup_armed, home_seen;
    static uint32_t home_seen_ms;
    const bool home_visible = atomic_load(&startup_home_visible);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (home_visible && !home_seen) { home_seen = true; home_seen_ms = now; }
    const uint32_t last_voice = atomic_load(&settings_last_audible_pcm_ms);
    const uint32_t quiet_ms = last_voice ? now - last_voice : 0;
    if (home_visible && cloud_audio_queue != NULL && atomic_load(&idle_welcome_pcm_played) &&
        !atomic_load(&idle_welcome_finished) && !atomic_load(&chat_user_has_spoken) &&
        screen_idle_welcome_pcm_finished(atomic_load(&idle_welcome_pcm_played),
            feedback_queue_allows_tone(), quiet_ms)) {
        atomic_store(&idle_welcome_finished, true);
        ESP_LOGI("screen_idle", "welcome_pcm_finished=1 quiet_ms=%u", (unsigned)quiet_ms);
    }
    if (!startup_armed) {
        const bool offline = !cloud_connected && !cloud_start_requested && !cloud_recovery_in_progress;
        const bool welcome_finished = atomic_load(&idle_welcome_finished);
        startup_armed = screen_idle_startup_can_arm(home_visible, welcome_finished,
            atomic_load(&chat_user_has_spoken), offline, now - home_seen_ms,
            quiet_ms);
        if (!startup_armed) return true;
        lv_display_trigger_activity(NULL);
        ESP_LOGI("screen_idle", "startup_gate_ready=1 welcome_finished=%d offline=%d", welcome_finished, offline);
    }
    /* A delayed first welcome after an offline fallback also gets protection. */
    if (atomic_load(&idle_welcome_started) && !atomic_load(&idle_welcome_finished) &&
        !atomic_load(&chat_user_has_spoken)) return true;
    return !atomic_load(&startup_home_visible) || qh_navigation_busy() || voice_ui_suspended() ||
        conversation_recording || conversation_finish_requested || local_loop_active ||
        /* RTC can keep sending silent PCM with its codec open. Advisory
         * ANSWERING/idle-bit states must not keep the screen awake forever. */
        feedback_voice_recent() || atomic_load(&settings_test_status) == 2;
}
static unsigned display_saved_brightness(void) { return atomic_load(&settings_brightness); }

void app_main(void)
{
    esp_chip_info_t chip_info = {0};
    uint32_t flash_size = 0;
    lv_display_t *display;

    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "============================================================");
    ESP_LOGI(TAG, "Built_In_V6 P4X audio bring-up");
    ESP_LOGI(TAG, "board_profile=%s, idf=%s", APP_BOARD_PROFILE, esp_get_idf_version());
    ESP_LOGI(TAG, "chip: model=%d, revision=%d, cores=%d", chip_info.model, chip_info.revision, chip_info.cores);
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "flash: %" PRIu32 " MiB", flash_size / BYTES_PER_MIB);
    }
    log_memory_summary();

    const esp_err_t chat_init = chat_history_init();
    if (chat_init != ESP_OK) ESP_LOGW(TAG, "chat: initialization error=%d", (int)chat_init);
    settings_storage_start();
    const bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = UI_WIDTH * 160,
        .double_buffer = true,
        .hw_cfg = {
            .dsi_bus = {.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS},
        },
        .flags = {.buff_dma = true, .buff_spiram = true, .sw_rotate = true},
    };
    display = bsp_display_start_with_config(&display_config);
    if (display == NULL || bsp_display_get_input_dev() == NULL) {
        ESP_LOGE(TAG, "display/touch initialization failed");
        return;
    }
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "failed to acquire LVGL lock");
        return;
    }
#if CONFIG_P4_SIGN_LANGUAGE_MODE
    portrait_observe_touch(bsp_display_get_input_dev());
#endif
    /* Portrait transport rotates partial draws; LVGL maps GT911 coordinates. */
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
    ESP_LOGI(TAG, "portrait: logical=%ldx%ld rotation=90",
             (long)lv_display_get_horizontal_resolution(display),
             (long)lv_display_get_vertical_resolution(display));
    qh_theme_native();
    qh_screen(lv_screen_active());
    create_diagnostics_ui();
    create_startup_placeholder();
    screen_idle_init(display_busy, display_saved_brightness);
    bsp_display_unlock();
    ESP_ERROR_CHECK(bsp_display_brightness_set(settings_brightness));
    const esp_err_t keyboard_error = usb_keyboard_start(display, physical_keyboard_key);
    if (keyboard_error != ESP_OK) ESP_LOGW(TAG, "keyboard: init_error=%d", (int)keyboard_error);

    verify_sdcard_read_only();
    const esp_err_t chat_storage = chat_history_start_storage();
    if (chat_storage != ESP_OK) ESP_LOGW(TAG, "chat: storage start error=%d", (int)chat_storage);
    atomic_store(&startup_base_loading, access(SDCARD_FONT_VFS_PATH, R_OK) == 0);
#if CONFIG_P4_ASR_MODE
    const esp_err_t resources = speech_to_text_mode_resources_init();
    if (resources != ESP_OK) ESP_LOGW(TAG, "startup_resources_init error=%d", (int)resources);
#endif

    microphone = bsp_audio_codec_microphone_init();
    speaker = bsp_audio_codec_speaker_init();
    if (microphone == NULL || speaker == NULL) {
        ESP_LOGE(TAG, "audio codec initialization failed: mic=%p speaker=%p", microphone, speaker);
        set_status_text("音频初始化失败，请查看 COM20 日志");
        return;
    }
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(microphone, 30.0F));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(speaker, atomic_load(&settings_volume_requested)));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(speaker, atomic_load(&settings_volume_requested) == 0));
    cloud_audio_queue = xQueueCreate(CLOUD_AUDIO_QUEUE_LENGTH, sizeof(cloud_audio_frame_t));
    audio_event_group = xEventGroupCreate();
    cloud_engine_lock = xSemaphoreCreateMutex();
    if (cloud_audio_queue == NULL || audio_event_group == NULL || cloud_engine_lock == NULL ||
        xTaskCreate(cloud_audio_playback_task, "cloud_playback",
                    AUDIO_PLAYBACK_TASK_STACK_BYTES, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(cloud_idle_audio_uplink_task, "cloud_idle_uplink",
                    AUDIO_IDLE_UPLINK_TASK_STACK_BYTES, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "audio: unable to create cloud playback queue/task");
        set_status_text("云端音频播放任务初始化失败");
        return;
    }
    xEventGroupSetBits(audio_event_group, AUDIO_PLAYBACK_IDLE_BIT);
    atomic_store(&startup_audio_ready, true);
    ESP_LOGI(TAG, "audio: ES8311 initialized (mic gain 30 dB, speaker 45%%)");
    start_wifi_station();
    if (xTaskCreate(startup_resource_task, "startup_resources", 12288, NULL, 2, NULL) != pdPASS) {
        atomic_store(&startup_resources_failed,true);
        atomic_store(&startup_base_loading, false);
        atomic_store(&startup_base_done, true);
        atomic_store(&startup_fonts_done, true);
        ESP_LOGW(TAG, "startup_resources_task_failed=1");
    }
    ESP_LOGI(TAG, "audio verification UI ready");
    ESP_LOGI(TAG, "============================================================");
}
