#include "sd_font_upgrade.h"
#include "sd_font_assets.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "../../../secrets/sd_font_transfer.h"

#define SEGMENT_BYTES (16 * 1024)
static const char *TAG = "sd_font_v2";
typedef struct { const char *name; size_t bytes; const char *hash; } asset_t;
static const asset_t assets[] = {
    {SD_FONT_14_NAME, SD_FONT_14_BYTES, SD_FONT_14_SHA256},
    {SD_FONT_28_NAME, SD_FONT_28_BYTES, SD_FONT_28_SHA256},
};

/* Persistent verification receipt, not a substitute for detecting bit rot.
 * Rehash when asset version, size or FAT modification time changes. */
typedef struct {
    char hash[65];
    int64_t bytes;
    int64_t modified;
} verified_receipt_t;

static const char *receipt_key(const asset_t *asset)
{
    return asset == &assets[0] ? "font14" : "font28";
}

static bool file_receipt(const char *path, const asset_t *asset, verified_receipt_t *receipt)
{
    struct stat info;
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size != asset->bytes) return false;
    memset(receipt, 0, sizeof(*receipt));
    memcpy(receipt->hash, asset->hash, sizeof(receipt->hash));
    receipt->bytes = info.st_size;
    receipt->modified = info.st_mtime;
    return true;
}

static bool previously_verified(const char *path, const asset_t *asset)
{
    verified_receipt_t current, saved;
    if (!file_receipt(path, asset, &current)) return false;
    nvs_handle_t handle;
    if (nvs_open("sd_font_v2", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = sizeof(saved);
    const esp_err_t error = nvs_get_blob(handle, receipt_key(asset), &saved, &length);
    nvs_close(handle);
    return error == ESP_OK && length == sizeof(saved) &&
           memcmp(&current, &saved, sizeof(current)) == 0;
}

static void remember_verified(const char *path, const asset_t *asset)
{
    verified_receipt_t receipt;
    if (!file_receipt(path, asset, &receipt)) return;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("sd_font_v2", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, receipt_key(asset), &receipt, sizeof(receipt));
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK) ESP_LOGW(TAG, "receipt_save_error=%d", (int)error);
}

static bool digest_matches(const uint8_t digest[32], const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i)
        if (expected[i * 2] != hex[digest[i] >> 4] ||
            expected[i * 2 + 1] != hex[digest[i] & 15]) return false;
    return expected[64] == 0;
}

