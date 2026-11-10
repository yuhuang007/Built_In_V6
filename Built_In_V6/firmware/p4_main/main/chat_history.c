#include "chat_history.h"
#include "ui_layout.h"
#include "qinghe_ui.h"
#include "chat_store_codec.h"
#include "asr_display_font.h"
#include "speech_to_text_mode.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct {
    uint32_t id, turn;
    unsigned role;
    int sequence;
    size_t prefix;
    char text[CHAT_TEXT_BYTES];
} message_t;
static message_t *messages, *snapshot;
static SemaphoreHandle_t model_lock;
static TaskHandle_t storage_task;
static unsigned message_count;
static uint32_t turn_number, next_id = 1, revision, saved_revision;
static uint32_t cleared_through_turn;
static bool clear_pending, cleared_once;
/* 0 unavailable, 1 loading, 2 pending, 3 saved, 4 error, 5 clearing. */
static unsigned storage_state;
static lv_obj_t *viewport, *empty_label, *rows[CHAT_MAX_MESSAGES], *labels[CHAT_MAX_MESSAGES];
static lv_obj_t *bubbles[CHAT_MAX_MESSAGES];
static uint32_t rendered_ids[CHAT_MAX_MESSAGES];
static unsigned rendered_count;
static uint32_t rendered_revision = UINT32_MAX;
static const lv_font_t *ui_font;
/* Chat redraws many glyphs while scrolling. Keep decoded glyphs in bounded
 * PSRAM caches instead of seeking the SD font file on every redraw. These
 * wrappers borrow the boot fonts and live as long as the home screen. */
static lv_font_t *chat_fonts[2];
static const lv_font_t *chat_display_font(const lv_font_t *font)
{
#if CONFIG_P4_ASR_MODE
    int slot = font == speech_to_text_mode_font14() ? 0 :
               font == speech_to_text_mode_font28() ? 1 : -1;
    if (!font || slot < 0) return font;
    if (!chat_fonts[slot]) chat_fonts[slot] = asr_display_font_create_native(font);
    return chat_fonts[slot] ? chat_fonts[slot] : font;
#else
    return font;
#endif
}
static const char *paths[] = {"/sdcard/CHATLOG/H0.BIN", "/sdcard/CHATLOG/H1.BIN"};

static bool lock_model(void) { return model_lock && xSemaphoreTake(model_lock, portMAX_DELAY) == pdTRUE; }
static void unlock_model(void) { xSemaphoreGive(model_lock); }
static void notify_storage(void) { if (storage_task) xTaskNotifyGive(storage_task); }

