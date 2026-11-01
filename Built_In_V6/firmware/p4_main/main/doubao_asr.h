#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct doubao_asr_client doubao_asr_client_t;
typedef void (*doubao_asr_text_fn)(const char *text, void *context);

/* One recognition request per connection. Logs never include headers/text. */
esp_err_t doubao_asr_open(doubao_asr_client_t **client,
                         doubao_asr_text_fn on_text, void *context);
esp_err_t doubao_asr_send(doubao_asr_client_t *client, const uint8_t *pcm,
                         size_t bytes, bool last);
esp_err_t doubao_asr_receive(doubao_asr_client_t *client, int wait_ms, bool *last);
void doubao_asr_close(doubao_asr_client_t *client);
