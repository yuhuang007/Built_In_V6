#include "text_entry.h"
#include "ui_layout.h"
#include "pinyin_service.h"
#if CONFIG_P4_SIGN_LANGUAGE_MODE
#include "portrait_display.h"
#endif
#include <stdatomic.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static lv_obj_t *editor, *input, *keyboard, *language, *status, *send, *cancel;
static lv_obj_t *composition, *candidates, *previous, *next, *candidate_panel;
static lv_obj_t *language_label;
static text_entry_submit_fn submit_text;
static void *submit_context;
static bool pending, lookup_busy, choosing, decoder_ready;
static atomic_int completion;
static uint32_t revision;
static uint32_t lookup_started;
static unsigned page_offset, candidate_count, candidate_total;
static char spelling[P4_PINYIN_MAX + 1];
static uint32_t deferred_keys[32];
static unsigned deferred_count;
static char candidate_labels[P4_PINYIN_PAGE][P4_PINYIN_TEXT + 4];
static const char *candidate_map[P4_PINYIN_PAGE * 2];
typedef struct {
    lv_obj_t *parent;
    const lv_font_t *font;
    text_entry_submit_fn submit;
    void *context;
    text_entry_layout_fn layout;
    void *layout_context;
    bool enabled;
    unsigned language;
    int top;
    lv_obj_t *language_button;
    char draft[2001];
    char spelling[P4_PINYIN_MAX + 1];
} dock_owner_t;
static dock_owner_t owners[2], *owner;
static bool expanded, touch_keyboard;
static bool status_visible;
static int cpu_region_top = UI_HEIGHT;
static void sync_dock(void);
static void build_dock(dock_owner_t *target);
static void layout_dock(void);
static esp_timer_handle_t health_timer;
static atomic_bool health_active;
static atomic_uint health_ticks, health_stage, health_refresh, health_keys, health_draw;

