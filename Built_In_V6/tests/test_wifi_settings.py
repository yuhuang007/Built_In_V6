"""Run production credential validation/switch code with synthetic Wi-Fi events.

No local secrets or real network are read. This checks failure persistence and
rollback, not ESP-Hosted timing, UI or radio interoperability.
"""
import ctypes
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
TCC = ROOT / 'logs/chat-host-tools/tcc/tcc.exe'


class WifiSettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TCC.exists():
            raise unittest.SkipTest('Optional host TCC unavailable')
        cls.temp = tempfile.TemporaryDirectory(dir=ROOT / 'logs')
        cls.addClassCleanup(cls.temp.cleanup)
        work = pathlib.Path(cls.temp.name)
        source = (ROOT / 'firmware/p4_main/main/wifi_settings.c').read_text(encoding='utf-8')
        definitions = source[source.index('typedef struct { uint8_t version;'):source.index('static QueueHandle_t')]
        validation = source[source.index('static void clear_private'):source.index('void wifi_settings_load')]
        loading = source[source.index('void wifi_settings_load'):source.index('void wifi_settings_event')]
        switching = source[source.index('static esp_err_t save_successful_network'):source.index('static void refresh_connected_network')]
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define SAVED_LIMIT 8
typedef int esp_err_t;
typedef int EventBits_t;
typedef struct { struct { unsigned char ssid[32], password[64];
    struct { int authmode; } threshold; } sta; } wifi_config_t;
#define BIT0 1
#define BIT1 2
#define LINK_UP BIT0
#define LINK_DOWN BIT1
#define WIFI_AUTH_WPA2_PSK 2
#define WIFI_AUTH_OPEN 0
#define WIFI_IF_STA 0
#define ESP_OK 0
#define ESP_FAIL -1
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY -1
#define atomic_store(p,v) (*(p)=(v))
#define atomic_load(p) (*(p))
static bool online, storage_ready, retry_paused;
static int state, storage, link_events, scenario, saves, commits, connects;
static int snapshot_lock, revision;
static char connected_ssid[33];
static unsigned char restore_blob[2048], legacy_blob[128];
static size_t restore_length, legacy_length;
static void xSemaphoreTake(int lock, int timeout) { }
static void xSemaphoreGive(int lock) { }
static int64_t ticks;
static wifi_config_t active;
static int64_t esp_timer_get_time(void) { return ticks * 1000; }
static void vTaskDelay(int delay) { ticks += delay; }
static void xEventGroupClearBits(int group, int bits) { }
static int esp_wifi_get_config(int interface, wifi_config_t *value) {
    *value = active; return scenario == 3 ? ESP_FAIL : ESP_OK;
}
static int esp_wifi_disconnect(void) { online = false; return ESP_OK; }
static int esp_wifi_set_config(int interface, wifi_config_t *value) {
    if ((scenario == 4 && value->sta.ssid[0] == 'n') || scenario == 6) return ESP_FAIL;
    active = *value; return ESP_OK;
}
static int esp_wifi_connect(void) { ++connects; return ESP_OK; }
static int xEventGroupWaitBits(int group, int bits, int clear, int all, int timeout) {
    ticks += timeout;
    if (bits == LINK_DOWN) return LINK_DOWN;
    if (scenario != 1 && scenario != 7 && connects && active.sta.ssid[0] == 'n') {
        online = true; return LINK_UP;
    }
    return 0;
}
static int nvs_set_blob(int handle, const char *key, const void *value, size_t length) {
    ++saves; return ESP_OK;
}
static int nvs_commit(int handle) { ++commits; return scenario == 2 ? ESP_FAIL : ESP_OK; }
static int nvs_open(const char *name, int mode, int *handle) { *handle = 1; return ESP_OK; }
#define NVS_READWRITE 1
static int nvs_get_blob(int handle, const char *key, void *value, size_t *length) {
    unsigned char *source = strcmp(key, "networks_v1") == 0 ? restore_blob : legacy_blob;
    size_t size = strcmp(key, "networks_v1") == 0 ? restore_length : legacy_length;
    if (!size || size > *length) return ESP_FAIL;
    memcpy(value, source, size); *length = size; return ESP_OK;
}
static int nvs_erase_key(int handle, const char *key) {
    if (!legacy_length) return ESP_FAIL;
    legacy_length = 0; return ESP_OK;
}
'''
        exports = r'''
