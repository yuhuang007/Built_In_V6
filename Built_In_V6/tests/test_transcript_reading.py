"""Exercise production transcript scrolling and clear callbacks with bounded UI doubles.

The double models synchronous layout clamping/scroll events, not LVGL rendering.
Fixtures are synthetic; no serial, ASR, stored history or cloud access.
"""
import ctypes
import _ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_portrait_layout import function

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/p4_main/main"
TCC = ROOT / "logs/chat-host-tools/tcc/tcc.exe"


class TranscriptReadingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TCC.exists():
            raise unittest.SkipTest("Host TCC unavailable")
        cls.temp = tempfile.TemporaryDirectory(dir=ROOT / "logs")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        source = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "ui_layout.h"
#include "transcript_turns.c"
#define QH_SOFT 0xE2E9D4
#define QH_DANGER 0x9E5148
#define QH_SURFACE 0xFFFEF8
#define QH_INK 0x314633
#define QH_ON_ACCENT 0xFAFFF6
#define LV_COORD_MAX 1000000
#define LV_ANIM_OFF 0
#define LV_TEXT_FLAG_NONE 0
#define LV_ALIGN_TOP_MID 1
#define LV_ALIGN_BOTTOM_MID 2
#define LV_LABEL_LONG_WRAP 0
#define LV_PART_MAIN 0
#define LV_STATE_PRESSED 1
#define LV_OPA_50 128
#define LV_OPA_20 51
#define LV_OPA_COVER 255
#define LV_EVENT_CLICKED 1
#define LV_EVENT_RELEASED 2
#define LV_EVENT_PRESS_LOST 3
#define LV_EVENT_PRESSED 4
#define PRESS_BIT 1
#define TRANSCRIPT_CAPACITY 32768
#define atomic_load(p) (*(p))
#define atomic_store(p,v) (*(p)=(v))
#define ESP_LOGI(...) ((void)0)
typedef struct { int code; } lv_event_t;
typedef int lv_event_code_t;
typedef void (*lv_event_cb_t)(lv_event_t *);
typedef struct obj {
    int x,y,w,h,scroll,kind; bool hidden,deleted;const char *text;
    struct obj *parent;
    lv_event_cb_t callback;
} lv_obj_t;
typedef struct { int line_height; } lv_font_t;
typedef struct { int x,y; } lv_point_t;
static lv_obj_t pool[64];
static int allocated,next_height,presses,completed;
static bool busy,return_requested,release_requested,entry_enabled;
static bool follow_latest,updating_transcript_view;
static lv_font_t font={54};
static lv_font_t *transcript_font=&font;
static lv_obj_t *text_screen,*status_label,*transcript_label,*transcript_content;
static lv_obj_t *transcript_viewport,*latest_button,*latest_label,*clear_dialog,*empty_state;
static char storage[TRANSCRIPT_CAPACITY],*transcript_storage=storage;
static transcript_turns_t transcript;
static int events;
static void transcript_scroll_event(lv_event_t *event);
static int limit(void) {
    int n=transcript_content->h-transcript_viewport->h;return n>0?n:0;
}
static int bound(int n) {return n<0?0:n>limit()?limit():n;}
static int lv_obj_get_scroll_y(lv_obj_t *o) {return o->scroll;}
static int lv_obj_get_scroll_bottom(lv_obj_t *o) {return limit()-o->scroll;}
static void lv_obj_set_hidden(lv_obj_t *o,bool hidden) {o->hidden=hidden;}
static void lv_obj_set_height(lv_obj_t *o,int h) {o->h=h;}
static int lv_obj_get_height(lv_obj_t *o) {return o->h;}
static void lv_obj_update_layout(lv_obj_t *o) {
    int y=bound(transcript_viewport->scroll);
    if(y!=transcript_viewport->scroll) {
        transcript_viewport->scroll=y;transcript_scroll_event(0);
    }
}
static void lv_obj_scroll_to_y(lv_obj_t *o,int y,int anim) {
    o->scroll=bound(y);transcript_scroll_event(0);
}
static void lv_label_set_text(lv_obj_t *o,const char *text) {
    o->text=text;
    if(o==transcript_label) o->h=*text?next_height:0;
}
static const char *lv_label_get_text(lv_obj_t *o){return o->text?o->text:"";}
static lv_obj_t *lv_obj_create(lv_obj_t *parent) {
    lv_obj_t *o=&pool[allocated++];memset(o,0,sizeof(*o));o->parent=parent;return o;
}
static lv_obj_t *lv_button_create(lv_obj_t *parent) {
    lv_obj_t *o=lv_obj_create(parent);o->kind=1;return o;
}
static lv_obj_t *lv_label_create(lv_obj_t *parent) {return lv_obj_create(parent);}
static void lv_obj_remove_style_all(lv_obj_t *o) {}
static void lv_obj_set_size(lv_obj_t *o,int w,int h) {o->w=w;o->h=h;}
static void lv_obj_set_width(lv_obj_t *o,int w) {o->w=w;}
static void lv_obj_set_scrollable(lv_obj_t *o,bool b) {}
static int lv_color_black(void) {return 0;}
static int lv_color_white(void) {return 0xffffff;}
static int qh_color(int c) {return c;}
static int lv_color_darken(int c,int opa) {return c;}
static void lv_obj_set_style_bg_color(lv_obj_t *o,int c,int p) {}
static void lv_obj_set_style_bg_opa(lv_obj_t *o,int c,int p) {}
static void lv_obj_set_style_radius(lv_obj_t *o,int c,int p) {}
static void lv_obj_set_style_pad_all(lv_obj_t *o,int c,int p) {}
static void lv_obj_set_style_text_color(lv_obj_t *o,int c,int p) {}
static void lv_obj_set_style_text_font(lv_obj_t *o,const lv_font_t *f,int p) {}
static void lv_obj_set_style_text_line_space(lv_obj_t *o,int c,int p) {}
static void lv_label_set_long_mode(lv_obj_t *o,int m) {}
static void lv_text_get_size(lv_point_t *s,const char *t,const lv_font_t *f,
                            int letter,int line,int width,int flags) {
    s->x=9*28;s->y=2*f->line_height+line;
}
static void lv_obj_center(lv_obj_t *o) {
    o->x=(o->parent->w-o->w)/2;o->y=(o->parent->h-o->h)/2;
}
static void lv_obj_align(lv_obj_t *o,int align,int x,int y) {
    o->x=(o->parent->w-o->w)/2+x;
    o->y=(align==LV_ALIGN_BOTTOM_MID?o->parent->h-o->h:0)+y;
}
static void lv_obj_add_event_cb(lv_obj_t *o,lv_event_cb_t cb,int code,void *data) {o->callback=cb;}
static void lv_obj_delete(lv_obj_t *o) {o->deleted=true;}
static void text_entry_set_enabled(lv_obj_t *screen,bool enabled) {entry_enabled=enabled;}
static void qh_reveal(lv_obj_t *o) {}
static void text_entry_error(const char *s) {}
static void text_entry_finish(bool success) {completed+=success;}
static int lv_event_get_code(lv_event_t *e) {return e->code;}
static bool atomic_compare_exchange_strong(bool *p,bool *expected,bool desired) {
    if(*p!=*expected) {*expected=*p;return false;}*p=desired;return true;
}
static void xEventGroupSetBits(int e,int bits) {presses++;}
'''
        for signature in ["static void update_latest_button(",
                          "static void transcript_scroll_event(",
                          "static void update_transcript_view(",
                          "static void resize_transcript(", "static void set_transcript(",
                          "static void latest_event(", "static void hold_event(",
                          "static bool accept_edited_text(", "static void clear_cancel(",
                          "static void clear_confirm(", "static void clear_event("]:
            harness += function(source, signature) + "\n"
        harness += r'''
