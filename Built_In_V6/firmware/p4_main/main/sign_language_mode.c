/* Demand-driven SC2336 preview derived from the verified eda69f5 probe. */
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "sign_language_mode.h"
#include "portrait_display.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/isp_awb.h"
#include "driver/isp_gamma.h"
#include "driver/isp_wbg.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_ipa.h"
#include "esp_log.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SENSOR_WIDTH 1920
#define SENSOR_HEIGHT 1080
#define PREVIEW_WIDTH 540
#define PREVIEW_HEIGHT 960
#define PREVIEW_OFFSET_X 30
/* PPA angles are counterclockwise: 90 turns the previous view by 180 degrees.
 * Half scaling preserves the complete sensor image without cropping. */
#define PREVIEW_OFFSET_Y 64
#define PROBE_BYTES_PER_PIXEL 2
#define SENSOR_FRAME_BYTES ((size_t)SENSOR_WIDTH * SENSOR_HEIGHT * PROBE_BYTES_PER_PIXEL)
#define CAPTURE_BUFFER_COUNT 3
#define PROBE_SECONDS 60
#define PREVIEW_STALL_TIMEOUT_US 3000000LL
#define PROBE_CSI_LANE_COUNT 2
#define PROBE_CSI_LANE_BIT_RATE_MBPS 336
#define PROBE_SENSOR_FORMAT "MIPI_2lane_24Minput_RAW8_1920x1080_30fps"
#define PROBE_SENSOR_HMIRROR 1
#define PROBE_WBG_FRACTION_BITS 8
#define PROBE_WBG_UNITY_GAIN (1U << PROBE_WBG_FRACTION_BITS)

static const char *TAG = "sign_preview";

/* Hardware shadow lift with preserved endpoints. A small post-AWB blue trim
 * addresses the observed cool preview without changing exposure or adding
 * per-frame CPU processing. This is a visual trim, not color calibration. */
static uint32_t preview_tone_curve(uint32_t value)
{
    return value * 384U / (value + 128U);
}

static uint32_t preview_blue_curve(uint32_t value)
{
    const uint32_t tone = preview_tone_curve(value);
    return tone - tone * (255U - tone) / 3072U;
}

static esp_err_t configure_preview_tone(isp_proc_handle_t isp)
{
    isp_gamma_curve_points_t points = {0};
    ESP_RETURN_ON_ERROR(esp_isp_gamma_fill_curve_points(preview_tone_curve, &points),
                        TAG, "creating preview tone curve failed");
    ESP_RETURN_ON_ERROR(esp_isp_gamma_configure(isp, COLOR_COMPONENT_R, &points),
                        TAG, "configuring red tone curve failed");
    ESP_RETURN_ON_ERROR(esp_isp_gamma_configure(isp, COLOR_COMPONENT_G, &points),
                        TAG, "configuring green tone curve failed");
    ESP_RETURN_ON_ERROR(esp_isp_gamma_fill_curve_points(preview_blue_curve, &points),
                        TAG, "creating warm blue curve failed");
    ESP_RETURN_ON_ERROR(esp_isp_gamma_configure(isp, COLOR_COMPONENT_B, &points),
                        TAG, "configuring blue tone curve failed");
    ESP_RETURN_ON_ERROR(esp_isp_gamma_enable(isp), TAG, "enabling preview tone failed");
    ESP_LOGI(TAG, "preview tone enabled: R/G 64->128 128->192 192->230; mild blue trim");
    return ESP_OK;
}

static volatile bool mode_active;
static volatile bool stop_requested;
static lv_obj_t *voice_screen;
static lv_obj_t *preview_screen;
static lv_obj_t *preview_status;
static const lv_font_t *preview_font;
static sign_language_prepare_fn prepare_voice;
static sign_language_resume_fn resume_voice;

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    esp_sccb_io_handle_t sccb;
    esp_cam_sensor_device_t *sensor;
} sensor_context_t;

typedef struct {
    void *capture_buffers[CAPTURE_BUFFER_COUNT];
    QueueHandle_t completed_frames;
    QueueHandle_t free_frames;
    volatile uint32_t new_buffer_count;
    volatile uint32_t finished_frame_count;
    volatile uint32_t displayed_frame_count;
    volatile uint32_t dropped_frame_count;
} capture_context_t;

