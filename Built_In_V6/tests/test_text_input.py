"""Offline production RTC text framing and input routing checks; no services."""
import ctypes
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/p4_main/main"
TCC = ROOT / "logs/chat-host-tools/tcc/tcc.exe"
SDK = ROOT.parent / "ConversationalAI-Embedded-Kit-2.0-main_Korvo_2/volc_conv_ai/inc"
CJSON = Path("E:/XUEXI/ESP_IDF_5.5.4/v5.5.4/esp-idf/components/json/cJSON")


class TextCommandTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TCC.exists() or not (CJSON / "cJSON.c").exists():
            raise unittest.SkipTest("Optional host compiler/cJSON unavailable")
        cls.temp = tempfile.TemporaryDirectory(dir=ROOT / "logs")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        stub = work / "stub.c"
        stub.write_text(r'''
#include <stdlib.h>
#include <string.h>
#include "cloud_text.h"
#include "cJSON.h"
static char captured[8192];
static int size, calls, binary, sdk_result, allocations, fail_at, live;
static void *allocate(size_t n) {
    if (++allocations == fail_at) return NULL;
    void *p = malloc(n); if (p) live++; return p;
}
static void release(void *p) { if (p) live--; free(p); }
int volc_send_message(volc_engine_t e, const void *p, size_t n, volc_message_info_t *info) {
    calls++; size = n; binary = info->is_binary;
    memcpy(captured, p, n); return sdk_result;
}
__declspec(dllexport) int attempt(const char *text, int result, int failure) {
    cJSON_Hooks hooks = {allocate, release}; cJSON_InitHooks(&hooks);
    size = calls = binary = allocations = 0; fail_at = failure; sdk_result = result;
    return cloud_text_send((volc_engine_t)1, text);
}
__declspec(dllexport) char *packet(void) { return captured; }
__declspec(dllexport) int packet_size(void) { return size; }
__declspec(dllexport) int send_calls(void) { return calls; }
__declspec(dllexport) int binary_packet(void) { return binary; }
__declspec(dllexport) int live_allocations(void) { return live; }
''', encoding="utf-8")
        dll = work / "text.dll"
        subprocess.run([str(TCC), "-shared", "-o", str(dll), "-I" + str(MAIN),
                        "-I" + str(SDK), "-I" + str(CJSON), str(stub),
                        str(MAIN / "cloud_text.c"), str(CJSON / "cJSON.c")], check=True,
                       capture_output=True)
        cls.lib = ctypes.CDLL(str(dll))
        from _ctypes import FreeLibrary
        cls.addClassCleanup(FreeLibrary, cls.lib._handle)
        cls.lib.attempt.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        cls.lib.packet.restype = ctypes.c_void_p

    def test_utf8_escaping_and_vendor_binary_framing(self):
        text = '你好 "world"\n\\test'
        self.assertEqual(self.lib.attempt(text.encode(), 0, 0), 0)
        packet = ctypes.string_at(self.lib.packet(), self.lib.packet_size())
        self.assertEqual(packet[:4], b"ctrl")
        self.assertEqual(struct.unpack(">I", packet[4:8])[0], len(packet) - 8)
        self.assertEqual(json.loads(packet[8:]), {
            "Command": "ExternalTextToLLM", "Message": text, "InterruptMode": 2})
        self.assertEqual(self.lib.binary_packet(), 1)
        self.assertEqual(self.lib.live_allocations(), 0)

    def test_empty_and_overlong_inputs_never_send(self):
        for text in [None, b"", b"x" * 1501]:
            self.assertEqual(self.lib.attempt(text, 0, 0), -1)
            self.assertEqual(self.lib.send_calls(), 0)
        self.assertEqual(self.lib.attempt(b"x" * 1500, 0, 0), 0)

    def test_transport_error_and_all_json_allocation_failures_free_memory(self):
        self.assertEqual(self.lib.attempt(b"test", -9, 0), -9)
        self.assertEqual(self.lib.live_allocations(), 0)
        for fail in range(1, 20):
            self.lib.attempt(b"test", 0, fail)
            self.assertEqual(self.lib.live_allocations(), 0, fail)