__declspec(dllexport) void reset(void) {
    allocated=0;memset(pool,0,sizeof(pool));
    text_screen=lv_obj_create(0);lv_obj_set_size(text_screen,600,1024);
    status_label=lv_obj_create(text_screen);
    transcript_viewport=lv_obj_create(text_screen);transcript_viewport->h=668;
    transcript_content=lv_obj_create(transcript_viewport);transcript_content->h=668;
    transcript_label=lv_obj_create(transcript_content);
    latest_button=lv_obj_create(text_screen);latest_label=lv_obj_create(latest_button);
    empty_state=lv_obj_create(transcript_content);
    clear_dialog=0;busy=return_requested=updating_transcript_view=false;
    follow_latest=entry_enabled=true;presses=completed=0;next_height=0;
    transcript_turns_init(&transcript,storage,sizeof(storage));set_transcript("");
}
__declspec(dllexport) void change(int height) {next_height=height;set_transcript("synthetic");}
__declspec(dllexport) void scroll(int y) {transcript_viewport->scroll=bound(y);transcript_scroll_event(0);}
__declspec(dllexport) void reflow(int height) {transcript_viewport->h=height;resize_transcript();}
__declspec(dllexport) void latest(void) {latest_event(0);}
__declspec(dllexport) int position(void) {return transcript_viewport->scroll;}
__declspec(dllexport) int following(void) {return follow_latest;}
__declspec(dllexport) int empty_hidden(void){return empty_state->hidden;}
__declspec(dllexport) int latest_hidden(void) {return latest_button->hidden;}
__declspec(dllexport) void open_clear(void) {clear_event(0);}
__declspec(dllexport) void set_busy(int b) {busy=b;}
__declspec(dllexport) void set_return(int b) {return_requested=b;}
__declspec(dllexport) int dialog_open(void) {return clear_dialog!=0;}
__declspec(dllexport) int enabled(void) {return entry_enabled;}
__declspec(dllexport) int accept(const char *text) {return accept_edited_text(text,0);}
__declspec(dllexport) const char *text(void) {return transcript.text;}
__declspec(dllexport) void press_record(void) {lv_event_t e={LV_EVENT_PRESSED};hold_event(&e);}
__declspec(dllexport) int recording_requests(void) {return presses;}
__declspec(dllexport) void action(int index) {
    int count=0;
    for(int i=0;i<allocated;i++) {
        lv_obj_t *o=&pool[i];
        if(o->callback && o->parent && o->parent->parent==clear_dialog && count++==index) {
            o->callback(0);return;
        }
    }
}
__declspec(dllexport) int dialog_bounds(void) {
    int actions=0;
    for(int i=0;i<allocated;i++) {
        lv_obj_t *o=&pool[i];
        if(!o->parent || o->parent->parent!=clear_dialog || !o->callback) continue;
        if(o->h<font.line_height || o->y<20+2*font.line_height+4+20 ||
           o->x<0 || o->x+o->w>o->parent->w || o->y+o->h>o->parent->h) return 0;
        actions++;
    }
    return actions==2;
}
'''
        path = work / "reading.c"
        path.write_text(harness, encoding="utf-8")
        dll = work / "reading.dll"
        result = subprocess.run([str(TCC), "-shared", "-I" + str(MAIN),
                                 str(path), "-o", str(dll)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        cls.lib = ctypes.CDLL(str(dll))
        cls.addClassCleanup(_ctypes.FreeLibrary, cls.lib._handle)
        cls.lib.accept.argtypes = [ctypes.c_char_p]
        cls.lib.text.restype = ctypes.c_char_p

    def setUp(self):
        self.lib.reset()

    def test_empty_guidance_hides_on_real_text(self):
        self.lib.reset()
        self.assertEqual(self.lib.empty_hidden(),0)
        self.lib.change(200)
        self.assertEqual(self.lib.empty_hidden(),1)

    def test_bottom_tracks_growing_and_shortened_hypotheses(self):
        for height in [1500, 2200, 1800, 300, 1600]:
            self.lib.change(height)
            self.assertEqual(self.lib.position(), max(0, height - 668))
            self.assertEqual(self.lib.following(), 1)
            self.assertEqual(self.lib.latest_hidden(), 1)

    def test_reading_offset_survives_updates_and_layout_clamping(self):
        self.lib.change(1800)
        self.lib.scroll(400)
        for height in [2600, 1500, 2100]:
            self.lib.change(height)
            self.assertEqual(self.lib.position(), 400)
            self.assertEqual(self.lib.latest_hidden(), 0)
        self.lib.change(800)  # Original reading offset no longer exists.
        self.assertEqual(self.lib.position(), 132)
        self.assertEqual(self.lib.following(), 0)
        self.lib.change(2000)  # Shrink did not silently re-enable following.
        self.assertEqual(self.lib.position(), 132)
        self.assertEqual(self.lib.latest_hidden(), 0)
        self.lib.latest()
        self.assertEqual(self.lib.position(), 1332)
        self.lib.change(2300)
        self.assertEqual(self.lib.position(), 1632)
        self.assertEqual(self.lib.latest_hidden(), 1)

    def test_keyboard_reflow_preserves_reader_or_tracks_bottom(self):
        self.lib.change(2000)
        self.lib.reflow(532)
        self.assertEqual(self.lib.position(), 1468)
        self.lib.scroll(300)
        self.lib.reflow(668)
        self.assertEqual(self.lib.position(), 300)
        self.lib.scroll(1332)  # Manual return to bottom also resumes following.
        self.lib.change(2300)
        self.assertEqual(self.lib.position(), 1632)

    def test_cancel_keeps_text_position_and_blocks_background_actions(self):
        self.lib.change(2000)
        self.assertTrue(self.lib.accept(b"previous"))
        self.lib.scroll(300)
        self.lib.open_clear()
        self.assertTrue(self.lib.dialog_open())
        self.assertTrue(self.lib.dialog_bounds())
        self.assertFalse(self.lib.enabled())
        self.lib.press_record()
        self.assertEqual(self.lib.recording_requests(), 0)
        self.assertFalse(self.lib.accept(b"blocked"))
        self.lib.action(0)
        self.assertFalse(self.lib.dialog_open())
        self.assertTrue(self.lib.enabled())
        self.assertEqual(self.lib.text(), b"previous")
        self.assertEqual(self.lib.position(), 300)
        self.assertFalse(self.lib.following())

    def test_confirm_resets_model_and_allows_more_input_and_recording(self):
        self.lib.change(2000)
        self.lib.accept(b"previous")
        self.lib.scroll(300)
        self.lib.open_clear()
        self.lib.action(1)
        self.assertFalse(self.lib.dialog_open())
        self.assertEqual(self.lib.text(), b"")
        self.assertEqual(self.lib.position(), 0)
        self.assertTrue(self.lib.following())
        self.assertTrue(self.lib.enabled())
        self.assertTrue(self.lib.accept(b"after clear"))
        self.assertEqual(self.lib.text(), b"after clear")
        self.lib.press_record()
        self.assertEqual(self.lib.recording_requests(), 1)

    def test_busy_or_returning_cannot_open_or_confirm_clear(self):
        self.lib.accept(b"preserved")
        for setter in [self.lib.set_busy, self.lib.set_return]:
            setter(1)
            self.lib.open_clear()
            self.assertFalse(self.lib.dialog_open())
            setter(0)
            self.lib.open_clear()
            setter(1)
            self.lib.action(1)
            self.assertEqual(self.lib.text(), b"preserved")
            self.assertFalse(self.lib.enabled())
            setter(0)


if __name__ == "__main__":
    unittest.main()
