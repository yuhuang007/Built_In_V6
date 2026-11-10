#include "wifi_settings.h"
#include "ui_layout.h"
#include "qinghe_ui.h"
#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "device_about.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AP_LIMIT 16
#define SAVED_LIMIT 8
#define LINK_UP BIT0
#define LINK_DOWN BIT1
typedef struct { uint8_t version; char ssid[33]; char password[64]; } credentials_t;
typedef struct { uint8_t version, count; credentials_t entries[SAVED_LIMIT]; } saved_book_t;
typedef struct { int type; credentials_t credentials; } command_t;
enum { CMD_SCAN = 1, CMD_CONNECT, CMD_FORGET };
enum { IDLE, SCANNING, CONNECTING, CONNECTED, SCAN_FAILED, RESTORED, SAVE_FAILED, CONNECT_FAILED, RESTORE_FAILED, FORGOTTEN, FORGET_FAILED };
static QueueHandle_t commands;
static EventGroupHandle_t link_events;
static SemaphoreHandle_t snapshot_lock;
static atomic_bool online;
static atomic_bool occupied;
static atomic_bool station_started, retry_paused;
static atomic_int state;
static atomic_uint link_generation;
static unsigned connected_generation;
static char connected_ssid[33];
static wifi_ap_record_t networks[AP_LIMIT];
static unsigned network_count, revision;
static nvs_handle_t storage;
static bool storage_ready;
static saved_book_t saved_book = { .version = 1 };
typedef struct { char ssid[33]; bool open, saved, nearby; int rssi; } network_row_t;
static network_row_t displayed_networks[AP_LIMIT + SAVED_LIMIT];
static unsigned displayed_count;
static lv_obj_t *network_menu;
static char menu_ssid[33];
static lv_obj_t *screen, *return_screen, *status, *list, *editor, *ssid_input, *password_input, *open_choice;
static lv_obj_t *editor_error;
static lv_obj_t *current_network, *footer;
static lv_obj_t *scan_button, *manual_button;
static const lv_font_t *ui_font;
static unsigned displayed_revision;

/* Erase transient password copies even with compiler optimization enabled. */
static void clear_private(void *data, size_t length)
{
    volatile unsigned char *bytes = data;
    while (length--) *bytes++ = 0;
}

static bool valid(const credentials_t *value)
{
    const size_t ssid = strnlen(value->ssid, sizeof(value->ssid));
    const size_t password = strnlen(value->password, sizeof(value->password));
    return value->version == 1 && ssid > 0 && ssid <= 32 &&
           (password == 0 || (password >= 8 && password <= 63));
}

static int book_find(const saved_book_t *book, const char *ssid)
{
    for (unsigned i = 0; i < book->count; ++i)
        if (strcmp(book->entries[i].ssid, ssid) == 0) return (int)i;
    return -1;
}

static bool book_valid(const saved_book_t *book)
{
    if (book->version != 1 || book->count > SAVED_LIMIT) return false;
    for (unsigned i = 0; i < book->count; ++i) {
        if (!valid(&book->entries[i])) return false;
        for (unsigned j = 0; j < i; ++j)
            if (strcmp(book->entries[i].ssid, book->entries[j].ssid) == 0) return false;
    }
    return true;
}

/* Most recently successful network first; bounded storage and no duplicates. */
static void book_put(saved_book_t *book, const credentials_t *credentials)
{
    const int previous = book_find(book, credentials->ssid);
    unsigned position = previous >= 0 ? (unsigned)previous : book->count;
    if (position >= SAVED_LIMIT) position = SAVED_LIMIT - 1;
    for (unsigned i = position; i > 0; --i) book->entries[i] = book->entries[i - 1];
    book->entries[0] = *credentials;
    if (previous < 0 && book->count < SAVED_LIMIT) ++book->count;
}

static void book_remove(saved_book_t *book, unsigned index)
{
    if (index >= book->count) return;
    for (unsigned i = index + 1; i < book->count; ++i) book->entries[i - 1] = book->entries[i];
    --book->count;
    clear_private(&book->entries[book->count], sizeof(credentials_t));
}

static wifi_config_t make_config(const credentials_t *value)
{
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, value->ssid, strlen(value->ssid));
    memcpy(config.sta.password, value->password, strlen(value->password));
    config.sta.threshold.authmode = value->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    return config;
}

