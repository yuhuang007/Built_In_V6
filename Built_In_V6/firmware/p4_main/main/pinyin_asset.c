#include "pinyin_asset.h"
#include "pinyin_asset_meta.h"
#include "../../../secrets/sd_font_transfer.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static bool digest_matches(const uint8_t *digest)
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i)
        if (PINYIN_ASSET_SHA256[i * 2] != hex[digest[i] >> 4] ||
            PINYIN_ASSET_SHA256[i * 2 + 1] != hex[digest[i] & 15]) return false;
    return true;
}

static esp_err_t verify(void)
{
    FILE *file = fopen(PINYIN_ASSET_PATH, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    uint8_t buffer[2048], digest[32];
    size_t total = 0;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    esp_err_t error = mbedtls_sha256_starts(&sha, 0) ? ESP_FAIL : ESP_OK;
    while (error == ESP_OK) {
        size_t got = fread(buffer, 1, sizeof(buffer), file);
        if (!got) break;
        total += got;
        if (total > PINYIN_ASSET_BYTES || mbedtls_sha256_update(&sha, buffer, got)) error = ESP_FAIL;
        if (total % 16384 == 0) vTaskDelay(1);
    }
    if (ferror(file) || total != PINYIN_ASSET_BYTES) error = ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK && (mbedtls_sha256_finish(&sha, digest) || !digest_matches(digest)))
        error = ESP_ERR_INVALID_CRC;
    fclose(file);
    mbedtls_sha256_free(&sha);
    return error;
}

esp_err_t pinyin_asset_ensure(bool allow_install)
{
    esp_err_t error = verify();
    /* Never overwrite an existing, possibly user-owned or damaged dictionary. */
    if (error != ESP_ERR_NOT_FOUND) return error;
    if (!allow_install) return ESP_ERR_NOT_FOUND;
    if (mkdir("/sdcard/ime", 0775) && errno != EEXIST) return ESP_FAIL;
    const char *temporary = "/sdcard/ime/P4PYV1.TMP";
    if (access(temporary, F_OK) == 0) return ESP_ERR_INVALID_STATE;
    FILE *file = fopen(temporary, "wbx");
    if (!file) return ESP_FAIL;
    uint8_t *buffer = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) { fclose(file); remove(temporary); return ESP_ERR_NO_MEM; }
    const esp_log_level_t saved = esp_log_level_get("HTTP_CLIENT");
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    error = mbedtls_sha256_starts(&sha, 0) ? ESP_FAIL : ESP_OK;
    size_t written = 0;
    while (error == ESP_OK && written < PINYIN_ASSET_BYTES) {
        size_t amount = PINYIN_ASSET_BYTES - written;
        if (amount > 16384) amount = 16384;
        char url[256];
        int length = snprintf(url, sizeof(url), "%s?font=P4PYV1.DAT&offset=%u&length=%u",
                              SD_FONT_TRANSFER_URL, (unsigned)written, (unsigned)amount);
        if (length <= 0 || length >= sizeof(url)) { error = ESP_ERR_INVALID_SIZE; break; }
        bool complete = false;
        for (unsigned attempt = 0; attempt < 3 && !complete; ++attempt) {
            esp_http_client_config_t config = {.url=url, .timeout_ms=5000, .buffer_size=2048};
            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (!client) { error = ESP_ERR_NO_MEM; break; }
            esp_err_t opened = esp_http_client_open(client, 0);
            int size = opened == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
            size_t received = 0;
            if (opened == ESP_OK && size == (int)amount && esp_http_client_get_status_code(client) == 200) {
                while (received < amount) {
                    int got = esp_http_client_read(client, (char *)buffer + received, amount - received);
                    if (got <= 0) break;
                    received += got;
                }
            }
            complete = received == amount;
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (!complete) vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (!complete) { error = ESP_FAIL; break; }
        if (fwrite(buffer, 1, amount, file) != amount || mbedtls_sha256_update(&sha, buffer, amount)) {
            error = ESP_FAIL; break;
        }
        written += amount;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uint8_t digest[32];
    if (error == ESP_OK && (mbedtls_sha256_finish(&sha, digest) || !digest_matches(digest)))
        error = ESP_ERR_INVALID_CRC;
    mbedtls_sha256_free(&sha);
    heap_caps_free(buffer);
    if (fflush(file) || fsync(fileno(file))) error = ESP_FAIL;
    if (fclose(file)) error = ESP_FAIL;
    if (error == ESP_OK && access(PINYIN_ASSET_PATH, F_OK) == 0) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && rename(temporary, PINYIN_ASSET_PATH)) error = ESP_FAIL;
    if (error != ESP_OK) remove(temporary);
    esp_log_level_set("HTTP_CLIENT", saved);
    ESP_LOGI("pinyin", "asset_error=%d bytes=%u", (int)error, (unsigned)written);
    return error;
}
