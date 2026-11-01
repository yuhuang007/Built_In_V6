/*
 * Isolated SC2336 bring-up probe for the ESP32-P4X Function EV Board.
 *
 * This program initializes only the SC2336 and the native MIPI-DSI display.
 * It intentionally does not initialize touch, audio, Wi-Fi, RTC, LVGL, or
 * the stable voice application.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bsp/display.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/isp_awb.h"
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
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SENSOR_WIDTH 1920
#define SENSOR_HEIGHT 1080
#define PREVIEW_WIDTH 960
#define PREVIEW_HEIGHT 540
#define PREVIEW_OFFSET_X 32
#define PREVIEW_OFFSET_Y 30
#define DISPLAY_WIDTH 1024
#define DISPLAY_HEIGHT 600
#define PROBE_BYTES_PER_PIXEL 2
#define SENSOR_FRAME_BYTES ((size_t)SENSOR_WIDTH * SENSOR_HEIGHT * PROBE_BYTES_PER_PIXEL)
#define DISPLAY_FRAME_BYTES ((size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * PROBE_BYTES_PER_PIXEL)
#define CAPTURE_BUFFER_COUNT 3
#define PROBE_SECONDS 60
#define PREVIEW_STALL_TIMEOUT_US 3000000LL
#define PROBE_CSI_LANE_COUNT 2
#define PROBE_CSI_LANE_BIT_RATE_MBPS 336
#define PROBE_SCCB_SDA_GPIO 7
#define PROBE_SCCB_SCL_GPIO 8
#define PROBE_SENSOR_FORMAT "MIPI_2lane_24Minput_RAW8_1920x1080_30fps"
#define PROBE_SENSOR_HMIRROR 1
#define PROBE_WBG_FRACTION_BITS 8
#define PROBE_WBG_UNITY_GAIN (1U << PROBE_WBG_FRACTION_BITS)

static const char *TAG = "sc2336_probe";

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    esp_sccb_io_handle_t sccb;
    esp_cam_sensor_device_t *sensor;
} sensor_context_t;

typedef struct {
    esp_cam_ctlr_trans_t transaction;
    void *capture_buffers[CAPTURE_BUFFER_COUNT];
    QueueHandle_t completed_frames;
    volatile uint32_t next_capture_buffer;
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

static void stop_white_balance(white_balance_context_t *white_balance)
{
    if (white_balance->awb_running) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_isp_awb_controller_stop_continuous_statistics(
            white_balance->awb_controller));
        white_balance->awb_running = false;
    }
    if (white_balance->awb_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_isp_awb_controller_disable(white_balance->awb_controller));
        white_balance->awb_enabled = false;
    }
    if (white_balance->awb_controller != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_isp_del_awb_controller(white_balance->awb_controller));
        white_balance->awb_controller = NULL;
    }
    if (white_balance->wbg_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_isp_wbg_disable(white_balance->isp));
        white_balance->wbg_enabled = false;
    }
    if (white_balance->ipa_pipeline != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_ipa_pipeline_destroy(white_balance->ipa_pipeline));
        white_balance->ipa_pipeline = NULL;
    }
    if (white_balance->stats_queue != NULL) {
        vQueueDelete(white_balance->stats_queue);
        white_balance->stats_queue = NULL;
    }
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
    stop_white_balance(white_balance);
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
    const uint32_t buffer_index = capture->next_capture_buffer;
    trans->buffer = capture->capture_buffers[buffer_index];
    trans->buflen = capture->transaction.buflen;
    capture->next_capture_buffer = (buffer_index + 1) % CAPTURE_BUFFER_COUNT;
    ++capture->new_buffer_count;
    return false;
}

static bool camera_frame_finished(esp_cam_ctlr_handle_t handle,
                                  esp_cam_ctlr_trans_t *trans,
                                  void *user_data)
{
    (void)handle;
    capture_context_t *capture = user_data;
    ++capture->finished_frame_count;
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (uxQueueMessagesWaitingFromISR(capture->completed_frames) != 0) {
        ++capture->dropped_frame_count;
    }
    xQueueOverwriteFromISR(capture->completed_frames, &trans->buffer,
                           &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static esp_err_t detect_and_start_sensor(sensor_context_t *context)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PROBE_SCCB_SDA_GPIO,
        .scl_io_num = PROBE_SCCB_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &context->i2c_bus), TAG,
                        "SCCB I2C bus initialization failed");

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

static void stop_sensor(sensor_context_t *context)
{
    if (context->sensor != NULL) {
        int stream_enabled = 0;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_cam_sensor_ioctl(
            context->sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_enabled));
    }
    if (context->sccb != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sccb_del_i2c_io(context->sccb));
    }
    if (context->i2c_bus != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_del_master_bus(context->i2c_bus));
    }
    memset(context, 0, sizeof(*context));
}

void app_main(void)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    bool display_initialized = false;

    ESP_LOGI(TAG, "isolated SC2336 display preview starting");
    ESP_LOGI(TAG, "expected=%s lanes=%d lane_rate=%dMbps",
             PROBE_SENSOR_FORMAT, PROBE_CSI_LANE_COUNT, PROBE_CSI_LANE_BIT_RATE_MBPS);

    const bsp_display_config_t display_config = {
        .hdmi_resolution = BSP_HDMI_RES_NONE,
        .dsi_bus = {
            .phy_clk_src = 0,
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };
    esp_err_t err = bsp_display_new(&display_config, &panel, &panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MIPI-DSI display initialization failed: %s", esp_err_to_name(err));
        return;
    }
    display_initialized = true;
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_display_brightness_set(70));
    ESP_LOGI(TAG, "display ready: %dx%d RGB565, double-buffered full-rate preview",
             BSP_LCD_H_RES, BSP_LCD_V_RES);

    void *display_frame_buffers[2] = {0};
    err = esp_lcd_dpi_panel_get_frame_buffer(
        panel, 2, &display_frame_buffers[0], &display_frame_buffers[1]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "retrieving DSI frame buffers failed: %s", esp_err_to_name(err));
        bsp_display_delete();
        return;
    }
    memset(display_frame_buffers[0], 0, DISPLAY_FRAME_BYTES);
    memset(display_frame_buffers[1], 0, DISPLAY_FRAME_BYTES);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_cache_msync(
        display_frame_buffers[0], DISPLAY_FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_cache_msync(
        display_frame_buffers[1], DISPLAY_FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_draw_bitmap(
        panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, display_frame_buffers[1]));

    sensor_context_t sensor = {0};
    err = detect_and_start_sensor(&sensor);
    if (err != ESP_OK) {
        stop_sensor(&sensor);
        bsp_display_delete();
        return;
    }

    ESP_LOGI(TAG, "two DSI frame buffers=%u bytes each free_psram=%u",
             (unsigned)DISPLAY_FRAME_BYTES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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
    ppa_client_handle_t ppa = NULL;
    white_balance_context_t white_balance = {0};
    capture_context_t capture = {0};
    uint32_t display_buffer_index = 0;

    capture.completed_frames = xQueueCreate(1, sizeof(void *));
    ESP_GOTO_ON_FALSE(capture.completed_frames != NULL, ESP_ERR_NO_MEM, cleanup, TAG,
                      "allocating completed frame queue failed");
    for (uint32_t index = 0; index < CAPTURE_BUFFER_COUNT; ++index) {
        capture.capture_buffers[index] = heap_caps_aligned_alloc(
            64, SENSOR_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        ESP_GOTO_ON_FALSE(capture.capture_buffers[index] != NULL, ESP_ERR_NO_MEM,
                          cleanup, TAG, "allocating full-resolution capture buffer failed");
    }
    capture.transaction.buffer = capture.capture_buffers[0];
    capture.transaction.buflen = SENSOR_FRAME_BYTES;
    capture.next_capture_buffer = 1;
    ESP_LOGI(TAG, "%d full-resolution capture buffers=%u bytes each free_psram=%u",
             CAPTURE_BUFFER_COUNT, (unsigned)SENSOR_FRAME_BYTES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_GOTO_ON_ERROR(ppa_register_client(&ppa_config, &ppa), cleanup, TAG,
                      "registering PPA scaler failed");
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
    ESP_GOTO_ON_ERROR(start_white_balance(isp, &white_balance), cleanup, TAG,
                      "automatic white balance initialization failed");
    ESP_GOTO_ON_ERROR(esp_cam_ctlr_start(camera), cleanup, TAG, "CSI stream start failed");
    ESP_GOTO_ON_ERROR(esp_cam_ctlr_receive(camera, &capture.transaction,
                                           ESP_CAM_CTLR_MAX_DELAY),
                      cleanup, TAG, "submitting CSI receive transaction failed");

    const int64_t started_us = esp_timer_get_time();
    int64_t last_report_us = started_us;
    int64_t last_frame_us = started_us;
    uint32_t last_observed_frame = 0;
    bool stability_passed = false;
    while (ret == ESP_OK) {
        void *completed_frame = NULL;
        if (xQueueReceive(capture.completed_frames, &completed_frame,
                          pdMS_TO_TICKS(20)) == pdTRUE) {
            ESP_GOTO_ON_ERROR(esp_cache_msync(completed_frame, SENSOR_FRAME_BYTES,
                                              ESP_CACHE_MSYNC_FLAG_DIR_M2C),
                              cleanup, TAG, "invalidating captured frame cache failed");
            ppa_srm_oper_config_t scale_config = {
                .in = {
                    .buffer = completed_frame,
                    .pic_w = SENSOR_WIDTH,
                    .pic_h = SENSOR_HEIGHT,
                    .block_w = SENSOR_WIDTH,
                    .block_h = SENSOR_HEIGHT,
                    .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                },
                .out = {
                    .buffer = display_frame_buffers[display_buffer_index],
                    .buffer_size = DISPLAY_FRAME_BYTES,
                    .pic_w = DISPLAY_WIDTH,
                    .pic_h = DISPLAY_HEIGHT,
                    .block_offset_x = PREVIEW_OFFSET_X,
                    .block_offset_y = PREVIEW_OFFSET_Y,
                    .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                },
                .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
                .scale_x = 0.5f,
                .scale_y = 0.5f,
                .mode = PPA_TRANS_MODE_BLOCKING,
            };
            ESP_GOTO_ON_ERROR(ppa_do_scale_rotate_mirror(ppa, &scale_config), cleanup,
                              TAG, "scaling full-view frame failed");
            ESP_GOTO_ON_ERROR(esp_lcd_panel_draw_bitmap(
                                  panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                  display_frame_buffers[display_buffer_index]),
                              cleanup, TAG, "DSI frame switch failed");
            display_buffer_index ^= 1;
            ++capture.displayed_frame_count;
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
    stop_white_balance(&white_balance);
    if (camera != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_cam_ctlr_stop(camera));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_cam_ctlr_disable(camera));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_cam_ctlr_del(camera));
    }
    if (ppa != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ppa_unregister_client(ppa));
    }
    if (isp != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_isp_disable(isp));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_isp_del_processor(isp));
    }
    if (capture.completed_frames != NULL) {
        vQueueDelete(capture.completed_frames);
    }
    for (uint32_t index = 0; index < CAPTURE_BUFFER_COUNT; ++index) {
        heap_caps_free(capture.capture_buffers[index]);
    }
    stop_sensor(&sensor);
    if (display_initialized) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_display_backlight_off());
        bsp_display_delete();
    }
    ESP_LOGI(TAG, "preview stopped; power-cycle or reset before another run");
}