void wifi_settings_load(wifi_config_t *fallback)
{
    atomic_store(&retry_paused, false);
    storage_ready = nvs_open("p4_wifi", NVS_READWRITE, &storage) == ESP_OK;
    if (!storage_ready) return;
    size_t book_length = sizeof(saved_book);
    saved_book_t restored = {0};
    if (nvs_get_blob(storage, "networks_v1", &restored, &book_length) == ESP_OK &&
        book_length == sizeof(restored) && book_valid(&restored)) {
        saved_book = restored;
        if (saved_book.count) *fallback = make_config(&saved_book.entries[0]);
        else atomic_store(&retry_paused, true);
        clear_private(&restored, sizeof(restored));
        return;
    }
    clear_private(&restored, sizeof(restored));
    /* Migrate the previous one-network record and retain the configured
     * fallback locally. Never reseed a valid book after an explicit forget. */
    credentials_t saved = { .version = 1 };
    memcpy(saved.ssid, fallback->sta.ssid, 32);
    memcpy(saved.password, fallback->sta.password, 63);
    if (strnlen((char *)fallback->sta.password, sizeof(fallback->sta.password)) <= 63 && valid(&saved))
        book_put(&saved_book, &saved);
    clear_private(&saved, sizeof(saved));
    size_t length = sizeof(saved);
    if (nvs_get_blob(storage, "station_v1", &saved, &length) == ESP_OK &&
        length == sizeof(saved) && valid(&saved)) {
        book_put(&saved_book, &saved);
        *fallback = make_config(&saved);
    }
    if (nvs_set_blob(storage, "networks_v1", &saved_book, sizeof(saved_book)) == ESP_OK &&
        nvs_commit(storage) == ESP_OK) {
        if (nvs_erase_key(storage, "station_v1") == ESP_OK) nvs_commit(storage);
    }
    clear_private(&saved, sizeof(saved));
}

void wifi_settings_event(int32_t event_id, bool got_ip)
{
    if (!link_events) return;
    if (!got_ip && event_id == WIFI_EVENT_STA_START) {
        atomic_store(&station_started, true);
        return;
    }
    if (got_ip) {
        atomic_fetch_add(&link_generation, 1);
        atomic_store(&online, true);
        xEventGroupSetBits(link_events, LINK_UP);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&online, false);
        atomic_fetch_add(&link_generation, 1);
        xEventGroupClearBits(link_events, LINK_UP);
        xEventGroupSetBits(link_events, LINK_DOWN);
    }
}

static void scan_networks(void)
{
    atomic_store(&state, SCANNING);
    /* Association retries can reject a scan. Pause them only while offline;
     * the worker resumes the active configuration after collecting results. */
    if (!atomic_load(&online)) {
        xEventGroupClearBits(link_events, LINK_DOWN);
        if (esp_wifi_disconnect() == ESP_OK)
            xEventGroupWaitBits(link_events, LINK_DOWN, pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
    }
    wifi_scan_config_t config = { .show_hidden = false };
    esp_err_t error = esp_wifi_scan_start(&config, true);
    wifi_ap_record_t found[AP_LIMIT] = {0};
    uint16_t count = AP_LIMIT;
    if (error == ESP_OK) error = esp_wifi_scan_get_ap_records(&count, found);
    if (error != ESP_OK) {
        esp_wifi_clear_ap_list();
        atomic_store(&state, SCAN_FAILED);
        return;
    }
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    network_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        found[i].ssid[32] = 0;
        if (!found[i].ssid[0]) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < network_count; ++j)
            if (strcmp((char *)networks[j].ssid, (char *)found[i].ssid) == 0) duplicate = true;
        if (!duplicate) networks[network_count++] = found[i];
    }
    ++revision;
    xSemaphoreGive(snapshot_lock);
    clear_private(found, sizeof(found));
    atomic_store(&state, atomic_load(&online) ? CONNECTED : IDLE);
}

static esp_err_t save_successful_network(const credentials_t *credentials)
{
    if (!storage_ready) return ESP_FAIL;
    saved_book_t next;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    next = saved_book;
    xSemaphoreGive(snapshot_lock);
    book_put(&next, credentials);
    esp_err_t error = nvs_set_blob(storage, "networks_v1", &next, sizeof(next));
    if (error == ESP_OK) error = nvs_commit(storage);
    if (error == ESP_OK) {
        xSemaphoreTake(snapshot_lock, portMAX_DELAY);
        saved_book = next;
        ++revision;
        xSemaphoreGive(snapshot_lock);
    }
    clear_private(&next, sizeof(next));
    return error;
}

