#include "pinyin_service.h"
#include "pinyin_asset.h"
#include <string.h>
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

typedef struct {
    uint32_t revision;
    unsigned offset;
    int choose;
    bool allow_install;
    char spelling[P4_PINYIN_MAX + 1];
} request_t;
static QueueHandle_t requests, responses;
static atomic_int state;
int pinyin_service_state(void) { return atomic_load(&state); }

static void worker(void *unused)
{
    (void)unused;
    bool initialized = false, attempted = false;
    unsigned queries = 0;
    request_t request;
    while (xQueueReceive(requests, &request, portMAX_DELAY) == pdTRUE) {
        pinyin_response_t response = {.revision = request.revision};
        if (!initialized && (!attempted || !request.spelling[0])) {
            attempted = true;
            atomic_store(&state, PINYIN_LOADING);
            esp_err_t error = pinyin_asset_ensure(request.allow_install);
            size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            if (error == ESP_OK) initialized = p4_pinyin_open(PINYIN_ASSET_PATH);
            if (error == ESP_OK && !initialized) error = ESP_FAIL;
            ESP_LOGI("pinyin", "init_error=%d ready=%d heap_used=%u", (int)error,
                     initialized, (unsigned)(before - heap_caps_get_free_size(MALLOC_CAP_8BIT)));
            response.error = error;
            atomic_store(&state, initialized ? PINYIN_READY : PINYIN_FAILED);
        }
        response.initialized = initialized;
        if (!initialized && !response.error) response.error = ESP_ERR_INVALID_STATE;
        int64_t started = esp_timer_get_time();
        if (initialized && !p4_pinyin_query(request.spelling, request.offset, request.choose, &response.result))
            response.error = ESP_FAIL;
        if (initialized && (++queries == 1 || request.choose >= 0 || queries % 16 == 0))
            ESP_LOGI("pinyin", "query_count=%u elapsed_us=%u candidates=%u selected=%d error=%d",
                     queries, (unsigned)(esp_timer_get_time() - started), response.result.total,
                     request.choose >= 0, response.error);
        xQueueOverwrite(responses, &response);
    }
}

static bool request_work(uint32_t revision, const char *spelling, unsigned offset, int choose, bool allow_install)
{
    if (!spelling || strlen(spelling) > P4_PINYIN_MAX) return false;
    if (!requests) {
        requests = xQueueCreate(1, sizeof(request_t));
        responses = xQueueCreate(1, sizeof(pinyin_response_t));
        if (!requests || !responses || xTaskCreate(worker, "pinyin", 12288, NULL, 3, NULL) != pdPASS) {
            if (requests) vQueueDelete(requests);
            if (responses) vQueueDelete(responses);
            requests = responses = NULL;
            return false;
        }
    }
    request_t request = {.revision = revision, .offset = offset, .choose = choose, .allow_install = allow_install};
    strcpy(request.spelling, spelling);
    return xQueueOverwrite(requests, &request) == pdTRUE;
}

bool pinyin_service_request(uint32_t revision, const char *spelling, unsigned offset, int choose)
{
    return request_work(revision, spelling, offset, choose, true);
}

bool pinyin_service_preload(void)
{
    atomic_store(&state, PINYIN_LOADING);
    bool started = request_work(0, "", 0, -1, false);
    if (!started) atomic_store(&state, PINYIN_FAILED);
    return started;
}

bool pinyin_service_poll(pinyin_response_t *response)
{
    return responses && response && xQueueReceive(responses, response, 0) == pdTRUE;
}
