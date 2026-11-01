#include "cloud_text.h"
#include "cJSON.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int cloud_text_send(volc_engine_t engine, const char *text)
{
    if (!engine || !text || !text[0] || strlen(text) > 1500) return -1;
    cJSON *command = cJSON_CreateObject();
    if (!command) return -1;
    char *json = NULL;
    uint8_t *packet = NULL;
    int result = -1;
    if (!cJSON_AddStringToObject(command, "Command", "ExternalTextToLLM") ||
        !cJSON_AddStringToObject(command, "Message", text) ||
        !cJSON_AddNumberToObject(command, "InterruptMode", 2)) goto done;
    json = cJSON_PrintUnformatted(command);
    if (!json) goto done;
    const size_t length = strlen(json);
    packet = malloc(length + 8);
    if (!packet) goto done;
    /* Pinned vendor RTC control framing: ctrl + uint32 big-endian + JSON.
     * Its text convenience helper leaks its printed JSON; leave SDK untouched. */
    memcpy(packet, "ctrl", 4);
    for (unsigned i = 0; i < 4; ++i) packet[4 + i] = (uint8_t)(length >> (24 - 8 * i));
    memcpy(packet + 8, json, length);
    volc_message_info_t info = {.is_binary = true};
    result = volc_send_message(engine, packet, length + 8, &info);
done:
    free(packet);
    cJSON_free(json);
    cJSON_Delete(command);
    return result;
}