/* Independent of the UI task: detect stalls without reading or logging text. */
static void health_report(void *unused)
{
    (void)unused;
    if (!atomic_load(&health_active)) return;
    ESP_LOGI("text_entry", "health ticks=%u stage=%u refresh=%u keys=%u draw=%u internal=%u external=%u",
             atomic_load(&health_ticks), atomic_load(&health_stage),
             atomic_load(&health_refresh), atomic_load(&health_keys), atomic_load(&health_draw),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void refresh_event(lv_event_t *event)
{
    atomic_store(&health_refresh, lv_event_get_code(event));
}

static void draw_event(lv_event_t *event)
{
    const unsigned id = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    atomic_store(&health_draw, id * 2 + (lv_event_get_code(event) == LV_EVENT_DRAW_MAIN_BEGIN ? 0 : 1));
}

bool text_entry_active(void) { return editor && owner && owner->parent == lv_screen_active(); }
bool text_entry_is_root(lv_obj_t *object) { return object == editor || object == candidate_panel; }
void text_entry_error(const char *message)
{
    if (!status) return;
    lv_label_set_text(status, message);
    status_visible = message && message[0];
    if (status_visible) expanded = true;
    layout_dock();
}
void text_entry_finish(bool success) { atomic_store(&completion, success ? 1 : -1); }
static bool chinese(void) { return owner && owner->language == 0; }

/* Probe only fixed UI symbols, never user input or candidate contents. */
static void report_font_probe(void)
{
    const uint32_t symbols[] = {'E', 0x62fc};
    unsigned visible = 0;
    for (unsigned i = 0; i < 2; ++i) {
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(owner->font, &glyph, symbols[i], 0) ||
            glyph.is_placeholder || !glyph.box_w || !glyph.box_h) continue;
        lv_draw_buf_t *buffer = lv_draw_buf_create(glyph.box_w, glyph.box_h, LV_COLOR_FORMAT_A8, 0);
        if (!buffer) continue;
        const lv_draw_buf_t *pixels = lv_font_get_glyph_bitmap(&glyph, buffer);
        bool nonblank = false;
        if (pixels) {
            for (unsigned y = 0; y < glyph.box_h && !nonblank; ++y)
                for (unsigned x = 0; x < glyph.box_w; ++x)
                    if (pixels->data[y * pixels->header.stride + x]) { nonblank = true; break; }
        }
        visible += nonblank;
        lv_font_glyph_release_draw_data(&glyph);
        lv_draw_buf_destroy(buffer);
    }
    ESP_LOGI("text_entry", "fixed_ui_glyphs_visible=%u expected=2", visible);
}

static void refresh_language_label(void)
{
    if (!language_label || !owner) return;
    lv_obj_set_style_text_font(language_label, owner->font, 0);
    lv_obj_set_style_text_color(language_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(language_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(language_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(language_label, 52, owner->font->line_height);
    lv_label_set_long_mode(language_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(language_label, chinese() ? "拼" : "E");
    lv_obj_center(language_label);
}

static void clear_candidates(void)
{
    candidate_count = candidate_total = 0;
    lv_obj_set_hidden(candidates, true);
    lv_obj_set_hidden(previous, true);
    lv_obj_set_hidden(next, true);
}

static void lookup(int choose)
{
    lookup_busy = true;
    lookup_started = lv_tick_get();
    choosing = choose >= 0;
    clear_candidates();
    lv_label_set_text(composition, spelling);
    layout_dock();
    if (!pinyin_service_request(++revision, spelling, page_offset, choose)) {
        lookup_busy = choosing = false;
        text_entry_error("输入法启动失败，可切换英文");
    } else if (!decoder_ready) text_entry_error("正在准备离线词库...");
    else text_entry_error("");
}

static void reset_composition(void)
{
    ++revision; /* Discard results from a closed editor or old composition. */
    spelling[0] = 0;
    deferred_count = 0;
    lookup_busy = choosing = false;
    page_offset = 0;
    lv_label_set_text(composition, "");
    clear_candidates();
}

static void deleted(lv_event_t *event)
{
    (void)event;
    atomic_store(&health_active, false);
#if CONFIG_P4_SIGN_LANGUAGE_MODE
    portrait_editor_cpu_copy(false);
#endif
    ++revision;
    memset(spelling, 0, sizeof(spelling));
    memset(deferred_keys, 0, sizeof(deferred_keys));
    deferred_count = 0;
    if (candidate_panel) lv_obj_delete(candidate_panel);
    editor = input = keyboard = language = status = send = cancel = NULL;
    composition = candidates = previous = next = candidate_panel = NULL;
    language_label = NULL;
    submit_text = NULL;
    pending = lookup_busy = choosing = false;
    owner = NULL;
    status_visible = false;
    cpu_region_top = UI_HEIGHT;
    atomic_store(&completion, 0);
}

static void close_editor(lv_event_t *event)
{
    (void)event;
    if (editor && !pending) {
        expanded = touch_keyboard = false;
        /* Hidden keyboards still retain their button/style allocations. */
        if (keyboard) {
            lv_obj_t *old_keyboard = keyboard;
            keyboard = NULL;
            lv_obj_delete(old_keyboard);
        }
        layout_dock();
    }
}

static void choose_candidate(unsigned index)
{
    if (!pending && !lookup_busy && index < candidate_count)
        lookup((int)(page_offset + index));
}

static void submit(lv_event_t *event)
{
    (void)event;
    if (!editor || pending || choosing) return;
    /* First confirm commits composition; a second press submits text. */
    if (spelling[0]) { choose_candidate(0); return; }
    const char *text = lv_textarea_get_text(input);
    bool content = false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p > ' ') { content = true; break; }
    if (!content) { text_entry_error("请输入文字"); return; }
    atomic_store(&completion, 0);
    if (!submit_text(text, submit_context)) return;
    pending = true;
    lv_obj_add_state(send, LV_STATE_DISABLED);
    lv_obj_add_state(cancel, LV_STATE_DISABLED);
    lv_obj_add_state(input, LV_STATE_DISABLED);
    if (keyboard) lv_obj_add_state(keyboard, LV_STATE_DISABLED);
    lv_obj_add_state(language, LV_STATE_DISABLED);
    text_entry_error("正在提交...");
}

static void poll_completion(lv_timer_t *timer)
{
    (void)timer;
    sync_dock();
    atomic_fetch_add(&health_ticks, 1);
    atomic_store(&health_stage, 1);
    pinyin_response_t response;
    bool received = pinyin_service_poll(&response);
    if (received)
        ESP_LOGI("text_entry", "pinyin_result=1 matched=%d ready=%d error=%d",
                 editor && response.revision == revision, response.initialized, response.error);
    if (received && editor && response.revision == revision) {
        atomic_store(&health_stage, 2);
        lookup_busy = false;
        decoder_ready = response.initialized;
        if (response.error) {
            choosing = false;
            text_entry_error("词库未就绪，可切换英文后重试");
        } else if (choosing) {
            choosing = false;
            uint32_t keys[32];
            unsigned key_count = deferred_count;
            memcpy(keys, deferred_keys, key_count * sizeof(*keys));
            memset(deferred_keys, 0, sizeof(deferred_keys));
            deferred_count = 0;
            uint32_t position = lv_textarea_get_cursor_pos(input);
            lv_textarea_set_cursor_pos(input, LV_TEXTAREA_CURSOR_LAST);
            uint32_t length = lv_textarea_get_cursor_pos(input);
            lv_textarea_set_cursor_pos(input, position);
            unsigned added = 0;
            for (const unsigned char *p = (const unsigned char *)response.result.committed; *p; ++p)
                if ((*p & 0xc0) != 0x80) ++added;
            if (length + added > lv_textarea_get_max_length(input)) {
                lookup(-1);
                text_entry_error("文字已达长度上限，请先删减");
            } else {
                lv_textarea_add_text(input, response.result.committed);
                strcpy(spelling, response.result.remaining);
                page_offset = 0;
                lookup(-1);
            }
            for (unsigned i = 0; i < key_count && editor && !pending; ++i) text_entry_key(keys[i]);
        } else {
            page_offset = response.result.offset;
            candidate_count = response.result.count;
            candidate_total = response.result.total;
            for (unsigned i = 0; i < P4_PINYIN_PAGE; ++i)
                snprintf(candidate_labels[i], sizeof(candidate_labels[i]), "%u %s", i + 1,
                         i < candidate_count ? response.result.words[i] : "");
            atomic_store(&health_stage, 3);
            atomic_store(&health_stage, 4);
            lv_obj_set_hidden(candidates, !expanded || !candidate_count);
            lv_obj_set_hidden(previous, !expanded || !candidate_count);
            lv_obj_set_hidden(next, !expanded || !candidate_count);
            lv_obj_set_state(previous, LV_STATE_DISABLED, page_offset == 0);
            lv_obj_set_state(next, LV_STATE_DISABLED, page_offset + candidate_count >= candidate_total);
            atomic_store(&health_stage, 5);
            if (spelling[0] && candidate_total) {
                char summary[48];
                snprintf(summary, sizeof(summary), "%u-%u / %u", page_offset + 1,
                         page_offset + candidate_count, candidate_total);
                text_entry_error(summary);
            } else text_entry_error(spelling[0] ? "没有候选，请检查拼音" : "");
        }
    }
    if (editor && lookup_busy && lv_tick_get() - lookup_started >= 15000) {
        lookup_busy = choosing = false;
        text_entry_error("输入法响应超时，请关闭编辑后重试");
        ESP_LOGW("text_entry", "pinyin_timeout=1 state=%d", pinyin_service_state());
    }
    atomic_store(&health_stage, 0);
    if (!editor || !pending) return;
    int result = atomic_exchange(&completion, 0);
    if (!result) return;
    pending = false;
    ESP_LOGI("text_entry", "completed=%d", result > 0);
    lv_obj_remove_state(send, LV_STATE_DISABLED);
    lv_obj_remove_state(cancel, LV_STATE_DISABLED);
    lv_obj_remove_state(input, LV_STATE_DISABLED);
    if (keyboard) lv_obj_remove_state(keyboard, LV_STATE_DISABLED);
    lv_obj_remove_state(language, LV_STATE_DISABLED);
    if (result > 0) {
        lv_textarea_set_text(input, "");
        reset_composition();
        text_entry_error("");
        return;
    }
    text_entry_error("提交失败，文字已保留，请重试");
}

static void candidate_event(lv_event_t *event)
{
    (void)event;
    choose_candidate(lv_buttonmatrix_get_selected_button(candidates));
}

static void change_page(bool forward)
{
    if (lookup_busy || pending || choosing) return;
    if (!forward) {
        if (page_offset < P4_PINYIN_PAGE) return;
        page_offset -= P4_PINYIN_PAGE;
    } else {
        if (page_offset + P4_PINYIN_PAGE >= candidate_total) return;
        page_offset += P4_PINYIN_PAGE;
    }
    lookup(-1);
}

static void page_event(lv_event_t *event)
{
    change_page(lv_event_get_target(event) != previous);
}

void text_entry_key(uint32_t key)
{
    sync_dock();
    atomic_fetch_add(&health_keys, 1);
    if (!text_entry_active() || !owner->enabled || pending) return;
    if (!expanded && key != LV_KEY_ESC) text_entry_focus(false);
    if (choosing) {
        /* Preserve fast typing across the asynchronous candidate commit. */
        if (deferred_count < 32) deferred_keys[deferred_count++] = key;
        else text_entry_error("输入过快，请稍候");
        return;
    }
    lv_display_trigger_activity(NULL);
    if (key == LV_KEY_ESC) {
        if (spelling[0]) { reset_composition(); text_entry_error(""); }
        else close_editor(NULL);
        return;
    }
    if (key == LV_KEY_ENTER) { submit(NULL); return; }
    if (spelling[0] && (key == ' ' || (key >= '1' && key <= '6'))) {
        choose_candidate(key == ' ' ? 0 : key - '1');
        return;
    }
    if (spelling[0] && (key == LV_KEY_LEFT || key == LV_KEY_UP ||
                       key == LV_KEY_RIGHT || key == LV_KEY_DOWN)) {
        change_page(key == LV_KEY_RIGHT || key == LV_KEY_DOWN);
        return;
    }
    if (key == LV_KEY_BACKSPACE && spelling[0]) {
        spelling[strlen(spelling) - 1] = 0;
        page_offset = 0;
        lookup(-1);
        return;
    }
    if (chinese() && ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') || key == '\'')) {
        size_t length = strlen(spelling);
        if (length == P4_PINYIN_MAX) { text_entry_error("本段拼音过长，请先选词"); return; }
        spelling[length] = (char)tolower((unsigned char)key);
        spelling[length + 1] = 0;
        page_offset = 0;
        lookup(-1);
        return;
    }
    if (spelling[0]) return; /* Do not mix punctuation into uncommitted Pinyin. */
    if (key == LV_KEY_BACKSPACE || key == LV_KEY_LEFT || key == LV_KEY_RIGHT ||
        key == LV_KEY_HOME || key == LV_KEY_END || key == LV_KEY_DEL) {
        lv_obj_send_event(input, LV_EVENT_KEY, &key);
    } else if (key >= 32 && key <= 126) {
        char text[2] = {(char)key, 0};
        lv_textarea_add_text(input, text);
    }
}

static void keyboard_event(lv_event_t *event)
{
    (void)event;
    const char *key = lv_buttonmatrix_get_button_text(keyboard,
                         lv_buttonmatrix_get_selected_button(keyboard));
    if (!key) return;
    if (!strcmp(key, "abc")) lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    else if (!strcmp(key, "ABC")) lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
    else if (!strcmp(key, "1#")) lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
    else if (!strcmp(key, LV_SYMBOL_BACKSPACE)) text_entry_key(LV_KEY_BACKSPACE);
    else if (!strcmp(key, LV_SYMBOL_OK)) submit(NULL);
    else if (!strcmp(key, LV_SYMBOL_CLOSE) || !strcmp(key, LV_SYMBOL_KEYBOARD)) close_editor(NULL);
    else if (!strcmp(key, LV_SYMBOL_NEW_LINE) || !strcmp(key, "Enter")) text_entry_key(LV_KEY_ENTER);
    else if (!strcmp(key, LV_SYMBOL_LEFT)) text_entry_key(LV_KEY_LEFT);
    else if (!strcmp(key, LV_SYMBOL_RIGHT)) text_entry_key(LV_KEY_RIGHT);
    else for (const unsigned char *p = (const unsigned char *)key; *p; ++p) text_entry_key(*p);
}

void text_entry_toggle_language(lv_event_t *event)
{
    (void)event;
    sync_dock();
    if (!text_entry_active() || !owner->enabled || pending || choosing) return;
    if (spelling[0]) {
        const uint32_t position = lv_textarea_get_cursor_pos(input);
        lv_textarea_set_cursor_pos(input, LV_TEXTAREA_CURSOR_LAST);
        const uint32_t length = lv_textarea_get_cursor_pos(input);
        lv_textarea_set_cursor_pos(input, position);
        if (length + strlen(spelling) > lv_textarea_get_max_length(input)) {
            text_entry_error("文字已满，请先删减后切换");
            return;
        }
        lv_textarea_add_text(input, spelling);
    }
    reset_composition();
    owner->language = !owner->language;
    refresh_language_label();
    report_font_probe();
    text_entry_error("");
}

static lv_obj_t *icon_button(const char *symbol, int x, int y, int size, lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(editor);
    lv_obj_set_size(button, size, size);
    lv_obj_set_pos(button, x, y);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    return button;
}

static void toggle_keyboard(lv_event_t *event)
{
    (void)event;
    if (!pending) {
        if (expanded && touch_keyboard) close_editor(NULL);
        else text_entry_focus(true);
    }
}

static void focus_input(lv_event_t *event)
{
    (void)event;
    text_entry_focus(false);
}

static void build_dock(dock_owner_t *target)
{
    lv_obj_t *parent = target->parent;
    const lv_font_t *font = target->font;
    owner = target;
    language = target->language_button;
    language_label = lv_obj_get_child(language, 0);
    refresh_language_label();
    report_font_probe();
    if (!health_timer) {
        const esp_timer_create_args_t args = {.callback = health_report, .name = "editor_health"};
        if (esp_timer_create(&args, &health_timer) == ESP_OK) {
            esp_timer_start_periodic(health_timer, 2000000);
            lv_display_t *display = lv_obj_get_display(parent);
            lv_display_add_event_cb(display, refresh_event, LV_EVENT_REFR_START, NULL);
            lv_display_add_event_cb(display, refresh_event, LV_EVENT_REFR_READY, NULL);
            lv_display_add_event_cb(display, refresh_event, LV_EVENT_RENDER_START, NULL);
            lv_display_add_event_cb(display, refresh_event, LV_EVENT_RENDER_READY, NULL);
            lv_display_add_event_cb(display, refresh_event, LV_EVENT_FLUSH_START, NULL);
            lv_display_add_event_cb(display, refresh_event, LV_EVENT_FLUSH_FINISH, NULL);
            lv_display_add_event_cb(display, refresh_event, LV_EVENT_FLUSH_WAIT_START, NULL);
            lv_display_add_event_cb(display, refresh_event, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
        }
    }
    atomic_store(&health_active, true);
    submit_text = target->submit;
    submit_context = target->context;
    atomic_store(&completion, 0);
    decoder_ready = pinyin_service_state() == PINYIN_READY;
#if CONFIG_P4_SIGN_LANGUAGE_MODE
    portrait_editor_cpu_copy(true);
#endif
    editor = lv_obj_create(parent);
    lv_obj_remove_style_all(editor);
    lv_obj_set_size(editor, UI_CONTENT_WIDTH, UI_INPUT_COLLAPSED);
    lv_obj_set_scrollable(editor, false);
    lv_obj_remove_flag(editor, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_bg_color(editor, lv_color_hex(0x10233F), 0);
    lv_obj_set_style_bg_opa(editor, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(editor, deleted, LV_EVENT_DELETE, NULL);
    cancel = icon_button(LV_SYMBOL_KEYBOARD, 0, 4, 52, toggle_keyboard);
    send = icon_button(LV_SYMBOL_UP, 500, 4, 52, submit);
    lv_obj_set_width(send, 60);
    input = lv_textarea_create(editor);
    lv_obj_set_size(input, 432, 56);
    lv_obj_set_pos(input, 60, 0);
    lv_obj_set_style_text_font(input, font, 0);
    lv_obj_set_style_pad_top(input, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(input, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(input, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(input, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(input, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_opa(input, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(input, lv_color_hex(0x526171), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_opa(input, LV_OPA_COVER, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(input, lv_color_black(), LV_PART_CURSOR);
    lv_textarea_set_text(input, target->draft);
    lv_textarea_set_placeholder_text(input, "输入文字");
    lv_obj_add_event_cb(input, focus_input, LV_EVENT_CLICKED, NULL);
    lv_obj_remove_flag(input, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_textarea_set_cursor_pos(input, LV_TEXTAREA_CURSOR_LAST);
    const uint32_t initial_length = lv_textarea_get_cursor_pos(input);
    lv_textarea_set_max_length(input, initial_length > 500 ? initial_length : 500);
    candidate_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(candidate_panel);
    lv_obj_set_size(candidate_panel, UI_CONTENT_WIDTH, UI_CANDIDATE_HEIGHT);
    lv_obj_set_scrollable(candidate_panel, false);
    lv_obj_set_style_bg_color(candidate_panel, lv_color_hex(0x10233F), 0);
    lv_obj_set_style_bg_opa(candidate_panel, LV_OPA_80, 0);
    status = lv_label_create(candidate_panel);
    lv_obj_set_size(status, 440, 48);
    lv_obj_set_pos(status, 0, 284);
    lv_obj_set_style_text_font(status, font, 0);
    lv_obj_set_style_text_color(status, lv_color_white(), 0);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    composition = lv_label_create(candidate_panel);
    lv_obj_set_size(composition, UI_CONTENT_WIDTH, 64);
    lv_obj_set_pos(composition, 0, 8);
    lv_obj_set_style_text_font(composition, font, 0);
    lv_obj_set_style_text_color(composition, lv_color_white(), 0);
    lv_label_set_long_mode(composition, LV_LABEL_LONG_WRAP);
    previous = icon_button(LV_SYMBOL_LEFT, 456, 284, 44, page_event);
    next = icon_button(LV_SYMBOL_RIGHT, 512, 284, 44, page_event);
    lv_obj_set_parent(previous, candidate_panel);
    lv_obj_set_parent(next, candidate_panel);
    candidates = lv_buttonmatrix_create(candidate_panel);
    lv_obj_set_size(candidates, UI_CONTENT_WIDTH, 204);
    lv_obj_set_pos(candidates, 0, 76);
    lv_obj_set_style_text_font(candidates, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(candidates, font, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(candidates, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(candidates, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(candidates, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(candidates, LV_OPA_80, LV_PART_ITEMS);
    lv_obj_set_style_text_opa(candidates, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(candidates, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(candidates, lv_color_black(), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(candidates, lv_color_black(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(candidates, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(candidates, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(candidates, 0, LV_PART_MAIN);
    for (unsigned i = 0; i < P4_PINYIN_PAGE; ++i) {
        strcpy(candidate_labels[i], " ");
    }
    candidate_map[0] = " ";
    candidate_map[1] = "";
    lv_buttonmatrix_set_map(candidates, candidate_map);
    lv_obj_add_event_cb(candidates, candidate_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *draw_objects[] = {editor, cancel, send, input, status,
                               composition, previous, next, candidates, keyboard};
    for (unsigned i = 0; i < sizeof(draw_objects) / sizeof(draw_objects[0]); ++i) {
        if (!draw_objects[i]) continue;
        lv_obj_add_event_cb(draw_objects[i], draw_event, LV_EVENT_DRAW_MAIN_BEGIN, (void *)(uintptr_t)(i + 1));
        lv_obj_add_event_cb(draw_objects[i], draw_event, LV_EVENT_DRAW_MAIN_END, (void *)(uintptr_t)(i + 1));
    }
    reset_composition();
    strcpy(spelling, target->spelling);
    expanded = touch_keyboard = false;
    layout_dock();
    text_entry_set_enabled(parent, target->enabled);
    if (spelling[0]) lookup(-1);
    else text_entry_error("");
    ESP_LOGI("text_entry", "opened=1");
}

static int layout_candidates(int top)
{
    const lv_font_t *font = owner->font;
    const char *footer = lv_label_get_text(status);
    lv_point_t size;
    int width = 200;
    /* Measure actual glyph advances, not UTF-8 byte counts or nominal font size. */
    for (unsigned i = 0; i < candidate_count + 2; ++i) {
        const char *text = i < candidate_count ? candidate_labels[i] :
                           i == candidate_count ? spelling : footer;
        lv_text_get_size(&size, text, font, 0, 4, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int needed = size.x + (i == candidate_count + 1 ? 128 : 24);
        if (needed > width) width = needed;
    }
    if (width > UI_CONTENT_WIDTH) width = UI_CONTENT_WIDTH;
    lv_text_get_size(&size, spelling, font, 0, 4, width - 16, LV_TEXT_FLAG_NONE);
    const int composition_height = spelling[0] ? size.y : 0;
    lv_text_get_size(&size, footer, font, 0, 4, width - 128, LV_TEXT_FLAG_NONE);
    const int footer_height = size.y > 48 ? size.y : 48;
    const int row_height = font->line_height + 8;
    const int list_top = 8 + composition_height + (composition_height ? 8 : 0);
    const int list_height = candidate_count * row_height;
    const int footer_top = list_top + list_height + 8;
    const int height = footer_top + footer_height + 8;
    unsigned map_index = 0;
    for (unsigned i = 0; i < candidate_count; ++i) {
        if (i) candidate_map[map_index++] = "\n";
        candidate_map[map_index++] = candidate_labels[i];
    }
    if (!map_index) candidate_map[map_index++] = " ";
    candidate_map[map_index] = "";
    lv_buttonmatrix_set_map(candidates, candidate_map);
    lv_obj_set_size(candidate_panel, width, height);
    lv_obj_set_pos(candidate_panel, UI_MARGIN, top - height - 8);
    lv_obj_set_size(composition, width - 16, composition_height ? composition_height : 1);
    lv_obj_set_pos(composition, 8, 8);
    lv_obj_set_size(candidates, width, list_height ? list_height : 1);
    lv_obj_set_pos(candidates, 0, list_top);
    lv_obj_set_size(status, width - 128, footer_height);
    lv_obj_set_pos(status, 8, footer_top);
    lv_obj_set_pos(previous, width - 112, footer_top);
    lv_obj_set_pos(next, width - 56, footer_top);
    return top - height - 8;
}

static void layout_dock(void)
{
    if (!editor || !owner) return;
    const int height = expanded && touch_keyboard ? UI_INPUT_TOUCH : UI_INPUT_COLLAPSED;
    const int limit = UI_HEIGHT - UI_MARGIN - height;
    const int top = owner->top < limit ? owner->top : limit;
    const bool popup = expanded && (spelling[0] || status_visible);
    lv_obj_set_size(editor, UI_CONTENT_WIDTH, height);
    lv_obj_set_pos(editor, UI_MARGIN, top);
    /* Physical IME floats above the fixed composer, without moving the history. */
    const int popup_top = layout_candidates(top);
    lv_obj_set_hidden(candidate_panel, !popup);
    if (keyboard) lv_obj_set_hidden(keyboard, !expanded || !touch_keyboard);
    lv_obj_set_hidden(composition, !expanded);
    lv_obj_set_hidden(candidates, !expanded || !candidate_count);
    lv_obj_set_hidden(previous, !expanded || !candidate_count);
    lv_obj_set_hidden(next, !expanded || !candidate_count);
#if CONFIG_P4_SIGN_LANGUAGE_MODE
    /* Keep erased, previously larger popups on the safe CPU-copy path too. */
    if (popup_top < cpu_region_top) cpu_region_top = popup_top;
    portrait_editor_cpu_region(cpu_region_top);
#endif
    if (owner->layout) owner->layout(height, owner->layout_context);
}

static void sync_dock(void)
{
    dock_owner_t *target = NULL;
    for (unsigned i = 0; i < 2; ++i)
        if (owners[i].parent == lv_screen_active()) target = &owners[i];
    if (target == owner || pending) return;
    if (owner) text_entry_release(owner->parent);
    if (target) build_dock(target);
}

void text_entry_release(lv_obj_t *parent)
{
    if (!editor || !owner || owner->parent != parent || pending) return;
    snprintf(owner->draft, sizeof(owner->draft), "%s", lv_textarea_get_text(input));
    strcpy(owner->spelling, spelling);
    lv_obj_delete(editor);
}

static void owner_deleted(lv_event_t *event)
{
    dock_owner_t *target = lv_event_get_user_data(event);
    memset(target, 0, sizeof(*target));
}

void text_entry_mount(lv_obj_t *parent, const lv_font_t *font,
                      text_entry_submit_fn callback, void *context,
                      text_entry_layout_fn layout, void *layout_context,
                      int top, lv_obj_t *language_button)
{
    if (!parent || !font || !callback || !language_button) return;
    static bool timer_created;
    if (!timer_created) { lv_timer_create(poll_completion, 30, NULL); timer_created = true; }
    dock_owner_t *target = NULL;
    for (unsigned i = 0; i < 2; ++i)
        if (owners[i].parent == parent) target = &owners[i];
    if (!target) {
        for (unsigned i = 0; i < 2; ++i)
            if (!owners[i].parent) { target = &owners[i]; break; }
        if (!target) return;
        memset(target, 0, sizeof(*target));
        target->parent = parent;
        target->enabled = true;
        lv_obj_add_event_cb(parent, owner_deleted, LV_EVENT_DELETE, target);
    }
    target->font = font;
    target->submit = callback;
    target->context = context;
    target->layout = layout;
    target->layout_context = layout_context;
    target->top = top;
    target->language_button = language_button;
    sync_dock();
}

void text_entry_focus(bool touch)
{
    sync_dock();
    if (!text_entry_active() || !owner->enabled || pending) return;
    if (expanded && (touch_keyboard || !touch)) return;
    /* A USB-only session need not allocate an unused on-screen keyboard. */
    if (touch && !keyboard) {
        keyboard = lv_keyboard_create(editor);
        lv_obj_set_size(keyboard, UI_CONTENT_WIDTH, 224);
        lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(keyboard, 0, 76);
        lv_obj_set_style_text_font(keyboard, LV_FONT_DEFAULT, LV_PART_ITEMS);
        lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
        lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_VALUE_CHANGED, NULL);
    }
    expanded = true;
    touch_keyboard = touch;
    layout_dock();
    ESP_LOGI("text_entry", "dock_height=%d touch=%d", touch ? UI_INPUT_TOUCH : UI_INPUT_COLLAPSED, touch);
}

void text_entry_set_enabled(lv_obj_t *parent, bool enabled)
{
    for (unsigned i = 0; i < 2; ++i)
        if (owners[i].parent == parent) owners[i].enabled = enabled;
    if (!editor || !owner || owner->parent != parent) return;
    lv_obj_t *controls[] = {input, keyboard, send, cancel, language, candidates};
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i)
        if (controls[i]) lv_obj_set_state(controls[i], LV_STATE_DISABLED, !enabled || pending);
}

void text_entry_set_font(lv_obj_t *parent, const lv_font_t *font)
{
    if (!font) return;
    for (unsigned i = 0; i < 2; ++i)
        if (owners[i].parent == parent) owners[i].font = font;
    if (!editor || !owner || owner->parent != parent) return;
    lv_obj_t *labels[] = {input, language_label, status, composition};
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
        lv_obj_set_style_text_font(labels[i], font, LV_PART_MAIN);
    lv_obj_set_style_text_font(candidates, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(candidates, font, LV_PART_ITEMS);
    refresh_language_label();
}