typedef struct {
    isp_proc_handle_t isp;
    isp_awb_ctlr_t awb_controller;
    esp_ipa_pipeline_handle_t ipa_pipeline;
    const char *ipa_algorithm_names[2];
    esp_ipa_awb_config_t ipa_awb_config;
    esp_ipa_config_t ipa_config;
    QueueHandle_t stats_queue;
    esp_ipa_sensor_t sensor_info;
    uint64_t stats_sequence;
    float red_gain;
    float blue_gain;
    bool wbg_enabled;
    bool awb_enabled;
    bool awb_running;
} white_balance_context_t;

static bool awb_statistics_done(isp_awb_ctlr_t awb_controller,
                                const esp_isp_awb_evt_data_t *event,
                                void *user_data)
{
    (void)awb_controller;
    white_balance_context_t *white_balance = user_data;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueOverwriteFromISR(white_balance->stats_queue, event, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static esp_err_t stop_white_balance(white_balance_context_t *white_balance)
{
    if (white_balance->awb_running) {
        ESP_RETURN_ON_ERROR(esp_isp_awb_controller_stop_continuous_statistics(
            white_balance->awb_controller), TAG, "AWB stop failed");
        white_balance->awb_running = false;
    }
    if (white_balance->awb_enabled) {
        ESP_RETURN_ON_ERROR(
            esp_isp_awb_controller_disable(white_balance->awb_controller), TAG,
            "AWB disable failed");
        white_balance->awb_enabled = false;
    }
    if (white_balance->awb_controller != NULL) {
        ESP_RETURN_ON_ERROR(
            esp_isp_del_awb_controller(white_balance->awb_controller), TAG,
            "AWB delete failed");
        white_balance->awb_controller = NULL;
    }
    if (white_balance->wbg_enabled) {
        ESP_RETURN_ON_ERROR(esp_isp_wbg_disable(white_balance->isp), TAG,
                            "WBG disable failed");
        white_balance->wbg_enabled = false;
    }
    if (white_balance->ipa_pipeline != NULL) {
        ESP_RETURN_ON_ERROR(
            esp_ipa_pipeline_destroy(white_balance->ipa_pipeline), TAG,
            "IPA delete failed");
        white_balance->ipa_pipeline = NULL;
    }
    if (white_balance->stats_queue != NULL) {
        vQueueDelete(white_balance->stats_queue);
        white_balance->stats_queue = NULL;
    }
    return ESP_OK;
}

static esp_err_t start_white_balance(isp_proc_handle_t isp,
                                     white_balance_context_t *white_balance)
{
    esp_err_t ret = ESP_OK;
    const esp_ipa_config_t *sensor_config = esp_ipa_pipeline_get_config("SC2336");
    ESP_RETURN_ON_FALSE(sensor_config != NULL && sensor_config->awb != NULL,
                        ESP_ERR_NOT_FOUND, TAG, "SC2336 IPA AWB parameters are unavailable");

    memset(white_balance, 0, sizeof(*white_balance));
    white_balance->isp = isp;
    white_balance->sensor_info = (esp_ipa_sensor_t) {
        .width = SENSOR_WIDTH,
        .height = SENSOR_HEIGHT,
        .max_exposure = 33333,
        .min_exposure = 100,
        .cur_exposure = 10000,
        .max_gain = 64.0f,
        .min_gain = 1.0f,
        .cur_gain = 1.0f,
    };
    white_balance->red_gain = 1.0f;
    white_balance->blue_gain = 1.0f;
    white_balance->stats_queue = xQueueCreate(1, sizeof(esp_isp_awb_evt_data_t));
    ESP_GOTO_ON_FALSE(white_balance->stats_queue != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "allocating AWB statistics queue failed");

    white_balance->ipa_algorithm_names[0] = "esp_ipa_ian";
    white_balance->ipa_algorithm_names[1] = "esp_ipa_awb";
    white_balance->ipa_awb_config = *sensor_config->awb;
    white_balance->ipa_awb_config.min_red_gain_step = 0.01f;
    white_balance->ipa_awb_config.min_blue_gain_step = 0.01f;
    white_balance->ipa_config = (esp_ipa_config_t) {
        .names = white_balance->ipa_algorithm_names,
        .nums = 2,
        .version = sensor_config->version,
        .ian = sensor_config->ian,
        .awb = &white_balance->ipa_awb_config,
    };
    ESP_GOTO_ON_ERROR(
        esp_ipa_pipeline_create(&white_balance->ipa_config, &white_balance->ipa_pipeline),
        fail, TAG,
        "creating SC2336 AWB pipeline failed");

    esp_ipa_metadata_t metadata = {0};
    ESP_GOTO_ON_ERROR(esp_ipa_pipeline_init(white_balance->ipa_pipeline,
                                            &white_balance->sensor_info, &metadata),
                      fail, TAG, "initializing SC2336 AWB pipeline failed");
    ESP_GOTO_ON_FALSE(metadata.flags & IPA_METADATA_FLAGS_AWB, ESP_ERR_INVALID_RESPONSE,
                      fail, TAG, "SC2336 AWB pipeline returned no statistics range");

    const esp_isp_wbg_config_t wbg_config = {
        .flags.update_once_configured = true,
    };
    ESP_GOTO_ON_ERROR(esp_isp_wbg_configure(isp, &wbg_config), fail, TAG,
                      "configuring ISP white balance gain failed");
    ESP_GOTO_ON_ERROR(esp_isp_wbg_enable(isp), fail, TAG,
                      "enabling ISP white balance gain failed");
    white_balance->wbg_enabled = true;
    const isp_wbg_gain_t unity_gain = {
        .gain_r = PROBE_WBG_UNITY_GAIN,
        .gain_g = PROBE_WBG_UNITY_GAIN,
        .gain_b = PROBE_WBG_UNITY_GAIN,
    };
    ESP_GOTO_ON_ERROR(esp_isp_wbg_set_wb_gain(isp, unity_gain), fail, TAG,
                      "setting initial white balance gain failed");

    const esp_ipa_awb_range_t *range = &metadata.awb;
    const esp_isp_awb_config_t awb_config = {
        .sample_point = ISP_AWB_SAMPLE_POINT_BEFORE_CCM,
        .window = {
            .top_left = {.x = 0, .y = 0},
            .btm_right = {.x = SENSOR_WIDTH - 1, .y = SENSOR_HEIGHT - 1},
        },
        .subwindow = {
            .top_left = {.x = 0, .y = 0},
            .btm_right = {.x = SENSOR_WIDTH - 1, .y = SENSOR_HEIGHT - 1},
        },
        .white_patch = {
            .luminance = {
                .min = (uint32_t)(range->green_min * (1.0f + range->rg_min + range->bg_min)),
                .max = (uint32_t)(range->green_max * (1.0f + range->rg_max + range->bg_max)),
            },
            .red_green_ratio = {.min = range->rg_min, .max = range->rg_max},
            .blue_green_ratio = {.min = range->bg_min, .max = range->bg_max},
        },
    };
    ESP_GOTO_ON_ERROR(esp_isp_new_awb_controller(isp, &awb_config,
                                                  &white_balance->awb_controller),
                      fail, TAG, "creating ISP AWB statistics controller failed");
    const esp_isp_awb_cbs_t callbacks = {
        .on_statistics_done = awb_statistics_done,
    };
    ESP_GOTO_ON_ERROR(esp_isp_awb_register_event_callbacks(
                          white_balance->awb_controller, &callbacks, white_balance),
                      fail, TAG, "registering ISP AWB callback failed");
    ESP_GOTO_ON_ERROR(esp_isp_awb_controller_enable(white_balance->awb_controller),
                      fail, TAG, "enabling ISP AWB statistics failed");
    white_balance->awb_enabled = true;
    ESP_GOTO_ON_ERROR(esp_isp_awb_controller_start_continuous_statistics(
                          white_balance->awb_controller),
                      fail, TAG, "starting ISP AWB statistics failed");
    white_balance->awb_running = true;
    ESP_LOGI(TAG, "SC2336 auto white balance started with 1%% convergence threshold");
    return ESP_OK;

fail:
    (void)stop_white_balance(white_balance);
    return ret;
}

static esp_err_t update_white_balance(white_balance_context_t *white_balance)
{
    esp_isp_awb_evt_data_t event;
    if (xQueueReceive(white_balance->stats_queue, &event, 0) != pdTRUE) {
        return ESP_OK;
    }

    const isp_awb_stat_result_t *result = &event.awb_result;
    esp_ipa_stats_t stats = {
        .seq = ++white_balance->stats_sequence,
        .flags = IPA_STATS_FLAGS_AWB,
        .awb_stats = {{
            .counted = result->white_patch_num,
            .sum_r = result->sum_r,
            .sum_g = result->sum_g,
            .sum_b = result->sum_b,
        }},
    };
    esp_ipa_metadata_t metadata = {0};
    ESP_RETURN_ON_ERROR(esp_ipa_pipeline_process(white_balance->ipa_pipeline, &stats,
                                                 &white_balance->sensor_info, &metadata),
                        TAG, "processing AWB statistics failed");

    if ((metadata.flags & (IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG)) ==
        (IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG)) {
        const isp_wbg_gain_t gain = {
            .gain_r = (uint32_t)(metadata.red_gain * PROBE_WBG_UNITY_GAIN + 0.5f),
            .gain_g = PROBE_WBG_UNITY_GAIN,
            .gain_b = (uint32_t)(metadata.blue_gain * PROBE_WBG_UNITY_GAIN + 0.5f),
        };
        ESP_RETURN_ON_ERROR(esp_isp_wbg_set_wb_gain(white_balance->isp, gain), TAG,
                            "applying AWB gain failed");
        white_balance->red_gain = metadata.red_gain;
        white_balance->blue_gain = metadata.blue_gain;
    }

    if (white_balance->stats_sequence == 1 || white_balance->stats_sequence % 30 == 0) {
        const float measured_red_gain = result->sum_r == 0 ? 0.0f :
            (float)result->sum_g / result->sum_r;
        const float measured_blue_gain = result->sum_b == 0 ? 0.0f :
            (float)result->sum_g / result->sum_b;
        ESP_LOGI(TAG,
                 "AWB seq=%" PRIu64 " patches=%" PRIu32
                 " measured_r=%.3f measured_b=%.3f applied_r=%.3f applied_b=%.3f",
                 white_balance->stats_sequence, result->white_patch_num,
                 measured_red_gain, measured_blue_gain,
                 white_balance->red_gain, white_balance->blue_gain);
    }
    return ESP_OK;
}

static bool camera_get_new_buffer(esp_cam_ctlr_handle_t handle,
                                  esp_cam_ctlr_trans_t *trans,
                                  void *user_data)
{
    (void)handle;
    capture_context_t *capture = user_data;
    BaseType_t wake = pdFALSE;
    void *buffer = NULL;
    if (xQueueReceiveFromISR(capture->free_frames, &buffer, &wake) != pdTRUE &&
        xQueueReceiveFromISR(capture->completed_frames, &buffer, &wake) == pdTRUE) {
        ++capture->dropped_frame_count;
    }
    /* Never recycle the frame currently owned by the processing task. If no
     * owned buffer is available, CSI uses its existing internal backup. */
    trans->buffer = buffer;
    trans->buflen = buffer ? SENSOR_FRAME_BYTES : 0;
    ++capture->new_buffer_count;
    return wake == pdTRUE;
}

static bool camera_frame_finished(esp_cam_ctlr_handle_t handle,
                                  esp_cam_ctlr_trans_t *trans,
                                  void *user_data)
{
    (void)handle;
    capture_context_t *capture = user_data;
    ++capture->finished_frame_count;
    BaseType_t higher_priority_task_woken = pdFALSE;
    void *stale = NULL;
    if (xQueueReceiveFromISR(capture->completed_frames, &stale,
                              &higher_priority_task_woken) == pdTRUE) {
        ++capture->dropped_frame_count;
        BaseType_t released = xQueueSendFromISR(capture->free_frames, &stale,
                                                &higher_priority_task_woken);
        assert(released == pdTRUE);
    }
    xQueueOverwriteFromISR(capture->completed_frames, &trans->buffer,
                           &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static esp_err_t detect_and_start_sensor(sensor_context_t *context)
{
    context->i2c_bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(context->i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "BSP shared I2C bus is unavailable");

    esp_cam_sensor_config_t sensor_config = {
        .reset_pin = -1,
        .pwdn_pin = -1,
        .xclk_pin = -1,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };

    for (esp_cam_sensor_detect_fn_t *detect = &__esp_cam_sensor_detect_fn_array_start;
         detect < &__esp_cam_sensor_detect_fn_array_end; ++detect) {
        const sccb_i2c_config_t sccb_config = {
            .scl_speed_hz = 100000,
            .device_address = detect->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        esp_err_t err = sccb_new_i2c_io(context->i2c_bus, &sccb_config, &context->sccb);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SCCB candidate 0x%02x could not be opened: %s",
                     detect->sccb_addr, esp_err_to_name(err));
            continue;
        }

        sensor_config.sccb_handle = context->sccb;
        sensor_config.sensor_port = detect->port;
        context->sensor = detect->detect(&sensor_config);
        if (context->sensor != NULL && detect->port == ESP_CAM_SENSOR_MIPI_CSI) {
            ESP_LOGI(TAG, "MIPI camera detected at SCCB address 0x%02x", detect->sccb_addr);
            break;
        }

        context->sensor = NULL;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sccb_del_i2c_io(context->sccb));
        context->sccb = NULL;
    }

    if (context->sensor == NULL) {
        ESP_LOGE(TAG, "no compatible MIPI camera sensor detected");
        return ESP_ERR_NOT_FOUND;
    }

    esp_cam_sensor_format_array_t formats = {0};
    ESP_RETURN_ON_ERROR(esp_cam_sensor_query_format(context->sensor, &formats), TAG,
                        "querying sensor formats failed");

    const esp_cam_sensor_format_t *selected = NULL;
    for (int index = 0; index < formats.count; ++index) {
        const esp_cam_sensor_format_t *format = &formats.format_array[index];
        ESP_LOGI(TAG, "sensor format[%d]=%s", index, format->name);
        if (strcmp(format->name, PROBE_SENSOR_FORMAT) == 0) {
            selected = format;
        }
    }
    if (selected == NULL) {
        ESP_LOGE(TAG, "required format is unavailable: %s", PROBE_SENSOR_FORMAT);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(context->sensor, selected), TAG,
                        "selecting sensor format failed");
    ESP_LOGI(TAG, "selected sensor format=%s", selected->name);

    int horizontal_mirror = PROBE_SENSOR_HMIRROR;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_para_value(
                            context->sensor, ESP_CAM_SENSOR_HMIRROR,
                            &horizontal_mirror, sizeof(horizontal_mirror)),
                        TAG, "setting sensor horizontal mirror failed");
    ESP_LOGI(TAG, "sensor horizontal mirror enabled; Bayer order remains BGGR");

    int stream_enabled = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(context->sensor, ESP_CAM_SENSOR_IOC_S_STREAM,
                                             &stream_enabled), TAG,
                        "starting sensor stream failed");
    return ESP_OK;
}

