/* Protocol verified against the official sauc_python/protocol.py example.
 * Only numeric diagnostics are emitted. Never log authentication or payloads. */
#include "doubao_asr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_tls.h"
#include "zlib.h"
#include "../../../secrets/doubao_asr.h"

#define PACKET_CAPACITY (64 * 1024)
#define JSON_CAPACITY (128 * 1024)
#define SEND_CAPACITY 8192
#define IO_TIMEOUT_MS 5000
/* ASR Upgrade response measured at 1228 bytes; IDF's default 1024-byte
 * buffer rejects it before parsing HTTP status. Leave room for header growth. */
_Static_assert(CONFIG_WS_BUFFER_SIZE >= 4096, "ASR requires a 4096-byte WS handshake buffer");
static const char *TAG = "doubao_asr";

struct doubao_asr_client {
    esp_transport_handle_t tls;
    esp_transport_handle_t ws;
    uint32_t sequence;
    uint8_t *packet;
    uint8_t *decoded;
    uint8_t *send_buffer;
    doubao_asr_text_fn on_text;
    void *context;
};

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = value >> 24; p[1] = value >> 16;
    p[2] = value >> 8; p[3] = value;
}

static esp_err_t send_packet(doubao_asr_client_t *client, const void *data,
                             size_t bytes, bool audio, bool last)
{
    if (bytes > 6400 || client->sequence >= INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *packet = client->send_buffer;
    z_stream stream = {0};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return ESP_ERR_NO_MEM;
    }
    stream.next_in = (Bytef *)data;
    stream.avail_in = bytes;
    stream.next_out = packet + 12;
    stream.avail_out = SEND_CAPACITY - 12;
    const int result = deflate(&stream, Z_FINISH);
    const size_t payload_size = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        return ESP_ERR_INVALID_SIZE;
    }
    packet[0] = 0x11;
    packet[1] = (audio ? 0x20 : 0x10) | (last ? 3 : 1);
    packet[2] = 0x11; /* Official example uses JSON serialization + gzip for both. */
    packet[3] = 0;
    const int32_t sequence = last ? -(int32_t)client->sequence : (int32_t)client->sequence;
    write_be32(packet + 4, (uint32_t)sequence);
    write_be32(packet + 8, payload_size);
    const size_t total = payload_size + 12;
    if (esp_transport_write(client->ws, (char *)packet, total, IO_TIMEOUT_MS) != total) {
        return ESP_FAIL; /* Never retry a partially written WebSocket frame. */
    }
    ++client->sequence;
    return ESP_OK;
}

