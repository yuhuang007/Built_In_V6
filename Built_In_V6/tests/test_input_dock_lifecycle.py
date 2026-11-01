"""Execute production dock functions with an allocation/geometry LVGL double."""
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


class DockLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TCC.exists():
            raise unittest.SkipTest("Host TCC unavailable")
        cls.work = tempfile.TemporaryDirectory(dir=ROOT / "logs")
        cls.addClassCleanup(cls.work.cleanup)
        work = Path(cls.work.name)
        source = (MAIN / "text_entry.c").read_text(encoding="utf-8")
        stub = r'''
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "ui_layout.h"
#define CONFIG_P4_SIGN_LANGUAGE_MODE 1
#define LV_ALIGN_TOP_LEFT 0
#define LV_ALIGN_BOTTOM_MID 1
#define LV_FONT_DEFAULT 0
#define LV_PART_ITEMS 0
#define LV_COORD_MAX 1000000
#define LV_TEXT_FLAG_NONE 0
#define LV_EVENT_VALUE_CHANGED 0
#define ESP_LOGI(...) ((void)0)
typedef struct { int align,x,y,w,h; bool hidden; } lv_obj_t;
typedef struct { int line_height; } lv_font_t;
typedef struct { int x,y; } lv_point_t;
typedef void lv_event_t;
typedef struct { lv_obj_t *parent; bool enabled; int top;
    const lv_font_t *font;
    char draft[2001],spelling[64]; void (*layout)(int,void*); void *layout_context;
} dock_owner_t;
static lv_obj_t objects[12],kb;
static lv_obj_t *editor,*input,*keyboard,*candidate_panel,*composition,*candidates,*previous,*next;
static lv_obj_t *status;
static lv_font_t font={54};
static dock_owner_t saved,*owner;
static bool pending,expanded,touch_keyboard,status_visible;
static char spelling[64],text[2001];
static int live,deletions,last_height;
static int cpu_region_top=UI_HEIGHT;
static unsigned candidate_count;
static char candidate_labels[6][64];
static const char *candidate_map[12];
static void sync_dock(void) {}
static bool text_entry_active(void) { return editor != 0; }
static void keyboard_event(lv_event_t *e) {}
static void lv_keyboard_def_event_cb(lv_event_t *e) {}
static lv_obj_t *lv_keyboard_create(lv_obj_t *p) {
    memset(&kb,0,sizeof(kb)); kb.align=LV_ALIGN_BOTTOM_MID; live++; return &kb;
}
static void lv_obj_set_size(lv_obj_t *o,int w,int h) { o->w=w;o->h=h; }
static void lv_obj_set_pos(lv_obj_t *o,int x,int y) { o->x=x;o->y=y; }
static void lv_obj_set_align(lv_obj_t *o,int a) { o->align=a; }
static void lv_obj_set_hidden(lv_obj_t *o,bool h) { o->hidden=h; }
static void lv_obj_set_style_text_font(lv_obj_t *o,void *f,int p) {}
static void lv_obj_remove_event_cb(lv_obj_t *o,void *c) {}
static void lv_obj_add_event_cb(lv_obj_t *o,void *c,int e,void *u) {}
static void portrait_editor_cpu_region(int top) {}
static const char *lv_label_get_text(lv_obj_t *o) {return "1-6 / 80";}
static void lv_text_get_size(lv_point_t *s,const char *t,const lv_font_t *f,int l,int space,int max,int flags) {
    int length=(int)strlen(t)*28;
    s->x=length<max?length:max;
    s->y=(length ? (length+max-1)/max : 1)*(f->line_height+space)-space;
}
static void lv_buttonmatrix_set_map(lv_obj_t *o,const char **map) {}
static const char *lv_textarea_get_text(lv_obj_t *o) { return text; }
static void lv_obj_delete(lv_obj_t *o) {
    deletions++;
    if (o==&kb) live--;
    else if (o==editor) { if(keyboard) {live--; keyboard=0;} editor=0;owner=0; }
}
static void layout_dock(void);
'''
        for signature in ["static int layout_candidates(",
                          "static void layout_dock(void)\n{",
                          "void text_entry_focus(bool touch)",
                          "static void close_editor(",
                          "void text_entry_release(lv_obj_t *parent)"]:
            stub += function(source, signature) + "\n"
        stub += r'''
static void record_height(int h,void *c) {last_height=h;}
__declspec(dllexport) int exercise(int top) {
    memset(objects,0,sizeof(objects)); memset(&saved,0,sizeof(saved));
    editor=&objects[0];input=&objects[1];candidate_panel=&objects[2];
    composition=&objects[3];candidates=&objects[4];previous=&objects[5];next=&objects[6];
    status=&objects[8];saved.font=&font;
    saved.parent=&objects[7];saved.enabled=true;saved.top=top;saved.layout=record_height;
    owner=&saved; keyboard=0;live=deletions=0;pending=expanded=touch_keyboard=false;
    strcpy(text,"draft-preserved");strcpy(spelling,"nihao");
    text_entry_focus(false);
    if (keyboard || last_height!=UI_INPUT_COLLAPSED) return 1;
    for (int i=0;i<1000;i++) {
        text_entry_focus(true);
        if(!keyboard || live!=1 || keyboard->align!=LV_ALIGN_TOP_LEFT) return 2;
        if(editor->y+keyboard->y+keyboard->h>UI_HEIGHT-UI_MARGIN) return 3;
        if(keyboard->y-input->y-56!=20) return 4;
        close_editor(0);
        if(keyboard || live || last_height!=UI_INPUT_COLLAPSED) return 5;
        if(editor->y!=top) return 6;
    }
    text_entry_focus(true);
    text_entry_release(saved.parent);
    if(editor || owner || live) return 7;
    if(strcmp(saved.draft,text) || strcmp(saved.spelling,spelling)) return 8;
    return 0;
}
__declspec(dllexport) int popup(int length,int count,int top) {
    exercise(756);
    owner=&saved;candidate_panel=&objects[2];composition=&objects[3];
    candidates=&objects[4];previous=&objects[5];next=&objects[6];status=&objects[8];
    candidate_count=count;
    for(int i=0;i<count;i++) {memset(candidate_labels[i],'a',length);candidate_labels[i][length]=0;}
    int y=layout_candidates(top);
    if(y<UI_MARGIN || candidate_panel->w>UI_CONTENT_WIDTH) return -1;
    if(candidates->h<count*(font.line_height+8)) return -2;
    if(candidate_panel->y+candidate_panel->h!=top-8) return -3;
    if(count && strcmp(candidate_map[count*2-1],"")) return -4;
    return candidate_panel->w;
}
'''
        path = work / "dock.c"
        path.write_text(stub, encoding="utf-8")
        dll = work / "dock.dll"
        subprocess.run([str(TCC), "-shared", "-I" + str(MAIN), str(path), "-o", str(dll)],
                       check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(dll))
        cls.addClassCleanup(_ctypes.FreeLibrary, cls.lib._handle)
        cls.lib.exercise.argtypes = [ctypes.c_int]
        cls.lib.popup.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    def test_home_keyboard_1000_cycles_bounds_and_release(self):
        self.assertEqual(self.lib.exercise(756), 0)

    def test_transcript_keyboard_1000_cycles_bounds_and_release(self):
        self.assertEqual(self.lib.exercise(840), 0)

    def test_popup_fits_six_full_font_rows_and_shrinks_to_content(self):
        for top in [704, 756, 840]:
            for count in range(7):
                short = self.lib.popup(3, count, top)
                long = self.lib.popup(30, count, top)
                self.assertGreater(short, 0)
                self.assertLessEqual(short, long)
                self.assertLessEqual(long, 560)
                if count:
                    self.assertLess(short, long)

    def test_release_precedes_asr_page_and_worker_allocations(self):
        source = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        entry = function(source, "esp_err_t speech_to_text_mode_enter(")
        for allocation in ["xEventGroupCreate()", "heap_caps_malloc(", "lv_obj_create(NULL)", "xTaskCreate("]:
            self.assertLess(entry.index("text_entry_release("), entry.index(allocation))
        for stage in range(1, 6):
            self.assertIn(f"entry_failed({stage},", entry)
        self.assertLess(entry.index("xTaskCreate("), entry.index("lv_obj_create(NULL)"))
        self.assertLess(entry.index("text_entry_set_enabled(text_screen, false)"),
                        entry.index("xEventGroupSetBits(events, START_BIT)"))
        worker = function(source, "static void mode_task(")
        self.assertLess(worker.index("xEventGroupWaitBits(events, START_BIT"),
                        worker.index("prepare_voice()"))

    def test_dock_does_not_rebind_callbacks_to_page_owned_language_button(self):
        source = (MAIN / "text_entry.c").read_text(encoding="utf-8")
        draw_list = source.split("lv_obj_t *draw_objects[] =", 1)[1].split(";", 1)[0]
        self.assertNotIn("language", draw_list)
        deleted = function(source, "static void deleted(")
        self.assertNotIn("lv_obj_remove_event_cb(language", deleted)


if __name__ == "__main__":
    unittest.main()
