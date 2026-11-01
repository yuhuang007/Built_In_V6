#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "p4_pinyin.h"
typedef struct {
    uint32_t revision;
    int error;
    bool initialized;
    p4_pinyin_result_t result;
} pinyin_response_t;
/* All request data is copied. Only the worker touches the decoder and SD. */
bool pinyin_service_request(uint32_t revision, const char *spelling, unsigned offset, int choose);
bool pinyin_service_poll(pinyin_response_t *response);
/* Startup validates/loads an installed dictionary without network activity. */
bool pinyin_service_preload(void);
enum { PINYIN_IDLE, PINYIN_LOADING, PINYIN_READY, PINYIN_FAILED };
int pinyin_service_state(void);