static esp_err_t parse_packet(doubao_asr_client_t *client, size_t bytes, bool *last)
{
    const uint8_t *p = client->packet;
    if (bytes < 4 || (p[0] >> 4) != 1 || !(p[0] & 15)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const unsigned type = p[1] >> 4;
    const unsigned flags = p[1] & 15;
    const unsigned serialization = p[2] >> 4;
    const unsigned compression = p[2] & 15;
    size_t offset = (p[0] & 15) * 4;
    if (flags & 1) offset += 4;
    if (flags & 4) offset += 4;
    if (offset > bytes) return ESP_ERR_INVALID_RESPONSE;
    uint32_t code = 0;
    if (type == 15) {
        if (bytes - offset < 4) return ESP_ERR_INVALID_RESPONSE;
        code = read_be32(p + offset);
        offset += 4;
    } else if (type != 9) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (bytes - offset < 4) return ESP_ERR_INVALID_RESPONSE;
    const size_t payload_size = read_be32(p + offset);
    offset += 4;
    if (payload_size != bytes - offset) return ESP_ERR_INVALID_RESPONSE;
    if (code) {
        ESP_LOGW(TAG, "server_error=%lu", (unsigned long)code);
        return ESP_FAIL;
    }
    *last = (flags & 2) != 0;
    if (!payload_size) return ESP_OK;
    size_t decoded_size = payload_size;
    if (compression == 1) {
        z_stream stream = {0};
        if (inflateInit2(&stream, 31) != Z_OK) return ESP_ERR_NO_MEM;
        stream.next_in = (Bytef *)p + offset;
        stream.avail_in = payload_size;
        stream.next_out = client->decoded;
        stream.avail_out = JSON_CAPACITY - 1;
        const int status = inflate(&stream, Z_FINISH);
        decoded_size = stream.total_out;
        const bool consumed = stream.avail_in == 0;
        inflateEnd(&stream);
        if (status != Z_STREAM_END || !consumed) return ESP_ERR_INVALID_RESPONSE;
    } else if (compression == 0 && payload_size < JSON_CAPACITY) {
        memcpy(client->decoded, p + offset, payload_size);
    } else {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (serialization != 1) return ESP_ERR_INVALID_RESPONSE;
    client->decoded[decoded_size] = 0;
    cJSON *root = cJSON_ParseWithLength((char *)client->decoded, decoded_size + 1);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "code");
    esp_err_t result = ESP_OK;
    if (cJSON_IsNumber(status) && status->valuedouble != 0) {
        ESP_LOGW(TAG, "payload_error=%.0f", status->valuedouble);
        result = ESP_FAIL;
    } else {
        const cJSON *recognition = cJSON_GetObjectItemCaseSensitive(root, "result");
        if (cJSON_IsArray(recognition)) recognition = cJSON_GetArrayItem(recognition, 0);
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(recognition, "text");
        if (cJSON_IsString(text) && text->valuestring[0]) {
            client->on_text(text->valuestring, client->context);
        }
    }
    cJSON_Delete(root);
    return result;
}

esp_err_t doubao_asr_receive(doubao_asr_client_t *client, int wait_ms, bool *last)
{
    *last = false;
    /* Propagate control frames explicitly; assemble partial TLS reads and
     * RFC6455 continuation frames before parsing a single ASR packet. */
    if (esp_transport_poll_read(client->ws, wait_ms) == 0) return ESP_ERR_TIMEOUT;
    size_t message_size = 0;
    bool continuation = false;
    const int64_t deadline = esp_timer_get_time() + IO_TIMEOUT_MS * 1000LL;
    do {
        if (esp_timer_get_time() >= deadline) return ESP_FAIL;
        int got = esp_transport_read(client->ws, (char *)client->packet + message_size,
                                     PACKET_CAPACITY - message_size, IO_TIMEOUT_MS);
        if (got < 0) return ESP_FAIL;
        const unsigned opcode = esp_transport_ws_get_read_opcode(client->ws);
        const int payload = esp_transport_ws_get_read_payload_len(client->ws);
        if (payload < 0 || (size_t)payload > PACKET_CAPACITY - message_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        size_t frame_size = got;
        while (frame_size < (size_t)payload) {
            got = esp_transport_read(client->ws,
                (char *)client->packet + message_size + frame_size,
                payload - frame_size, IO_TIMEOUT_MS);
            if (got <= 0) return ESP_FAIL;
            frame_size += got;
        }
        if (opcode == WS_TRANSPORT_OPCODES_CLOSE) return ESP_FAIL;
        if (opcode == WS_TRANSPORT_OPCODES_PING) {
            if (esp_transport_ws_send_raw(client->ws,
                WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN,
                (char *)client->packet + message_size, frame_size, IO_TIMEOUT_MS) != frame_size) {
                return ESP_FAIL;
            }
            if (!message_size) return ESP_ERR_TIMEOUT;
            continue;
        }
        if (opcode == WS_TRANSPORT_OPCODES_PONG) {
            if (!message_size) return ESP_ERR_TIMEOUT;
            continue;
        }
        if (opcode != (continuation ? WS_TRANSPORT_OPCODES_CONT : WS_TRANSPORT_OPCODES_BINARY)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        message_size += frame_size;
        continuation = !esp_transport_ws_get_fin_flag(client->ws);
        if (continuation && message_size == PACKET_CAPACITY) return ESP_ERR_INVALID_SIZE;
    } while (continuation);
    return parse_packet(client, message_size, last);
}

esp_err_t doubao_asr_open(doubao_asr_client_t **out,
                         doubao_asr_text_fn on_text, void *context)
{
    *out = NULL;
    if (!on_text || !DOUBAO_ASR_API_KEY[0]) return ESP_ERR_INVALID_ARG;
    /* IDF's ws_connect error path can log its complete Upgrade request,
     * including custom authentication headers. Suppress that tag at all levels. */
    esp_log_level_set("transport_ws", ESP_LOG_NONE);
    doubao_asr_client_t *client = calloc(1, sizeof(*client));
    if (!client) return ESP_ERR_NO_MEM;
    client->on_text = on_text; client->context = context; client->sequence = 1;
    client->packet = heap_caps_malloc(PACKET_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    client->decoded = heap_caps_malloc(JSON_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    client->send_buffer = malloc(SEND_CAPACITY);
    client->tls = esp_transport_ssl_init();
    if (client->tls) client->ws = esp_transport_ws_init(client->tls);
    esp_err_t result = ESP_ERR_NO_MEM;
    if (!client->packet || !client->decoded || !client->send_buffer || !client->ws) goto fail;
    esp_transport_ssl_crt_bundle_attach(client->tls, esp_crt_bundle_attach);
    uint8_t random_id[16];
    esp_fill_random(random_id, sizeof(random_id));
    random_id[6] = (random_id[6] & 15) | 0x40;
    random_id[8] = (random_id[8] & 63) | 0x80;
    char request_id[37];
    snprintf(request_id, sizeof(request_id),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        random_id[0],random_id[1],random_id[2],random_id[3],random_id[4],random_id[5],
        random_id[6],random_id[7],random_id[8],random_id[9],random_id[10],random_id[11],
        random_id[12],random_id[13],random_id[14],random_id[15]);
    char headers[512];
    const int length = snprintf(headers, sizeof(headers),
        "X-Api-Key: %s\r\nX-Api-Resource-Id: %s\r\nX-Api-Request-Id: %s\r\n",
        DOUBAO_ASR_API_KEY, DOUBAO_ASR_RESOURCE_ID, request_id);
    if (length < 0 || length >= sizeof(headers)) { result = ESP_ERR_INVALID_SIZE; goto fail; }
    const esp_transport_ws_config_t config = {
        .ws_path = "/api/v3/sauc/bigmodel_async", .headers = headers,
        .propagate_control_frames = true,
    };
    result = esp_transport_ws_set_config(client->ws, &config);
    memset(headers, 0, sizeof(headers));
    if (result != ESP_OK) goto fail;
    if (esp_transport_connect(client->ws, "openspeech.bytedance.com", 443, 10000) < 0) {
        int tls_code = 0, tls_flags = 0;
        const esp_err_t tls_error = esp_tls_get_and_clear_last_error(
            esp_transport_get_error_handle(client->tls), &tls_code, &tls_flags);
        ESP_LOGW(TAG, "handshake_failed http_status=%d",
                 esp_transport_ws_get_upgrade_request_status(client->ws));
        ESP_LOGW(TAG, "tls_error=%d tls_code=%d tls_flags=%d ws_buffer=%d",
                 (int)tls_error, tls_code, tls_flags, CONFIG_WS_BUFFER_SIZE);
        result = ESP_FAIL; goto fail;
    }
    const char request[] = "{\"user\":{\"uid\":\"p4-asr-test\"},"
        "\"audio\":{\"format\":\"pcm\",\"codec\":\"raw\",\"rate\":16000,\"bits\":16,\"channel\":1},"
        "\"request\":{\"model_name\":\"bigmodel\",\"result_type\":\"full\","
        "\"enable_itn\":true,\"enable_punc\":true,\"enable_ddc\":false,"
        "\"show_utterances\":true,\"enable_nonstream\":true}}";
    result = send_packet(client, request, sizeof(request) - 1, false, false);
    if (result != ESP_OK) goto fail;
    *out = client;
    ESP_LOGI(TAG, "connected; format=pcm rate=16000 bits=16 channels=1");
    return ESP_OK;
fail:
    doubao_asr_close(client);
    return result;
}

esp_err_t doubao_asr_send(doubao_asr_client_t *client, const uint8_t *pcm,
                         size_t bytes, bool last)
{
    return send_packet(client, pcm, bytes, true, last);
}

void doubao_asr_close(doubao_asr_client_t *client)
{
    if (!client) return;
    if (client->ws) {
        esp_transport_close(client->ws);
        esp_transport_destroy(client->ws);
    }
    if (client->tls) esp_transport_destroy(client->tls);
    free(client->packet); free(client->decoded); free(client->send_buffer); free(client);
}