static esp_err_t verify_existing(const char *path, const asset_t *asset,
                                 sd_font_cancel_fn cancelled, void *context)
{
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    uint8_t buffer[2048], digest[32];
    size_t total = 0;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    esp_err_t error = mbedtls_sha256_starts(&sha, 0) == 0 ? ESP_OK : ESP_FAIL;
    while (error == ESP_OK) {
        if (cancelled && cancelled(context)) { error = ESP_ERR_INVALID_STATE; break; }
        const size_t got = fread(buffer, 1, sizeof(buffer), file);
        if (!got) break;
        total += got;
        if (total > asset->bytes || mbedtls_sha256_update(&sha, buffer, got)) {
            error = ESP_ERR_INVALID_SIZE; break;
        }
        /* Yield to RTC/LVGL while verifying multi-megabyte fonts. */
        if (total % SEGMENT_BYTES == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (ferror(file) || total != asset->bytes) error = ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK && (mbedtls_sha256_finish(&sha, digest) ||
                           !digest_matches(digest, asset->hash))) error = ESP_ERR_INVALID_CRC;
    fclose(file);
    mbedtls_sha256_free(&sha);
    return error;
}

static esp_err_t provision(const asset_t *asset, size_t completed, size_t total,
                           sd_font_progress_fn progress, sd_font_cancel_fn cancelled, void *context,
                           bool allow_install)
{
    char target[80], temporary[80];
    snprintf(target, sizeof(target), "/sdcard/fonts/%s", asset->name);
    snprintf(temporary, sizeof(temporary), "/sdcard/fonts/%s", 
             asset == &assets[0] ? "P4Z14V2.TMP" : "P4Z28V2.TMP");
    if (cancelled && cancelled(context)) return ESP_ERR_INVALID_STATE;
    if (previously_verified(target, asset)) {
        ESP_LOGI(TAG, "verified_fast_path bytes=%u", (unsigned)asset->bytes);
        return ESP_OK;
    }
    if (progress) progress(completed * 100 / total, context);
    esp_err_t error = verify_existing(target, asset, cancelled, context);
    if (error != ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "existing_file bytes=%u verify_error=%d", (unsigned)asset->bytes, (int)error);
        if (error == ESP_OK) remember_verified(target, asset);
        return error; /* Invalid existing font remains untouched. */
    }
    if (!allow_install) return ESP_ERR_NOT_FOUND;
    /* Do not overwrite any preexisting temporary file either. */
    if (access(temporary, F_OK) == 0) {
        ESP_LOGW(TAG, "temporary_exists=1");
        return ESP_ERR_INVALID_STATE;
    }
    FILE *file = fopen(temporary, "wbx");
    if (!file) {
        ESP_LOGW(TAG, "file_create_failed errno=%d", errno);
        return ESP_FAIL;
    }
    uint8_t digest[32];
    uint8_t *segment = heap_caps_malloc(SEGMENT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!segment) { fclose(file); (void)remove(temporary); return ESP_ERR_NO_MEM; }
    size_t written = 0;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    error = mbedtls_sha256_starts(&sha, 0) == 0 ? ESP_OK : ESP_FAIL;
    while (error == ESP_OK && written < asset->bytes) {
        if (cancelled && cancelled(context)) { error = ESP_ERR_INVALID_STATE; break; }
        const size_t amount = asset->bytes - written > SEGMENT_BYTES ? SEGMENT_BYTES : asset->bytes - written;
        char url[256];
        const int length = snprintf(url, sizeof(url), "%s?font=%s&offset=%u&length=%u",
            SD_FONT_TRANSFER_URL, asset->name, (unsigned)written, (unsigned)amount);
        if (length < 0 || length >= sizeof(url)) { error = ESP_ERR_INVALID_SIZE; break; }
        bool segment_complete = false;
        for (unsigned attempt = 0; attempt < 3 && !segment_complete; ++attempt) {
            if (cancelled && cancelled(context)) { error = ESP_ERR_INVALID_STATE; break; }
            const esp_http_client_config_t config = {.url=url, .timeout_ms=15000, .buffer_size=2048};
            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (!client) { error = ESP_ERR_NO_MEM; break; }
            const esp_err_t opened = esp_http_client_open(client, 0);
            const int content_length = opened == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
            const int status = opened == ESP_OK ? esp_http_client_get_status_code(client) : 0;
            size_t received = 0;
            if (opened == ESP_OK && content_length == (int)amount && status == 200) {
                while (received < amount) {
                    if (cancelled && cancelled(context)) { error = ESP_ERR_INVALID_STATE; break; }
                    const size_t want = amount - received > 2048 ? 2048 : amount - received;
                    const int got = esp_http_client_read(client, (char *)segment + received, want);
                    if (got <= 0) break;
                    received += got;
                }
                segment_complete = received == amount;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (error != ESP_OK) break;
            if (!segment_complete) {
                ESP_LOGW(TAG, "segment_retry attempt=%u open_error=%d http_status=%d length=%d offset=%u received=%u",
                         attempt + 1, (int)opened, status, content_length,
                         (unsigned)written, (unsigned)received);
                vTaskDelay(pdMS_TO_TICKS(200 * (attempt + 1)));
            }
        }
        /* Transactional segment: retries never alter disk offset or SHA state. */
        if (error == ESP_OK && !segment_complete) error = ESP_FAIL;
        if (error == ESP_OK && (fwrite(segment, 1, amount, file) != amount ||
                               mbedtls_sha256_update(&sha, segment, amount))) error = ESP_FAIL;
        if (error == ESP_OK) {
            written += amount;
            if (progress) progress((completed + written) * 100 / total, context);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (error == ESP_OK && (mbedtls_sha256_finish(&sha, digest) ||
                           !digest_matches(digest, asset->hash))) error = ESP_ERR_INVALID_CRC;
    mbedtls_sha256_free(&sha);
    heap_caps_free(segment);
    if (fflush(file) || fsync(fileno(file))) error = ESP_FAIL;
    if (fclose(file)) error = ESP_FAIL;
    /* Final target must still be absent; never replace an existing user file. */
    if (error == ESP_OK && access(target, F_OK) == 0) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && rename(temporary, target)) error = ESP_FAIL;
    if (error == ESP_OK) remember_verified(target, asset);
    if (error != ESP_OK) (void)remove(temporary); /* Only the file created above. */
    ESP_LOGI(TAG, "provision_done bytes=%u error=%d", (unsigned)written, (int)error);
    return error;
}

esp_err_t sd_font_upgrade_ensure(sd_font_progress_fn progress,
                               sd_font_cancel_fn cancelled, void *context)
{
    if (mkdir("/sdcard/fonts", 0775) && errno != EEXIST) return ESP_FAIL;
    /* ESP HTTP error paths can echo the configured private LAN URL. */
    const esp_log_level_t saved = esp_log_level_get("HTTP_CLIENT");
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
    size_t completed = 0;
    const size_t total = SD_FONT_14_BYTES + SD_FONT_28_BYTES;
    esp_err_t error = ESP_OK;
    for (unsigned i = 0; i < 2 && error == ESP_OK; ++i) {
        error = provision(&assets[i], completed, total, progress, cancelled, context, true);
        completed += assets[i].bytes;
    }
    esp_log_level_set("HTTP_CLIENT", saved);
    return error;
}

esp_err_t sd_font_upgrade_verify_installed(void)
{
    const size_t total = SD_FONT_14_BYTES + SD_FONT_28_BYTES;
    size_t completed = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const esp_err_t error = provision(&assets[i], completed, total, NULL, NULL, NULL, false);
        if (error != ESP_OK) return error;
        completed += assets[i].bytes;
    }
    return ESP_OK;
}