static void forget_network(const char *ssid)
{
    saved_book_t next;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    next = saved_book;
    xSemaphoreGive(snapshot_lock);
    const int index = book_find(&next, ssid);
    if (index < 0) { clear_private(&next, sizeof(next)); return; }
    book_remove(&next, (unsigned)index);
    esp_err_t error = storage_ready ? nvs_set_blob(storage, "networks_v1", &next, sizeof(next)) : ESP_FAIL;
    if (error == ESP_OK) error = nvs_commit(storage);
    if (error == ESP_OK) {
        xSemaphoreTake(snapshot_lock, portMAX_DELAY);
        saved_book = next;
        ++revision;
        xSemaphoreGive(snapshot_lock);
        atomic_store(&state, FORGOTTEN);
        if (!next.count || strcmp(connected_ssid, ssid) == 0) atomic_store(&retry_paused, true);
    } else atomic_store(&state, FORGET_FAILED);
    clear_private(&next, sizeof(next));
}

static void switch_network(const credentials_t *credentials)
{
    wifi_config_t previous = {0};
    wifi_config_t candidate = make_config(credentials);
    const bool previous_paused = atomic_load(&retry_paused);
    const bool previous_online = atomic_load(&online);
    atomic_store(&state, CONNECTING);
    esp_err_t error = esp_wifi_get_config(WIFI_IF_STA, &previous);
    if (error != ESP_OK) { atomic_store(&state, CONNECT_FAILED); goto done; }
    atomic_store(&retry_paused, false);
    xEventGroupClearBits(link_events, LINK_DOWN | LINK_UP);
    error = esp_wifi_disconnect();
    if (error == ESP_OK)
        xEventGroupWaitBits(link_events, LINK_DOWN, pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
    atomic_store(&online, false);
    xEventGroupClearBits(link_events, LINK_UP);
    error = esp_wifi_set_config(WIFI_IF_STA, &candidate);
    if (error == ESP_OK) error = esp_wifi_connect();
    bool success = false;
    /* Retry authentication/association at a bounded cadence, never from UI or
     * the event handler. Successful DHCP is required before persisting. */
    const int64_t deadline = esp_timer_get_time() + 30000000;
    while (error == ESP_OK && esp_timer_get_time() < deadline) {
        EventBits_t bits = xEventGroupWaitBits(link_events, LINK_UP | LINK_DOWN,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if ((bits & LINK_UP) && atomic_load(&online)) { success = true; break; }
        if (bits & LINK_DOWN) {
            xEventGroupClearBits(link_events, LINK_DOWN);
            vTaskDelay(pdMS_TO_TICKS(1000));
            error = esp_wifi_connect();
        }
    }
    if (success) {
        error = save_successful_network(credentials);
        atomic_store(&state, error == ESP_OK ? CONNECTED : SAVE_FAILED);
    } else {
        xEventGroupClearBits(link_events, LINK_DOWN | LINK_UP);
        if (esp_wifi_disconnect() == ESP_OK)
            xEventGroupWaitBits(link_events, LINK_DOWN, pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
        atomic_store(&online, false);
        xEventGroupClearBits(link_events, LINK_UP);
        error = esp_wifi_set_config(WIFI_IF_STA, &previous);
        if (error == ESP_OK) {
            atomic_store(&retry_paused, previous_paused);
            if (!previous_paused || previous_online) {
                esp_wifi_connect();
                atomic_store(&state, RESTORED);
            } else atomic_store(&state, CONNECT_FAILED);
        } else {
            /* Do not silently keep retrying the failed candidate if the
             * remote driver could not restore the previous configuration. */
            atomic_store(&retry_paused, true);
            atomic_store(&state, RESTORE_FAILED);
        }
    }
done:
    clear_private(&previous, sizeof(previous));
    clear_private(&candidate, sizeof(candidate));
}

static void refresh_connected_network(void)
{
    const unsigned generation = atomic_load(&link_generation);
    if (!atomic_load(&online) || connected_generation == generation) return;
    /* Read the actual associated AP, not the requested/saved credentials.
     * This RPC runs on the network worker and never holds the UI lock. */
    wifi_ap_record_t access_point = {0};
    const esp_err_t error = esp_wifi_sta_get_ap_info(&access_point);
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    if (error == ESP_OK && atomic_load(&online) &&
        generation == atomic_load(&link_generation)) {
        memcpy(connected_ssid, access_point.ssid, 32);
        connected_ssid[32] = 0;
        connected_generation = generation;
    }
    xSemaphoreGive(snapshot_lock);
    clear_private(&access_point, sizeof(access_point));
}

static void worker(void *argument)
{
    (void)argument;
    command_t command = {0};
    unsigned retry_ticks = 2;
    unsigned diagnostic_ticks = 0, attempts = 0;
    esp_err_t retry_error = ESP_OK;
    for (;;) {
        if (xQueueReceive(commands, &command, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (command.type == CMD_SCAN) scan_networks();
            else if (command.type == CMD_CONNECT && valid(&command.credentials)) switch_network(&command.credentials);
            else if (command.type == CMD_FORGET) forget_network(command.credentials.ssid);
            clear_private(&command, sizeof(command));
            atomic_store(&occupied, false);
            retry_ticks = 0;
        } else if (atomic_load(&station_started) && !atomic_load(&retry_paused) &&
                   !atomic_load(&online) && ++retry_ticks >= 3) {
            retry_error = esp_wifi_connect();
            ++attempts;
            retry_ticks = 0;
        } else if (atomic_load(&online)) retry_ticks = 0;
        refresh_connected_network();
        if (++diagnostic_ticks >= 10) {
            diagnostic_ticks = 0;
            xSemaphoreTake(snapshot_lock, portMAX_DELAY);
            const unsigned saved_count = saved_book.count;
            xSemaphoreGive(snapshot_lock);
            ESP_LOGI("wifi_settings", "health started=%d paused=%d online=%d state=%d saved=%u attempts=%u error=%d",
                (int)atomic_load(&station_started), (int)atomic_load(&retry_paused),
                (int)atomic_load(&online), atomic_load(&state), saved_count, attempts, (int)retry_error);
        }
    }
}

esp_err_t wifi_settings_start(void)
{
    commands = xQueueCreate(1, sizeof(command_t));
    link_events = xEventGroupCreate();
    snapshot_lock = xSemaphoreCreateMutex();
    if (!commands || !link_events || !snapshot_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "wifi_settings", 6144, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_style_text_font(object, ui_font, 0);
    lv_obj_set_style_text_color(object, qh_color(QH_INK), 0);
    lv_label_set_text(object, text);
    return object;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int width, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_size(object, width, ui_font->line_height + 16 > 70 ? ui_font->line_height + 16 : 70);
    qh_button(object, QH_SOFT);
    lv_obj_set_style_radius(object, 12, 0);
    lv_obj_t *caption = label(object, text);
    lv_obj_center(caption);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, NULL);
    return object;
}

static void close_editor(void)
{
    if (!editor) return;
    lv_textarea_set_text(password_input, "");
    lv_textarea_set_text(ssid_input, "");
    lv_obj_delete(editor);
    editor = NULL;
}

static void cancel_editor(lv_event_t *event) { (void)event; close_editor(); }
static void close_menu(void)
{
    if (network_menu) lv_obj_delete(network_menu);
    network_menu = NULL;
    clear_private(menu_ssid, sizeof(menu_ssid));
}
static void back(lv_event_t *event) { (void)event; close_editor(); close_menu(); qh_screen_load(return_screen, true); }

static bool enqueue(command_t *command)
{
    bool expected = false;
    if (!commands || !atomic_compare_exchange_strong(&occupied, &expected, true)) return false;
    if (xQueueSend(commands, command, 0) != pdTRUE) {
        atomic_store(&occupied, false);
        return false;
    }
    return true;
}

static void connect_clicked(lv_event_t *event)
{
    (void)event;
    command_t command = { .type = CMD_CONNECT, .credentials.version = 1 };
    const char *ssid = lv_textarea_get_text(ssid_input);
    const char *password = lv_obj_has_state(open_choice, LV_STATE_CHECKED) ? "" : lv_textarea_get_text(password_input);
    if (strlen(ssid) > 32 || strlen(password) > 63) {
        lv_label_set_text(editor_error, "名称最多32字节，密码最多63字节");
        return;
    }
    strcpy(command.credentials.ssid, ssid);
    strcpy(command.credentials.password, password);
    if (!valid(&command.credentials)) lv_label_set_text(editor_error, "请填写名称，密码需要8至63个字符");
    else if (enqueue(&command)) { atomic_store(&state, CONNECTING); close_editor(); }
    clear_private(&command, sizeof(command));
}

static void input_focus(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_user_data(event);
    lv_keyboard_set_textarea(keyboard, lv_event_get_target_obj(event));
}

static void keyboard_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CANCEL) close_editor();
    else if (lv_event_get_code(event) == LV_EVENT_READY) connect_clicked(event);
}

static void edit_network(const char *ssid, bool open)
{
    if (atomic_load(&occupied)) return;
    close_editor();
    editor = lv_obj_create(screen);
    lv_obj_set_size(editor, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_style_pad_all(editor, 16, 0);
    lv_obj_center(editor);
    qh_reveal(editor);
    lv_obj_set_scrollable(editor, false);
    lv_obj_set_style_bg_color(editor, qh_color(QH_BG), 0);
    lv_obj_t *caption=qh_header(editor,"连接 Wi-Fi",ui_font,cancel_editor,false);
    lv_obj_t *connect=button(editor,"连接",140,connect_clicked);
    lv_obj_align(connect,LV_ALIGN_TOP_RIGHT,-10,76);
    caption = label(editor, "网络名称");
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 12, 150);
    ssid_input = lv_textarea_create(editor);
    lv_obj_set_size(ssid_input, UI_WIDTH - 64, 64);
    lv_obj_align(ssid_input, LV_ALIGN_TOP_LEFT, 12, 210);
    qh_panel(ssid_input, QH_SURFACE, 16);
    lv_obj_set_style_pad_ver(ssid_input, 0, 0);
    lv_obj_set_style_text_color(ssid_input, qh_color(QH_INK), 0);
    lv_textarea_set_one_line(ssid_input, true);
    lv_textarea_set_max_length(ssid_input, 32);
    lv_obj_set_style_text_font(ssid_input, ui_font, 0);
    lv_textarea_set_text(ssid_input, ssid);
    caption = label(editor, "密码");
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 12, 290);
    password_input = lv_textarea_create(editor);
    lv_obj_set_size(password_input, UI_WIDTH - 64, 64);
    lv_obj_align(password_input, LV_ALIGN_TOP_LEFT, 12, 350);
    qh_panel(password_input, QH_SURFACE, 16);
    lv_obj_set_style_text_color(password_input, qh_color(QH_INK), 0);
    lv_textarea_set_one_line(password_input, true);
    lv_textarea_set_password_mode(password_input, true);
    lv_textarea_set_password_show_time(password_input, 0);
    lv_textarea_set_max_length(password_input, 63);
    /* ASCII credentials and password bullets use the built-in Latin font. */
    lv_obj_set_style_text_font(password_input, LV_FONT_DEFAULT, 0);
    open_choice = lv_checkbox_create(editor);
    lv_checkbox_set_text(open_choice, "无密码网络");
    lv_obj_set_style_text_font(open_choice, ui_font, 0);
    lv_obj_set_style_text_color(open_choice, qh_color(QH_INK), 0);
    lv_obj_align(open_choice, LV_ALIGN_TOP_LEFT, 12, 434);
    if (open) lv_obj_add_state(open_choice, LV_STATE_CHECKED);
    editor_error = label(editor, "");
    lv_obj_set_width(editor_error, UI_WIDTH - 64);
    lv_obj_set_height(editor_error, ui_font->line_height*2);
    lv_label_set_long_mode(editor_error, LV_LABEL_LONG_WRAP);
    lv_obj_align(editor_error, LV_ALIGN_TOP_LEFT, 12, 502);
    lv_obj_set_style_text_color(editor_error, qh_color(QH_ERROR), 0);
    lv_obj_t *keyboard = lv_keyboard_create(editor);
    lv_obj_set_size(keyboard, UI_WIDTH - 40, 340);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(keyboard, LV_FONT_DEFAULT, LV_PART_ITEMS);
    qh_panel(keyboard, QH_BG, 0);
    lv_obj_set_style_bg_color(keyboard, qh_color(QH_SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, qh_color(QH_INK), LV_PART_ITEMS);
    lv_keyboard_set_textarea(keyboard, ssid[0] ? password_input : ssid_input);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(ssid_input, input_focus, LV_EVENT_FOCUSED, keyboard);
    lv_obj_add_event_cb(password_input, input_focus, LV_EVENT_FOCUSED, keyboard);
}

static void manual(lv_event_t *event) { (void)event; close_menu(); edit_network("", false); }
static bool connect_saved(const char *ssid)
{
    if (atomic_load(&occupied)) return false;
    command_t command = { .type = CMD_CONNECT };
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    const int index = book_find(&saved_book, ssid);
    if (index >= 0) command.credentials = saved_book.entries[index];
    const bool already_connected = atomic_load(&online) &&
        connected_generation == atomic_load(&link_generation) && strcmp(connected_ssid, ssid) == 0;
    xSemaphoreGive(snapshot_lock);
    bool handled = index >= 0;
    if (handled) {
        if (already_connected) atomic_store(&state, CONNECTED);
        else if (enqueue(&command)) atomic_store(&state, CONNECTING);
    }
    clear_private(&command, sizeof(command));
    return handled;
}

static void choose(lv_event_t *event)
{
    if (network_menu) return;
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    char ssid[33] = {0};
    bool open = false;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    if (displayed_revision == revision && index < displayed_count) {
        memcpy(ssid, displayed_networks[index].ssid, 32);
        open = displayed_networks[index].open;
    }
    xSemaphoreGive(snapshot_lock);
    if (ssid[0] && !connect_saved(ssid)) edit_network(ssid, open);
    clear_private(ssid, sizeof(ssid));
}

static void menu_cancel(lv_event_t *event) { (void)event; close_menu(); }
static void menu_edit(lv_event_t *event)
{
    (void)event;
    char ssid[33];
    memcpy(ssid, menu_ssid, sizeof(ssid));
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    const int index = book_find(&saved_book, ssid);
    const bool open = index >= 0 && !saved_book.entries[index].password[0];
    xSemaphoreGive(snapshot_lock);
    close_menu();
    edit_network(ssid, open);
    clear_private(ssid, sizeof(ssid));
}
static void menu_forget(lv_event_t *event)
{
    (void)event;
    command_t command = { .type = CMD_FORGET };
    memcpy(command.credentials.ssid, menu_ssid, sizeof(menu_ssid));
    if (enqueue(&command)) close_menu();
    clear_private(&command, sizeof(command));
}
static void manage(lv_event_t *event)
{
    if (atomic_load(&occupied)) return;
    const unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (index >= displayed_count || !displayed_networks[index].saved) return;
    close_menu();
    memcpy(menu_ssid, displayed_networks[index].ssid, sizeof(menu_ssid));
    network_menu = lv_obj_create(screen);
    lv_obj_set_size(network_menu, 560, 520);
    lv_obj_set_style_pad_all(network_menu, 16, 0);
    lv_obj_center(network_menu);
    qh_reveal(network_menu);
    lv_obj_set_scrollable(network_menu, false);
    lv_obj_set_style_bg_color(network_menu, qh_color(QH_SURFACE), 0);
    lv_obj_t *caption = label(network_menu, menu_ssid);
    lv_obj_set_width(caption, 510);
    lv_obj_set_height(caption, 54);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 8, 4);
    caption = label(network_menu, "忘记后再次连接需输密码，当前连接保持。\n修改密码仅在连接成功后保存。");
    lv_obj_set_width(caption, 510);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 8, 70);
    lv_obj_t *cancel = button(network_menu, "取消", 130, menu_cancel);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 8, -6);
    lv_obj_t *change = button(network_menu, "修改密码", 160, menu_edit);
    lv_obj_align(change, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t *forget = button(network_menu, "忘记网络", 160, menu_forget);
    lv_obj_set_style_bg_color(forget, qh_color(QH_DANGER), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(forget, 0), qh_color(QH_ON_ACCENT), 0);
    lv_obj_align(forget, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
}

static void scan(lv_event_t *event)
{
    (void)event;
    command_t command = { .type = CMD_SCAN };
    if (enqueue(&command)) atomic_store(&state, SCANNING);
}

/* Keep the list bottom anchored, while the preceding natural line boxes grow
 * for 14/28px fonts and wrapped connection/error messages. No reserved status
 * rows or hidden placeholder objects consume the space above the list. */
static void layout_network_page(void)
{
    if (!list || !footer) return;
    lv_obj_set_height(current_network, LV_SIZE_CONTENT);
    lv_obj_set_height(status, LV_SIZE_CONTENT);
    lv_obj_set_height(footer, LV_SIZE_CONTENT);
    lv_obj_update_layout(screen);
    const int controls_bottom = lv_obj_get_y(scan_button) + lv_obj_get_height(scan_button);
    lv_obj_set_pos(current_network, UI_MARGIN, controls_bottom + 12);
    lv_obj_set_pos(status, UI_MARGIN,
                   lv_obj_get_y(current_network) + lv_obj_get_height(current_network) + 8);
    const int top = lv_obj_get_y(status) + lv_obj_get_height(status) + 12;
    const int bottom = UI_HEIGHT - 22 - lv_obj_get_height(footer) - 12;
    lv_obj_set_pos(list, UI_MARGIN, top);
    lv_obj_set_size(list, UI_CONTENT_WIDTH, bottom > top ? bottom - top : 1);
    lv_obj_set_pos(footer, UI_MARGIN, UI_HEIGHT - 22 - lv_obj_get_height(footer));
}

static bool network_label_text(lv_obj_t *object, const char *text)
{
    if (!strcmp(lv_label_get_text(object), text)) return false;
    lv_label_set_text(object, text);
    return true;
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (lv_screen_active() != screen) return;
    static const char *messages[] = {
        "未连接：选择附近网络，或手动输入名称", "正在扫描附近网络...", "正在连接，失败后恢复原网络...",
        "Wi-Fi 已连接；新连接成功后会自动保存", "扫描失败，请稍后重新扫描",
        "连接失败，正在恢复原网络，请检查密码或信号", "已连接，但保存失败，重启后可能恢复原网络",
        "连接未启动，请稍后重试", "原网络恢复失败，请重新选择网络或重启设备",
        "已忘记网络；当前连接保持，再次切回需输密码", "忘记失败，已保存的网络保留，请重试"
    };
    int current = atomic_load(&state);
    if (current == IDLE && atomic_load(&online)) current = CONNECTED;
    if ((current == CONNECTED || current == SAVE_FAILED) && !atomic_load(&online) && !atomic_load(&occupied)) current = IDLE;
    bool layout_changed = network_label_text(status, messages[current]);
    bool busy = atomic_load(&occupied);
    if (busy) { lv_obj_add_state(scan_button, LV_STATE_DISABLED); lv_obj_add_state(manual_button, LV_STATE_DISABLED); }
    else { lv_obj_remove_state(scan_button, LV_STATE_DISABLED); lv_obj_remove_state(manual_button, LV_STATE_DISABLED); }
    network_row_t copy[AP_LIMIT + SAVED_LIMIT] = {0};
    if (xSemaphoreTake(snapshot_lock, 0) != pdTRUE) {
        if (layout_changed) layout_network_page();
        return;
    }
    char connection_text[112];
    if (!atomic_load(&online)) snprintf(connection_text, sizeof(connection_text), "当前连接：未连接");
    else if (connected_generation != atomic_load(&link_generation))
        snprintf(connection_text, sizeof(connection_text), "当前连接：正在读取网络名称...");
    else snprintf(connection_text, sizeof(connection_text), "当前连接：%s", connected_ssid);
    /* Release the snapshot lock before layout or text/font measurement. */
    if (displayed_revision == revision) {
        xSemaphoreGive(snapshot_lock);
        layout_changed |= network_label_text(current_network, connection_text);
        if (layout_changed) layout_network_page();
        clear_private(connection_text, sizeof(connection_text));
        return;
    }
    unsigned count = network_count;
    for (unsigned i = 0; i < network_count; ++i) {
        memcpy(copy[i].ssid, networks[i].ssid, 32);
        copy[i].open = networks[i].authmode == WIFI_AUTH_OPEN;
        copy[i].rssi = networks[i].rssi;
        copy[i].nearby = true;
        copy[i].saved = book_find(&saved_book, copy[i].ssid) >= 0;
    }
    for (unsigned i = 0; i < saved_book.count; ++i) {
        bool found = false;
        for (unsigned j = 0; j < count; ++j)
            if (strcmp(copy[j].ssid, saved_book.entries[i].ssid) == 0) found = true;
        if (found) continue;
        memcpy(copy[count].ssid, saved_book.entries[i].ssid, 33);
        copy[count].open = !saved_book.entries[i].password[0];
        copy[count].saved = true;
        ++count;
    }
    memcpy(displayed_networks, copy, sizeof(copy));
    displayed_count = count;
    displayed_revision = revision;
    xSemaphoreGive(snapshot_lock);
    layout_changed |= network_label_text(current_network, connection_text);
    clear_private(connection_text, sizeof(connection_text));
    if (layout_changed) layout_network_page();
    lv_obj_clean(list);
    if (!count) {
        lv_obj_t *empty = label(list, "未找到可见网络，可以重新扫描或手动输入");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    }
    for (unsigned i = 0; i < count; ++i) {
        lv_obj_t *row = button(list, "", 520, choose);
        lv_obj_set_style_pad_all(row, 12, 0);
        lv_obj_set_height(row, ui_font->line_height*2 + 32);
        lv_obj_set_style_bg_color(row, qh_color(QH_SURFACE), 0);
        lv_obj_remove_event_cb(row, choose);
        lv_obj_add_event_cb(row, choose, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *name = label(row, copy[i].ssid);
        lv_obj_set_width(name, 350);
        lv_obj_set_height(name, ui_font->line_height);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
        char details[80];
        snprintf(details, sizeof(details), "%s / %s", copy[i].saved ? "已保存" : copy[i].open ? "开放" : "加密",
            !copy[i].nearby ? "未发现" : copy[i].rssi >= -60 ? "强" : copy[i].rssi >= -75 ? "中" : "弱");
        lv_obj_t *signal = label(row, details);
        lv_obj_align(signal, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        if (copy[i].saved) {
            lv_obj_t *settings = button(row, "管理", 110, manage);
            lv_obj_remove_event_cb(settings, manage);
            lv_obj_add_event_cb(settings, manage, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
            lv_obj_align(settings, LV_ALIGN_RIGHT_MID, -2, 0);
        }
    }
    clear_private(copy, sizeof(copy));
}

static void update_labels(lv_obj_t *object)
{
    if (lv_obj_check_type(object, &lv_label_class)) lv_obj_set_style_text_font(object, ui_font, 0);
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) update_labels(lv_obj_get_child(object, i));
}

void wifi_settings_set_font(const lv_font_t *font)
{
    ui_font = settings_cached_font(font);
    if (screen) {
        if (!editor) update_labels(screen);
        displayed_revision = UINT_MAX;
        layout_network_page();
    }
}

void wifi_settings_show(lv_obj_t *parent, const lv_font_t *font)
{
    return_screen = parent;
    wifi_settings_set_font(font);
    if (!commands || !snapshot_lock) return;
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, qh_color(QH_BG), 0);
        lv_obj_set_scrollable(screen, false);
        qh_screen(screen);
        lv_obj_t *caption=qh_header(screen,"Wi-Fi",ui_font,back,true);
        scan_button = button(screen, "重新扫描", 170, scan);
        lv_obj_align(scan_button, LV_ALIGN_TOP_LEFT, 28, 100);
        manual_button = button(screen, "手动连接", 170, manual);
        lv_obj_align(manual_button, LV_ALIGN_TOP_RIGHT, -28, 100);
        status = label(screen, "正在准备...");
        lv_obj_set_width(status, UI_CONTENT_WIDTH);
        lv_obj_set_height(status, LV_SIZE_CONTENT);
        lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
        current_network = label(screen, "当前连接：正在读取网络名称...");
        lv_obj_set_width(current_network, UI_CONTENT_WIDTH);
        lv_obj_set_height(current_network, LV_SIZE_CONTENT);
        lv_label_set_long_mode(current_network, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(current_network, qh_color(QH_MUTED), 0);
        list = lv_obj_create(screen);
        lv_obj_set_size(list, UI_CONTENT_WIDTH, 1);
        lv_obj_set_style_pad_all(list, 12, 0);
        lv_obj_set_style_bg_color(list, qh_color(QH_BG), 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_style_pad_row(list, 10, 0);
        footer = label(screen, "2.4GHz · 最多保存8个网络");
        lv_obj_set_width(footer, UI_CONTENT_WIDTH);
        lv_label_set_long_mode(footer, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
        layout_network_page();
        displayed_revision = UINT_MAX;
        lv_timer_create(refresh, 250, NULL);
    }
    qh_screen_load(screen, false);
    scan(NULL);
}