esp_err_t chat_history_init(void)
{
    model_lock = xSemaphoreCreateMutex();
    messages = heap_caps_calloc(CHAT_MAX_MESSAGES, sizeof(message_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snapshot = heap_caps_calloc(CHAT_MAX_MESSAGES, sizeof(message_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!model_lock || !messages || !snapshot) {
        heap_caps_free(messages); heap_caps_free(snapshot); messages = snapshot = NULL;
        if (model_lock) vSemaphoreDelete(model_lock);
        model_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t chat_history_current_turn(void)
{
    if (!lock_model()) return 0;
    uint32_t result = turn_number; unlock_model(); return result;
}
uint32_t chat_history_begin_turn(void)
{
    if (!lock_model()) return 0;
    uint32_t result = ++turn_number; unlock_model(); return result;
}
void chat_history_update(chat_role_t role, uint32_t turn, const char *text, int sequence)
{
    if (!text || !text[0] || role > CHAT_UNKNOWN || !lock_model()) return;
    if (cleared_once && turn <= cleared_through_turn) { unlock_model(); return; }
    message_t *message = NULL;
    for (unsigned i = message_count; i > 0; --i) {
        if (messages[i - 1].turn == turn && messages[i - 1].role == (unsigned)role) {
            message = &messages[i - 1]; break;
        }
    }
    if (!message) {
        if (message_count == CHAT_MAX_MESSAGES) {
            memmove(messages, messages + 1, (CHAT_MAX_MESSAGES - 1) * sizeof(message_t));
            --message_count;
        }
        message = &messages[message_count++];
        memset(message, 0, sizeof(*message));
        message->id = next_id++; message->role = role; message->turn = turn;
        message->sequence = sequence;
    }
    if (chat_stream_update(message->text, sizeof(message->text), &message->prefix,
                           &message->sequence, text, sequence, role == CHAT_USER)) {
        ++revision;
        if (storage_state != 0 && storage_state != 1) storage_state = clear_pending ? 5 : 2;
        notify_storage();
    }
    unlock_model();
}

unsigned chat_history_count(void)
{
    if (!lock_model()) return 0;
    unsigned count = message_count; unlock_model(); return count;
}
const char *chat_history_storage_status(void)
{
    static const char *texts[] = {"未保存", "读取中", "保存中", "已保存", "保存失败", "清除中"};
    if (!lock_model()) return texts[0];
    unsigned state = storage_state; unlock_model(); return texts[state];
}
bool chat_history_loading(void)
{
    if (!lock_model()) return false;
    bool loading = storage_state == 1; unlock_model(); return loading;
}
void chat_history_request_clear(void)
{
    if (!lock_model()) return;
    /* In-flight callbacks for the removed turn cannot recreate its records. */
    message_count = 0; cleared_through_turn = turn_number; cleared_once = true;
    clear_pending = true; ++revision; storage_state = storage_task ? 5 : 4;
    notify_storage(); unlock_model();
}

static bool file_owned(unsigned slot)
{
    FILE *file = fopen(paths[slot], "rb");
    if (!file) return errno == ENOENT;
    uint8_t header[CHAT_HEADER_BYTES] = {0}; uint32_t generation, count, crc;
    size_t length = fread(header, 1, sizeof(header), file);
    bool result = length == sizeof(header) && chat_header_decode(header, &generation, &count, &crc);
    if (length < sizeof(header) && !ferror(file)) {
        /* A torn write can leave the inactive slot empty or with a partial
         * format header. Only an empty file or our exact magic prefix qualifies. */
        uint8_t expected[CHAT_HEADER_BYTES]; chat_header_encode(expected, 1, 0, 0);
        result = memcmp(header, expected, length < 8 ? length : 8) == 0;
    }
    fclose(file); return result;
}

static bool read_slot(unsigned slot, uint32_t *generation, unsigned *count)
{
    FILE *file = fopen(paths[slot], "rb");
    if (!file) return false;
    uint8_t header[CHAT_HEADER_BYTES] = {0}, record[CHAT_RECORD_BYTES];
    uint32_t expected_crc, stored_count;
    bool valid = fread(header, 1, sizeof(header), file) == sizeof(header) &&
                 chat_header_decode(header, generation, &stored_count, &expected_crc);
    uint32_t crc = chat_crc_update(UINT32_MAX, header, 16);
    for (unsigned i = 0; valid && i < stored_count; ++i) {
        unsigned role, length; uint32_t turn;
        valid = fread(record, 1, sizeof(record), file) == sizeof(record) &&
                chat_record_decode(record, &role, &length, &turn);
        if (!valid) break;
        message_t *message = &snapshot[i];
        memset(message, 0, sizeof(*message));
        valid = fread(message->text, 1, length, file) == length &&
                memchr(message->text, '\0', length) == NULL;
        if (!valid) break;
        message->text[length] = '\0';
        /* Validate UTF-8 in place; malformed/truncated records are rejected. */
        valid = chat_utf8_copy(message->text, sizeof(message->text), message->text) == length;
        message->role = role; message->turn = turn; message->sequence = -1;
        crc = chat_crc_update(crc, record, sizeof(record));
        crc = chat_crc_update(crc, (const uint8_t *)message->text, length);
    }
    valid = valid && (crc ^ UINT32_MAX) == expected_crc && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (valid) *count = stored_count;
    return valid;
}

static bool write_slot(unsigned slot, uint32_t generation, unsigned count)
{
    if (!file_owned(slot)) return false;
    FILE *file = fopen(paths[slot], "wb");
    if (!file) return false;
    uint8_t header[CHAT_HEADER_BYTES], record[CHAT_RECORD_BYTES];
    chat_header_encode(header, generation, count, 0);
    uint32_t crc = chat_crc_update(UINT32_MAX, header, 16);
    bool valid = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    for (unsigned i = 0; valid && i < count; ++i) {
        const message_t *message = &snapshot[i];
        unsigned length = strlen(message->text);
        chat_record_encode(record, message->role, length, message->turn);
        crc = chat_crc_update(crc, record, sizeof(record));
        crc = chat_crc_update(crc, (const uint8_t *)message->text, length);
        valid = fwrite(record, 1, sizeof(record), file) == sizeof(record) &&
                fwrite(message->text, 1, length, file) == length;
    }
    chat_header_encode(header, generation, count, crc ^ UINT32_MAX);
    valid = valid && fseek(file, 0, SEEK_SET) == 0 &&
            fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
            fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) valid = false;
    return valid;
}

static void history_worker(void *argument)
{
    (void)argument;
    struct stat directory;
    bool available = (mkdir("/sdcard/CHATLOG", 0775) == 0 || errno == EEXIST) &&
                     stat("/sdcard/CHATLOG", &directory) == 0 && S_ISDIR(directory.st_mode);
    unsigned active_slot = 1, loaded_count = 0; uint32_t disk_generation = 0;
    if (available) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            uint32_t generation = 0; unsigned count = 0;
            if (read_slot(slot, &generation, &count) && generation > disk_generation) {
                disk_generation = generation; active_slot = slot; loaded_count = count;
            }
        }
        if (disk_generation && !read_slot(active_slot, &disk_generation, &loaded_count)) available = false;
    }
    bool need_save = false;
    if (lock_model()) {
        const unsigned live_count = message_count;
        if (available && disk_generation && !cleared_once) {
            unsigned keep = loaded_count > CHAT_MAX_MESSAGES - message_count ?
                            CHAT_MAX_MESSAGES - message_count : loaded_count;
            memmove(messages + keep, messages, message_count * sizeof(message_t));
            memcpy(messages, snapshot + loaded_count - keep, keep * sizeof(message_t));
            message_count += keep;
            for (unsigned i = 0; i < message_count; ++i) {
                messages[i].id = next_id++;
                if (messages[i].turn >= turn_number) turn_number = messages[i].turn + 1;
            }
            ++revision;
        }
        /* No existing files is a valid fresh store; corrupted/foreign files
         * are never silently overwritten, nor presented as empty saved history. */
        if (!disk_generation && available &&
            (access(paths[0], F_OK) == 0 || access(paths[1], F_OK) == 0)) available = false;
        need_save = available && (live_count != 0 || clear_pending);
        storage_state = !available ? 4 : clear_pending ? 5 : need_save ? 2 : 3;
        if (!need_save && available) saved_revision = revision;
        unlock_model();
    }
    if (need_save) notify_storage();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1200)) != 0) {
            if (lock_model()) { bool clear = clear_pending; unlock_model(); if (clear) break; }
        }
        if (!lock_model()) continue;
        unsigned count = message_count; uint32_t target_revision = revision;
        bool clear = clear_pending;
        memcpy(snapshot, messages, count * sizeof(message_t));
        unlock_model();
        unsigned target_slot = 1 - active_slot;
        bool result = available && disk_generation != UINT32_MAX &&
                      write_slot(target_slot, disk_generation + 1, count);
        if (result) { active_slot = target_slot; ++disk_generation; }
        if (result && clear) {
            const unsigned old_slot = 1 - active_slot;
            if (access(paths[old_slot], F_OK) == 0)
                result = file_owned(old_slot) && unlink(paths[old_slot]) == 0;
        }
        if (lock_model()) {
            if (result) {
                saved_revision = target_revision;
                if (clear) clear_pending = false;
                storage_state = revision == saved_revision ? 3 : clear_pending ? 5 : 2;
            } else storage_state = 4;
            bool changed = revision != target_revision;
            unlock_model();
            if (changed) notify_storage();
        }
        ESP_LOGI("chat", "history_write success=%d count=%u cleared=%d", result, count, clear && result);
    }
}