static esp_err_t stop_sensor(sensor_context_t *context)
{
    if (context->sensor != NULL) {
        int stream_enabled = 0;
        ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(
            context->sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_enabled), TAG,
            "sensor stop failed");
        ESP_RETURN_ON_ERROR(esp_cam_sensor_del_dev(context->sensor), TAG,
                            "sensor delete failed");
        context->sensor = NULL;
    }
    if (context->sccb != NULL) {
        ESP_RETURN_ON_ERROR(esp_sccb_del_i2c_io(context->sccb), TAG,
                            "SCCB delete failed");
    }
    /* Do not delete the BSP-owned touch/audio/SCCB I2C bus. */
    memset(context, 0, sizeof(*context));
    return ESP_OK;
}

static void return_to_voice(lv_event_t *event)
{
    (void)event;
    if (!stop_requested) {
        stop_requested = true;
        lv_label_set_text(preview_status, "正在返回语音...");
        ESP_LOGI(TAG, "return requested; stopping preview without reboot");
    }
}

static void preview_set_status(const char *text)
{
    if (bsp_display_lock(0)) {
        lv_label_set_text(preview_status, text);
        bsp_display_unlock();
    }
}

static void create_preview_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    preview_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    preview_status = lv_label_create(screen);
    lv_obj_set_style_text_font(preview_status, preview_font, 0);
    lv_obj_set_style_text_color(preview_status, lv_color_white(), 0);
    lv_label_set_text(preview_status, "正在启动摄像头...");
    lv_obj_set_size(preview_status, 400, 48);
    lv_label_set_long_mode(preview_status, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(preview_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(preview_status, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_obj_t *back = lv_button_create(screen);
    lv_obj_set_size(back, 150, 48);
    /* Theme shadows must not spill into the constantly refreshed preview. */
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_outline_width(back, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_t *label = lv_label_create(back);
    lv_obj_set_style_text_font(label, preview_font, 0);
    lv_label_set_text(label, "返回语音");
    lv_obj_center(label);
    lv_obj_add_event_cb(back, return_to_voice, LV_EVENT_CLICKED, NULL);
    lv_screen_load(screen);
    /* Keep the voice screen alive: existing cloud callbacks own its labels. */
}

static void preview_task(void *arg)
{
    (void)arg;
    esp_err_t ret = ESP_OK;
    sensor_context_t sensor = {0};

    const esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = SENSOR_WIDTH,
        .v_res = SENSOR_HEIGHT,
        .lane_bit_rate_mbps = PROBE_CSI_LANE_BIT_RATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num = PROBE_CSI_LANE_COUNT,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    esp_cam_ctlr_handle_t camera = NULL;
    isp_proc_handle_t isp = NULL;
    bool camera_enabled = false;
    bool camera_started = false;
    bool isp_enabled = false;
    bool gamma_enabled = false;
    white_balance_context_t white_balance = {0};
    capture_context_t capture = {0};

    ESP_GOTO_ON_ERROR(prepare_voice(), cleanup, TAG, "voice handoff failed");
    if (stop_requested) {
        goto cleanup;
    }

    ESP_GOTO_ON_ERROR(detect_and_start_sensor(&sensor), cleanup, TAG,
                      "sensor startup failed");

    capture.completed_frames = xQueueCreate(1, sizeof(void *));
    capture.free_frames = xQueueCreate(CAPTURE_BUFFER_COUNT, sizeof(void *));
    ESP_GOTO_ON_FALSE(capture.completed_frames != NULL && capture.free_frames != NULL,
                      ESP_ERR_NO_MEM, cleanup, TAG,
                      "allocating completed frame queue failed");
    for (uint32_t index = 0; index < CAPTURE_BUFFER_COUNT; ++index) {
        capture.capture_buffers[index] = heap_caps_aligned_alloc(
            64, SENSOR_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        ESP_GOTO_ON_FALSE(capture.capture_buffers[index] != NULL, ESP_ERR_NO_MEM,
                          cleanup, TAG, "allocating full-resolution capture buffer failed");
        xQueueSend(capture.free_frames, &capture.capture_buffers[index], 0);
    }
    ESP_LOGI(TAG, "%d full-resolution capture buffers=%u bytes each free_psram=%u",
             CAPTURE_BUFFER_COUNT, (unsigned)SENSOR_FRAME_BYTES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ESP_GOTO_ON_ERROR(esp_cam_new_csi_ctlr(&csi_config, &camera), cleanup, TAG,
                      "CSI controller initialization failed");

    const esp_cam_ctlr_evt_cbs_t callbacks = {
        .on_get_new_trans = camera_get_new_buffer,
        .on_trans_finished = camera_frame_finished,
    };
    ESP_GOTO_ON_ERROR(esp_cam_ctlr_register_event_callbacks(camera, &callbacks, &capture),
                      cleanup, TAG, "CSI callback registration failed");
    ESP_GOTO_ON_ERROR(esp_cam_ctlr_enable(camera), cleanup, TAG,
                      "CSI controller enable failed");
    camera_enabled = true;

    const esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80000000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = SENSOR_WIDTH,
        .v_res = SENSOR_HEIGHT,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR,
    };
    ESP_GOTO_ON_ERROR(esp_isp_new_processor(&isp_config, &isp), cleanup, TAG,
                      "ISP initialization failed");
    ESP_GOTO_ON_ERROR(esp_isp_enable(isp), cleanup, TAG, "ISP enable failed");
    isp_enabled = true;
    ESP_GOTO_ON_ERROR(configure_preview_tone(isp), cleanup, TAG,
                      "preview tone initialization failed");
    gamma_enabled = true;
    ESP_GOTO_ON_ERROR(start_white_balance(isp, &white_balance), cleanup, TAG,
                      "automatic white balance initialization failed");
    ESP_GOTO_ON_ERROR(esp_cam_ctlr_start(camera), cleanup, TAG, "CSI stream start failed");
    camera_started = true;
    /* on_get_new_trans supplies every DMA destination, including startup. */

    const int64_t started_us = esp_timer_get_time();
    int64_t last_report_us = started_us;
    int64_t last_frame_us = started_us;
    uint32_t last_observed_frame = 0;
    bool stability_passed = false;
    uint64_t cache_time_us = 0;
    uint64_t display_time_us = 0;
    uint32_t report_shown = 0;
    while (ret == ESP_OK && !stop_requested) {
        void *completed_frame = NULL;
        if (xQueueReceive(capture.completed_frames, &completed_frame,
                          pdMS_TO_TICKS(20)) == pdTRUE) {
            int64_t stage_us = esp_timer_get_time();
            ESP_GOTO_ON_ERROR(esp_cache_msync(completed_frame, SENSOR_FRAME_BYTES,
                                              ESP_CACHE_MSYNC_FLAG_DIR_M2C),
                              cleanup, TAG, "invalidating captured frame cache failed");
            cache_time_us += esp_timer_get_time() - stage_us;
            stage_us = esp_timer_get_time();
            if (bsp_display_lock(0)) {
                if (capture.displayed_frame_count == 0) {
                    lv_label_set_text(preview_status, "手语预览（识别模型待接入）");
                }
                /* Finish header updates before composing a complete native
                 * camera frame. No LVGL image copy or second rotation. */
                lv_refr_now(NULL);
                const lv_area_t area = {PREVIEW_OFFSET_X, PREVIEW_OFFSET_Y,
                    PREVIEW_OFFSET_X + PREVIEW_WIDTH - 1,
                    PREVIEW_OFFSET_Y + PREVIEW_HEIGHT - 1};
                ret = portrait_preview_frame(completed_frame, SENSOR_WIDTH, SENSOR_HEIGHT,
                                               &area, PPA_SRM_ROTATION_ANGLE_90);
                bsp_display_unlock();
                if (ret == ESP_OK) ++capture.displayed_frame_count;
            }
            /* The blocking compositor no longer retains or reads the source. */
            ESP_GOTO_ON_FALSE(xQueueSend(capture.free_frames, &completed_frame, 0) == pdTRUE,
                              ESP_ERR_INVALID_STATE, cleanup, TAG, "capture release failed");
            ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "direct preview failed");
            display_time_us += esp_timer_get_time() - stage_us;
        }
        ret = update_white_balance(&white_balance);
        if (ret != ESP_OK) {
            break;
        }
        const int64_t now_us = esp_timer_get_time();
        const uint32_t frames = capture.finished_frame_count;

        if (frames != last_observed_frame) {
            last_observed_frame = frames;
            last_frame_us = now_us;
        } else if (now_us - last_frame_us >= PREVIEW_STALL_TIMEOUT_US) {
            ret = ESP_ERR_TIMEOUT;
            ESP_LOGE(TAG, "camera frame stream stalled for three seconds");
            break;
        }

        if (now_us - last_report_us >= 1000000LL) {
            const uint32_t elapsed_ms = (uint32_t)((now_us - started_us) / 1000LL);
            const uint32_t fps_x100 = elapsed_ms == 0 ? 0 :
                (uint32_t)(((uint64_t)frames * 100000ULL) / elapsed_ms);
            ESP_LOGI(TAG,
                     "frames=%" PRIu32 " shown=%" PRIu32 " elapsed_ms=%" PRIu32
                     " capture_fps=%" PRIu32 ".%02" PRIu32
                     " dropped=%" PRIu32 " free_psram=%u",
                     frames, capture.displayed_frame_count, elapsed_ms,
                     fps_x100 / 100, fps_x100 % 100,
                     capture.dropped_frame_count,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            const uint32_t interval_shown = capture.displayed_frame_count - report_shown;
            const uint32_t shown_fps_x100 = (uint32_t)(
                (uint64_t)interval_shown * 100000000ULL / (now_us - last_report_us));
            ESP_LOGI(TAG, "timing_direct: shown_fps=%" PRIu32 ".%02" PRIu32
                     " cache_us=%" PRIu64 " compose_us=%" PRIu64,
                     shown_fps_x100 / 100, shown_fps_x100 % 100,
                     interval_shown ? cache_time_us / interval_shown : 0,
                     interval_shown ? display_time_us / interval_shown : 0);
            report_shown = capture.displayed_frame_count;
            cache_time_us = display_time_us = 0;
            last_report_us = now_us;
        }

        if (!stability_passed &&
            now_us - started_us >= (int64_t)PROBE_SECONDS * 1000000LL) {
            stability_passed = true;
            ESP_LOGI(TAG,
                     "PASS: displayed %" PRIu32 " camera frames during %d-second preview; continuing",
                     capture.displayed_frame_count, PROBE_SECONDS);
        }
    }

cleanup:
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "probe stopped after error: %s", esp_err_to_name(ret));
    }
    if (camera_started) {
        ESP_GOTO_ON_ERROR(esp_cam_ctlr_stop(camera), cleanup_failed, TAG, "CSI stop failed");
    }
    ESP_GOTO_ON_ERROR(stop_white_balance(&white_balance), cleanup_failed, TAG,
                      "AWB cleanup failed");
    if (camera_enabled) {
        ESP_GOTO_ON_ERROR(esp_cam_ctlr_disable(camera), cleanup_failed, TAG,
                          "CSI disable failed");
    }
    if (camera != NULL) {
        ESP_GOTO_ON_ERROR(esp_cam_ctlr_del(camera), cleanup_failed, TAG, "CSI delete failed");
    }
    if (gamma_enabled) {
        ESP_GOTO_ON_ERROR(esp_isp_gamma_disable(isp), cleanup_failed, TAG,
                          "preview tone disable failed");
    }
    if (isp_enabled) {
        ESP_GOTO_ON_ERROR(esp_isp_disable(isp), cleanup_failed, TAG, "ISP disable failed");
    }
    if (isp != NULL) {
        ESP_GOTO_ON_ERROR(esp_isp_del_processor(isp), cleanup_failed, TAG,
                          "ISP delete failed");
    }
    ESP_GOTO_ON_ERROR(stop_sensor(&sensor), cleanup_failed, TAG, "sensor cleanup failed");
    if (capture.completed_frames != NULL) {
        vQueueDelete(capture.completed_frames);
    }
    if (capture.free_frames != NULL) {
        vQueueDelete(capture.free_frames);
    }
    for (uint32_t index = 0; index < CAPTURE_BUFFER_COUNT; ++index) {
        heap_caps_free(capture.capture_buffers[index]);
    }
    /* Stop preserving the direct camera layer before returning to voice. */
    ESP_GOTO_ON_FALSE(bsp_display_lock(0), ESP_ERR_TIMEOUT, cleanup_failed, TAG,
                      "preview detach lock failed");
    {
        portrait_preview_stop();
        lv_obj_invalidate(preview_screen);
        if (!stop_requested) {
            lv_label_set_text(preview_status, "摄像头启动或预览失败，请返回后重试");
        }
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "preview resources released; free_psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* Even a startup error retains a usable back button, with no live DMA. */
    while (!stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_GOTO_ON_FALSE(bsp_display_lock(0), ESP_ERR_TIMEOUT, cleanup_failed, TAG,
                      "voice screen restore lock failed");
    {
        lv_screen_load(voice_screen);
        lv_obj_delete(preview_screen);
        preview_screen = preview_status = NULL;
        /* Keep touch callbacks out until the voice state has been restored. */
        mode_active = false;
        resume_voice();
        bsp_display_unlock();
    }
    vTaskDelete(NULL);
    return;

cleanup_failed:
    /* Driver ownership is uncertain. Preserve the worker stack, ISR contexts
     * and buffers instead of risking use-after-free or automatic reboot. */
    preview_set_status("摄像头停止失败，请手动重启");
    ESP_LOGE(TAG, "preview cleanup failed; resources retained for safety");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


bool sign_language_mode_active(void)
{
    return mode_active;
}

esp_err_t sign_language_mode_start(const lv_font_t *font,
                                   sign_language_prepare_fn prepare,
                                   sign_language_resume_fn resume)
{
    if (mode_active || font == NULL || prepare == NULL || resume == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    mode_active = true;
    stop_requested = false;
    voice_screen = lv_screen_active();
    preview_font = font;
    prepare_voice = prepare;
    resume_voice = resume;
    /* Called from LVGL: its recursive lock already protects screen creation. */
    create_preview_screen();
    if (xTaskCreate(preview_task, "sign_preview", 16384, NULL, 3, NULL) != pdPASS) {
        lv_screen_load(voice_screen);
        lv_obj_delete(preview_screen);
        preview_screen = preview_status = NULL;
        mode_active = false;
        /* No voice handoff happened, so the existing RTC session is intact. */
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