class InputRoutingTests(unittest.TestCase):
    def test_input_controls_bind_chinese_and_symbol_fonts_separately(self):
        source = (MAIN / "text_entry.c").read_text(encoding="utf-8")
        self.assertIn("lv_obj_set_style_text_font(language_label, owner->font, 0)", source)
        self.assertIn('lv_label_set_text(language_label, chinese() ? "拼" : "E")', source)
        self.assertNotIn("lv_dropdown_", source)
        self.assertIn('qh_label(candidate_buttons[i], font, "", QH_INK)', source)
        self.assertIn("lv_obj_set_style_text_font(candidate_captions[i], font, 0)", source)
        icon_start = source.index("static lv_obj_t *icon_button(")
        icons = source[icon_start:source.index("static void build_dock(", icon_start)]
        self.assertIn("LV_FONT_DEFAULT", icons)
        home = (MAIN / "main.c").read_text(encoding="utf-8")
        self.assertIn("object != typing_icon_label", home)
        self.assertIn("typing_icon_label = typing_icon", home)

    def test_transcript_edit_is_local_and_gated(self):
        source = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        start = source.index("static bool accept_edited_text(")
        body = source[start:source.index("static void clear_event(", start)]
        self.assertIn("atomic_load(&busy)", body)
        self.assertIn("atomic_load(&return_requested)", body)
        self.assertIn("transcript_turns_append(&transcript, text)", body)
        self.assertIn("set_transcript(transcript.text)", body)
        self.assertNotIn("volc_", body)
        self.assertNotIn("doubao_asr_open", body)

    def test_home_transport_is_worker_only_and_keeps_draft_on_failure(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        self.assertEqual(source.count("cloud_text_send("), 1)
        worker = source[source.index("static void typed_message_task("):
                        source.index("static bool submit_home_text(")]
        self.assertIn("xSemaphoreTake(cloud_engine_lock", worker)
        self.assertIn("text_entry_finish(result == VOLC_ERR_NO_ERROR)", worker)
        self.assertIn("memset(typed_message, 0", worker)
        self.assertNotIn('"%s"', worker)

    def test_pinyin_worker_and_physical_key_routing(self):
        source = (MAIN / "text_entry.c").read_text(encoding="utf-8")
        self.assertNotIn("lv_ime_pinyin", source)
        self.assertIn("response.revision == revision", source)
        self.assertIn("pinyin_service_request(++revision", source)
        self.assertIn("lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb)", source)
        self.assertIn("initial_length > 500 ? initial_length : 500", source)
        cleanup = source[source.index("static void deleted("):source.index("static void close_editor(")]
        self.assertIn("++revision", cleanup)
        self.assertIn("memset(spelling, 0", cleanup)
        usb = (MAIN / "usb_keyboard.c").read_text(encoding="utf-8")
        self.assertIn("lvgl_port_add_usb_hid_keyboard_input", usb)
        self.assertIn("LV_INDEV_STATE_PRESSED", usb)
        self.assertIn('esp_log_level_set("LVGL", ESP_LOG_WARN)', usb)
        idle = (MAIN / "main.c").read_text(encoding="utf-8")
        self.assertIn("if (screen_idle_keyboard_activity()) return;", idle)

    def test_inline_dock_truncates_visible_pinyin_and_preserves_full_draft(self):
        source = (MAIN / "text_entry.c").read_text(encoding="utf-8")
        self.assertIn("lv_label_set_long_mode(composition, LV_LABEL_LONG_DOT)", source)
        self.assertNotIn("LV_LABEL_LONG_SCROLL", source)
        self.assertNotIn("lv_screen_load", source)
        self.assertIn("expanded && touch_keyboard ? UI_INPUT_TOUCH : UI_INPUT_COLLAPSED", source)
        self.assertIn("owner->draft", source)
        self.assertIn("strcpy(owner->spelling, spelling)", source)
        self.assertIn("owner->parent == lv_screen_active()", source)
        self.assertIn("if (text_entry_is_root(object)) return", (MAIN / "main.c").read_text(encoding="utf-8"))

    def test_physical_input_keeps_layout_and_only_icon_opens_touch_keyboard(self):
        source = (MAIN / "text_entry.c").read_text(encoding="utf-8")
        focus = source[source.index("static void focus_input("):source.index("static void build_dock(dock_owner_t *target)\n{")]
        self.assertIn("text_entry_focus(false)", focus)
        self.assertNotIn("text_entry_focus(true)", focus)
        self.assertIn("lv_obj_remove_flag(input, LV_OBJ_FLAG_SCROLL_ON_FOCUS)", source)
        self.assertIn("candidate_panel = lv_obj_create(parent)", source)
        self.assertIn("lv_obj_set_pos(candidate_panel, UI_MARGIN, top - height - 8)", source)
        self.assertIn("portrait_editor_cpu_region(cpu_region_top)", source)
        self.assertIn("qh_panel(candidate_panel, QH_SOFT, 20)", source)
        toggle = source[source.index("static void toggle_keyboard("):source.index("static void focus_input(")]
        self.assertIn("text_entry_focus(true)", toggle)
        for name in ["main.c", "speech_to_text_mode.c"]:
            self.assertNotIn("text_entry_focus(true)", (MAIN / name).read_text(encoding="utf-8"))

    def test_compact_composer_and_mode_controls_have_explicit_anchors(self):
        source = (MAIN / "text_entry.c").read_text(encoding="utf-8")
        self.assertIn("lv_obj_set_size(input, 432, UI_INPUT_COLLAPSED)", source)
        self.assertIn("target->top = top", source)
        self.assertIn("target->language_button = language_button", source)
        home = (MAIN / "main.c").read_text(encoding="utf-8")
        layout = home[home.index("static void home_input_layout("):home.index("static void physical_keyboard_key(")]
        self.assertNotIn("lv_obj_set_hidden", layout)
        self.assertIn("top - 94 - UI_CHAT_TOP", layout)
        self.assertIn("text_entry_toggle_language(event)", layout)
        asr = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        for name in ["clear", "hold"]:
            self.assertLess(asr.index(f"lv_obj_set_align({name}, LV_ALIGN_TOP_LEFT)"),
                            asr.index(f"lv_obj_set_pos({name},"))
        layout = asr[asr.index("static void input_layout("):asr.index("static void clear_event(")]
        self.assertNotIn("lv_obj_set_hidden", layout)
        self.assertIn("text_entry_toggle_language, LV_EVENT_CLICKED", asr)

    def test_visible_input_has_explicit_contrast_and_shared_cached_font(self):
        source = (MAIN / "text_entry.c").read_text(encoding="utf-8")
        self.assertIn("qh_panel(input, QH_SURFACE, 18)", source)
        self.assertIn("lv_obj_set_style_text_color(input, qh_color(QH_INK), LV_PART_MAIN)", source)
        self.assertIn("lv_obj_set_style_text_opa(input, LV_OPA_COVER, LV_PART_MAIN)", source)
        self.assertIn("lv_obj_set_style_text_color(language_label, qh_color(QH_INK), 0)", source)
        self.assertIn("lv_obj_set_size(language_label, 52, owner->font->line_height)", source)
        self.assertIn("lv_obj_set_style_pad_top(input, 0, LV_PART_MAIN)", source)
        self.assertIn("lv_obj_set_style_pad_bottom(input, 0, LV_PART_MAIN)", source)
        self.assertIn("qh_button(candidate_buttons[i], QH_SURFACE)", source)
        home = (MAIN / "main.c").read_text(encoding="utf-8")
        apply = home[home.index("static void apply_subtitle_font("):home.index("static void refresh_ui_font_from_sdcard(")]
        self.assertIn("const lv_font_t *title_font = speech_to_text_mode_cached_font28()", apply)
        self.assertIn("text_entry_set_font(home_ui_screen, title_font)", apply)
        self.assertIn("input_font = speech_to_text_mode_cached_font28()", home)
        toggle = source[source.index("void text_entry_toggle_language("):source.index("static lv_obj_t *icon_button(")]
        self.assertIn("refresh_language_label()", toggle)

    def test_transcript_clear_stays_beside_recording_and_lines_are_compact(self):
        source = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        self.assertIn("lv_obj_set_pos(clear, UI_WIDTH - UI_MARGIN - 132, UI_HOLD_TOP)", source)
        self.assertIn("UI_CONTENT_WIDTH - 152", source)
        self.assertIn("lv_obj_set_style_text_line_space(transcript_label, 4, 0)", source)

    def test_local_transcript_entry_does_not_depend_on_network_or_rtc_registration(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        entry = source[source.index("static void speech_to_text_button_callback("):]
        gate = entry[entry.index("if (microphone == NULL"):entry.index("const lv_font_t *font")]
        for guard in ["conversation_recording", "conversation_finish_requested", "local_loop_active", "font_sync_in_progress"]:
            self.assertIn(guard, gate)
        for network in ["wifi_link_up", "cloud_start_requested", "cloud_recovery_in_progress"]:
            self.assertNotIn(network, gate)
        prepare = source[source.index("static esp_err_t prepare_speech_to_text_recording("):source.index("static void speech_to_text_button_callback(")]
        for guard in ["!wifi_link_up", "cloud_start_requested", "cloud_recovery_in_progress", "prepare_sign_language_mode()"]:
            self.assertIn(guard, prepare)
        mode = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        worker = mode[mode.index("static void mode_task("):mode.index("static void hold_event(")]
        self.assertNotIn("if (prepared == ESP_OK", worker)
        self.assertIn("const esp_err_t ready = prepare_voice()", worker)
        self.assertIn("ready == ESP_OK && !atomic_load(&return_requested)", worker)


if __name__ == "__main__":
    unittest.main()