esp_err_t chat_history_start_storage(void)
{
    if (!model_lock) return ESP_ERR_INVALID_STATE;
    if (lock_model()) { storage_state = 1; unlock_model(); }
    if (xTaskCreate(history_worker, "chat_sd", 4096, NULL, 1, &storage_task) != pdPASS) {
        if (lock_model()) { storage_state = 4; unlock_model(); }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void resize_bubble(lv_obj_t *row, unsigned role)
{
    lv_obj_set_flex_align(row, role == CHAT_USER ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}
/* One clear line icon per avatar; message model/storage remain unchanged. */
static void create_avatar(lv_obj_t *row, unsigned role)
{
    lv_obj_t *avatar=lv_obj_create(row);qh_panel(avatar,role==CHAT_USER?QH_USER:QH_SOFT,12);
    lv_obj_set_size(avatar,44,44);lv_obj_set_style_pad_all(avatar,0,0);
    lv_obj_set_style_border_width(avatar,1,0);lv_obj_set_style_border_color(avatar,qh_color(QH_LINE),0);
    lv_obj_set_scrollable(avatar,false);lv_obj_set_clickable(avatar,false);
    lv_obj_center(qh_icon(avatar,role==CHAT_USER?QH_ICON_USER:QH_ICON_ROBOT));
}
static void update_bubble(unsigned i, const char *text)
{
    lv_point_t size;
    /* Leave room for padding and an avatar on either side of the transcript. */
    lv_text_get_size(&size, text, ui_font,
        lv_obj_get_style_text_letter_space(labels[i], 0),
        lv_obj_get_style_text_line_space(labels[i], 0), UI_BUBBLE_TEXT_WIDTH, LV_TEXT_FLAG_NONE);
    int width = size.x < 20 ? 20 : size.x > UI_BUBBLE_TEXT_WIDTH ? UI_BUBBLE_TEXT_WIDTH : size.x;
    lv_obj_set_width(bubbles[i], width + 24);
    lv_obj_set_width(labels[i], width);
    if (text != lv_label_get_text(labels[i])) lv_label_set_text(labels[i], text);
}
static void render_history(lv_timer_t *timer)
{
    (void)timer;
    if (!viewport || lv_obj_get_screen(viewport) != lv_screen_active() || !lock_model()) return;
    if (revision == rendered_revision) { unlock_model(); return; }
    /* Preserve position while reading older messages. At the bottom, follow
     * new text and turns; first load and explicit clear reset the view. */
    bool follow = rendered_count == 0 || lv_obj_get_scroll_bottom(viewport) < 32;
    bool rebuild = rendered_count > message_count;
    for (unsigned i = 0; !rebuild && i < rendered_count; ++i)
        if (rendered_ids[i] != messages[i].id) rebuild = true;
    if (rebuild) { lv_obj_clean(viewport); rendered_count = 0; empty_label = NULL; }
    if (!message_count) {
        if (!empty_label) {
            empty_label = lv_label_create(viewport);
            lv_obj_set_style_text_font(empty_label, ui_font, 0);
            lv_obj_set_style_text_color(empty_label, qh_color(QH_MUTED), 0);
            lv_label_set_text(empty_label, "按住下方按钮开始对话");
        }
    } else {
        if (empty_label) { lv_obj_delete(empty_label); empty_label = NULL; }
        for (unsigned i = 0; i < message_count; ++i) {
            message_t *message = &messages[i];
            if (i >= rendered_count) {
                lv_obj_t *row = lv_obj_create(viewport);
                rows[i] = row; rendered_ids[i] = message->id;
                lv_obj_remove_style_all(row);
                lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
                lv_obj_set_scrollable(row, false);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_style_pad_column(row, 8, 0);
                resize_bubble(row, message->role);
                if (message->role != CHAT_USER) create_avatar(row, message->role);
                lv_obj_t *bubble = lv_obj_create(row);
                bubbles[i] = bubble;
                lv_obj_remove_style_all(bubble);
                lv_obj_set_size(bubble, UI_BUBBLE_TEXT_WIDTH + 24, LV_SIZE_CONTENT);
                lv_obj_set_style_pad_all(bubble, 11, 0);
                lv_obj_set_style_border_width(bubble,1,0);
                lv_obj_set_style_border_color(bubble,qh_color(QH_LINE),0);
                lv_obj_set_style_radius(bubble, 24, 0);
                lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(bubble, qh_color(message->role == CHAT_USER ?
                    QH_USER : message->role == CHAT_ASSISTANT ? QH_ASSISTANT : QH_SOFT), 0);
                lv_obj_set_scrollable(bubble, false);
                lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
                labels[i] = lv_label_create(bubble);
                lv_obj_set_width(labels[i], LV_PCT(100));
                lv_label_set_long_mode(labels[i], LV_LABEL_LONG_WRAP);
                lv_obj_set_style_text_font(labels[i], ui_font, 0);
                lv_obj_set_style_text_color(labels[i], qh_color(QH_INK), 0);
                update_bubble(i, message->text);
                if (message->role == CHAT_USER) create_avatar(row, message->role);
            } else if (strcmp(lv_label_get_text(labels[i]), message->text) != 0) {
                update_bubble(i, message->text);
            }
        }
    }
    rendered_count = message_count; rendered_revision = revision;
    unlock_model();
    if (follow) { lv_obj_update_layout(viewport); lv_obj_scroll_to_y(viewport, LV_COORD_MAX, LV_ANIM_OFF); }
}

void chat_history_create_ui(lv_obj_t *parent, const lv_font_t *font)
{
    ui_font = chat_display_font(font);
    viewport = lv_obj_create(parent);
    lv_obj_remove_style_all(viewport);
    lv_obj_set_size(viewport, UI_CONTENT_WIDTH, UI_CHAT_HEIGHT);
    lv_obj_align(viewport, LV_ALIGN_TOP_MID, 0, UI_CHAT_TOP);
    lv_obj_set_style_pad_all(viewport, 12, 0);
    lv_obj_set_style_pad_top(viewport,0,0);
    lv_obj_set_style_pad_row(viewport, 8, 0);
    lv_obj_set_style_bg_color(viewport, qh_color(QH_BG), 0);
    lv_obj_set_style_bg_opa(viewport, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(viewport, 16, 0);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_elastic(viewport, false);
    if (!model_lock) {
        empty_label = lv_label_create(viewport);
        lv_obj_set_style_text_font(empty_label, font, 0);
        lv_label_set_text(empty_label, "对话历史初始化失败，请重启后重试");
    }
    lv_timer_create(render_history, 150, NULL);
}
void chat_history_set_font(const lv_font_t *font)
{
    font = chat_display_font(font);
    ui_font = font;
    if (viewport) {
        if (empty_label) lv_obj_set_style_text_font(empty_label, font, 0);
        for (unsigned i = 0; i < rendered_count; ++i) {
            lv_obj_set_style_text_font(labels[i], font, 0);
            update_bubble(i, lv_label_get_text(labels[i]));
        }
        /* New bubbles use this borrowed font. Reflow follows only at the bottom. */
        rendered_revision = UINT32_MAX;
    }
}

void chat_history_set_view_height(int height)
{
    if (viewport) lv_obj_set_height(viewport, height);
}

void chat_history_set_view_bounds(int top,int bottom)
{
    if(!viewport)return;
    const bool follow=lv_obj_get_scroll_bottom(viewport)<32;
    const int scroll=lv_obj_get_scroll_y(viewport);
    const int height=bottom-top>40?bottom-top:40;
    if(lv_obj_get_y(viewport)==top && lv_obj_get_height(viewport)==height)return;
    lv_obj_set_y(viewport,top);lv_obj_set_height(viewport,height);
    lv_obj_update_layout(viewport);
    lv_obj_scroll_to_y(viewport,follow?LV_COORD_MAX:scroll,LV_ANIM_OFF);
}