__declspec(dllexport) int check_credentials(const char *ssid, const char *password) {
    credentials_t value = {0};
    value.version = 1;
    strncpy(value.ssid, ssid, sizeof(value.ssid));
    strncpy(value.password, password, sizeof(value.password));
    return valid(&value);
}
__declspec(dllexport) int run_switch(int test) {
    scenario = test; saves = commits = connects = 0; ticks = 0;
    online = test != 7; retry_paused = test == 7; storage_ready = test != 5;
    memset(&saved_book, 0, sizeof(saved_book));
    saved_book.version = 1;
    credentials_t old = {1, "old-test", "old-synthetic-password"};
    book_put(&saved_book, &old);
    memset(&active, 0, sizeof(active));
    memcpy(active.sta.ssid, "old-test", 8);
    credentials_t value = {1, "new-test", "synthetic-password"};
    switch_network(&value);
    return state | (saves << 8) | (commits << 12) |
        ((active.sta.ssid[0] == 'o') << 16) | ((ticks <= 33000) << 17);
}
__declspec(dllexport) int book_result(void) {
    return saved_book.count | (saved_book.entries[0].ssid[0] << 8);
}
__declspec(dllexport) int connection_attempts(void) { return connects; }
__declspec(dllexport) int book_checks(void) {
    saved_book_t book = {1, 0};
    credentials_t value = {1, "", "synthetic-password"};
    int result = book_valid(&book) ? 1 : 0;
    for (int i = 0; i < 9; ++i) {
        sprintf(value.ssid, "node-%d", i);
        book_put(&book, &value);
    }
    if (book.count == 8 && book_find(&book, "node-0") < 0 &&
        strcmp(book.entries[0].ssid, "node-8") == 0) result |= 2;
    strcpy(value.ssid, "node-4"); strcpy(value.password, "replacement-password");
    book_put(&book, &value);
    if (book.count == 8 && book_find(&book, "node-4") == 0 &&
        strcmp(book.entries[0].password, "replacement-password") == 0) result |= 4;
    book_remove(&book, 0);
    credentials_t zero = {0};
    if (book.count == 7 && book_find(&book, "node-4") < 0 &&
        strcmp(book.entries[0].ssid, "node-8") == 0 &&
        memcmp(&book.entries[7], &zero, sizeof(zero)) == 0) result |= 8;
    book.entries[1] = book.entries[0];
    if (!book_valid(&book)) result |= 16;
    book.count = 9;
    if (!book_valid(&book)) result |= 32;
    return result;
}
__declspec(dllexport) int migration_checks(void) {
    int result = 0;
    scenario = 0; restore_length = legacy_length = 0;
    memset(&saved_book, 0, sizeof(saved_book)); saved_book.version = 1;
    credentials_t base = {1, "base-test", "base-synthetic-password"};
    wifi_config_t fallback = make_config(&base);
    wifi_settings_load(&fallback);
    if (saved_book.count == 1 && book_find(&saved_book, base.ssid) == 0) result |= 1;
    credentials_t legacy = {1, "legacy-test", "legacy-synthetic-password"};
    memcpy(legacy_blob, &legacy, sizeof(legacy)); legacy_length = sizeof(legacy);
    memset(&saved_book, 0, sizeof(saved_book)); saved_book.version = 1;
    fallback = make_config(&base);
    wifi_settings_load(&fallback);
    if (saved_book.count == 2 && book_find(&saved_book, legacy.ssid) == 0 &&
        strcmp((char *)fallback.sta.ssid, legacy.ssid) == 0 && !legacy_length) result |= 2;
    memcpy(restore_blob, &saved_book, sizeof(saved_book)); restore_length = sizeof(saved_book);
    memset(&saved_book, 0, sizeof(saved_book)); saved_book.version = 1;
    fallback = make_config(&base);
    wifi_settings_load(&fallback);
    if (saved_book.count == 2 && book_find(&saved_book, legacy.ssid) == 0 &&
        strcmp((char *)fallback.sta.ssid, legacy.ssid) == 0) result |= 4;
    saved_book_t empty = {1, 0};
    memcpy(restore_blob, &empty, sizeof(empty));
    fallback = make_config(&base);
    wifi_settings_load(&fallback);
    if (saved_book.count == 0 && strcmp((char *)fallback.sta.ssid, base.ssid) == 0 && retry_paused) result |= 8;
    empty.count = 9;
    memcpy(restore_blob, &empty, sizeof(empty));
    memset(&saved_book, 0, sizeof(saved_book)); saved_book.version = 1;
    fallback = make_config(&base);
    wifi_settings_load(&fallback);
    if (saved_book.count == 1 && book_valid(&saved_book)) result |= 16;
    return result;
}
__declspec(dllexport) int forget_checks(int fail) {
    scenario = fail ? 2 : 0; online = true; storage_ready = true;
    memset(&saved_book, 0, sizeof(saved_book)); saved_book.version = 1;
    credentials_t value = {1, "forget-test", "synthetic-password"};
    book_put(&saved_book, &value);
    forget_network(value.ssid);
    return state | (saved_book.count << 8) | (online << 16);
}
'''
        path = work / 'wifi.c'
        path.write_text(harness + definitions + 'static saved_book_t saved_book;\n' + validation + loading + switching + exports, encoding='utf-8')
        result = subprocess.run([str(TCC), '-shared', str(path), '-o', str(work / 'wifi.dll')],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError('Synthetic Wi-Fi test compile failed: ' + result.stderr)
        cls.lib = ctypes.CDLL(str(work / 'wifi.dll'))
        free = ctypes.windll.kernel32.FreeLibrary
        free.argtypes = [ctypes.c_void_p]
        cls.addClassCleanup(free, cls.lib._handle)
        cls.lib.check_credentials.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        cls.lib.run_switch.argtypes = [ctypes.c_int]

    def test_byte_limits_and_open_network(self):
        valid = self.lib.check_credentials
        self.assertTrue(valid(b'x' * 32, b'p' * 63))
        self.assertTrue(valid(b'open-test', b''))
        self.assertFalse(valid(b'', b'p' * 8))
        self.assertFalse(valid(b'x' * 33, b'p' * 8))
        self.assertFalse(valid(b'x', b'p' * 7))
        self.assertFalse(valid(b'x', b'p' * 64))
        self.assertFalse(valid(('中' * 11).encode(), b'p' * 8))

    def test_dhcp_success_persists_once(self):
        self.assertEqual(self.lib.run_switch(0), 3 | (1 << 8) | (1 << 12) | (1 << 17))

    def test_timeout_restores_without_saving_candidate(self):
        self.assertEqual(self.lib.run_switch(1), 5 | (1 << 16) | (1 << 17))

    def test_save_failure_keeps_live_connection_and_reports_failure(self):
        self.assertEqual(self.lib.run_switch(2), 6 | (1 << 8) | (1 << 12) | (1 << 17))
        self.assertEqual(self.lib.run_switch(5), 6 | (1 << 17))

    def test_driver_failures_do_not_persist_candidate(self):
        self.assertEqual(self.lib.run_switch(3), 7 | (1 << 16) | (1 << 17))
        self.assertEqual(self.lib.run_switch(4), 5 | (1 << 16) | (1 << 17))
        self.assertEqual(self.lib.run_switch(6), 8 | (1 << 16) | (1 << 17))

    def test_saved_network_limit_update_dedup_forget_and_validation(self):
        self.assertEqual(self.lib.book_checks(), 63)

    def test_saved_registry_updates_only_after_connection_and_commit(self):
        self.lib.run_switch(0)
        self.assertEqual(self.lib.book_result(), 2 | (ord('n') << 8))
        for scenario in (1, 2, 3, 4, 5, 6):
            self.lib.run_switch(scenario)
            self.assertEqual(self.lib.book_result(), 1 | (ord('o') << 8))

    def test_migrate_previous_record_and_restore_book_without_reseeding_forgotten_entries(self):
        self.assertEqual(self.lib.migration_checks(), 31)

    def test_forget_persists_before_removing_and_keeps_current_link(self):
        self.assertEqual(self.lib.forget_checks(0), 9 | (1 << 16))
        self.assertEqual(self.lib.forget_checks(1), 10 | (1 << 8) | (1 << 16))

    def test_empty_book_failed_manual_attempt_does_not_reconnect_forgotten_fallback(self):
        self.assertEqual(self.lib.run_switch(7), 7 | (1 << 16) | (1 << 17))
        self.assertEqual(self.lib.connection_attempts(), 1)


if __name__ == '__main__':
    unittest.main()
