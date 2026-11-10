"""Run production motion ownership with an LVGL lifecycle double (no rendering)."""
import ctypes
import _ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/p4_main/main"
TCC = ROOT / "logs/chat-host-tools/tcc/tcc.exe"


class MotionTests(unittest.TestCase):
    def test_screen_replacement_completion_and_local_object_deletion(self):
        if not TCC.exists():
            self.skipTest("Host TCC unavailable")
        source = (MAIN / "qinghe_ui.c").read_text(encoding="utf-8")
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "ui_layout.h"
#define QH_SOFT 1
#define QH_ACCENT 2
#define QH_LINE 3
#define QH_MUTED 4
#define LV_OPA_TRANSP 0
#define LV_EVENT_DELETE 1
#define LV_EVENT_DRAW_MAIN 2
#define LV_RADIUS_CIRCLE 32767
#define ESP_LOGI(...) ((void)0)
typedef struct obj lv_obj_t;
typedef struct {lv_obj_t *target;} lv_event_t;
typedef struct anim {void *var; int start,end,duration; bool live;
    void (*exec)(void*,int32_t);void (*done)(struct anim*);} lv_anim_t;
struct obj {lv_obj_t *parent; bool live;int x,w,h;void *user_data;
    void (*deleted)(lv_event_t*);void (*draw)(lv_event_t*);};
typedef int lv_layer_t;
typedef struct {int x1,y1,x2,y2;} lv_area_t;
typedef struct {int radius,border_color,border_width,bg_color;} lv_draw_rect_dsc_t;
static lv_obj_t pool[32],top,*active;
static lv_anim_t animations[8];
static uint32_t ticks;
static int invalid_access;
static int invalidations,draws;
static lv_obj_t *lv_obj_create(lv_obj_t *parent) {
    for(int i=0;i<32;i++)if(!pool[i].live){memset(&pool[i],0,sizeof(pool[i]));
        pool[i].live=true;pool[i].parent=parent;return &pool[i];}
    invalid_access++;return 0;
}
static lv_obj_t *lv_layer_top(void) {return &top;}
static lv_obj_t *lv_screen_active(void) {return active;}
static void lv_screen_load(lv_obj_t *s) {active=s;}
static uint32_t lv_tick_get(void) {return ticks;}
static lv_obj_t *lv_event_get_target_obj(lv_event_t *e){return e->target;}
static void *lv_obj_get_user_data(lv_obj_t *o){return o->user_data;}
static void lv_obj_set_user_data(lv_obj_t *o,void *v){o->user_data=v;}
static void lv_obj_set_clickable(lv_obj_t *o,bool v){}
static void lv_obj_invalidate(lv_obj_t *o){if(!o->live)invalid_access++;invalidations++;}
static void lv_obj_get_coords(lv_obj_t *o,lv_area_t *a){*a=(lv_area_t){0,0,o->w-1,o->h-1};}
static lv_layer_t *lv_event_get_layer(lv_event_t *e){return 0;}
static int qh_color(int c){return c;}
static void qh_theme_apply(lv_obj_t *o){}
static void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *d){memset(d,0,sizeof(*d));}
static void lv_draw_rect(lv_layer_t *l,lv_draw_rect_dsc_t *d,const lv_area_t *a){
    draws++;if(a->x1<0 || a->y1<0 || a->x2>=216 || a->y2>=216)invalid_access++;
}
static void lv_obj_remove_style_all(lv_obj_t *o) {}
static void lv_obj_set_scrollable(lv_obj_t *o,bool v) {}
static void lv_obj_set_style_bg_opa(lv_obj_t *o,int v,int p) {}
static void qh_panel(lv_obj_t *o,int c,int r) {}
static void lv_obj_set_size(lv_obj_t *o,int w,int h){o->w=w;o->h=h;}
static void lv_obj_set_x(lv_obj_t *o,int x){if(!o->live)invalid_access++;o->x=x;}
static void lv_obj_set_style_translate_y(lv_obj_t *o,int y,int p){if(!o->live)invalid_access++;}
static void lv_obj_add_event_cb(lv_obj_t *o,void (*cb)(lv_event_t*),int ev,void *ud){
    if(ev==LV_EVENT_DELETE)o->deleted=cb;else if(ev==LV_EVENT_DRAW_MAIN)o->draw=cb;
}
static void lv_anim_delete(void *o,void (*cb)(void*,int32_t)){
    for(int i=0;i<8;i++)if(animations[i].var==o && (!cb || animations[i].exec==cb))animations[i].live=false;
}
static void lv_obj_delete(lv_obj_t *o){
    if(!o->live){invalid_access++;return;}
    if(o->deleted){lv_event_t e={o};o->deleted(&e);}
    for(int i=0;i<32;i++)if(pool[i].live && pool[i].parent==o)lv_obj_delete(&pool[i]);
    /* Actual LVGL lv_obj_destructor also calls lv_anim_delete(obj,NULL). */
    lv_anim_delete(o,0);o->live=false;
}
static void lv_anim_init(lv_anim_t *a){memset(a,0,sizeof(*a));}
static void lv_anim_set_var(lv_anim_t *a,void *v){a->var=v;}
static void lv_anim_set_values(lv_anim_t *a,int s,int e){a->start=s;a->end=e;}
static void lv_anim_set_duration(lv_anim_t *a,int d){a->duration=d;}
static void lv_anim_path_ease_out(void){}
static void lv_anim_set_path_cb(lv_anim_t *a,void (*p)(void)){}
static void lv_anim_set_exec_cb(lv_anim_t *a,void (*p)(void*,int32_t)){a->exec=p;}
static void lv_anim_set_completed_cb(lv_anim_t *a,void (*p)(lv_anim_t*)){a->done=p;}
static void lv_anim_start(lv_anim_t *a){
    for(int i=0;i<8;i++)if(!animations[i].live){animations[i]=*a;animations[i].live=true;
        a->exec(a->var,a->start);return;}
    invalid_access++;
}
static int count_anims(void){int n=0;for(int i=0;i<8;i++)n+=animations[i].live;return n;}
static int count_objects(void){int n=0;for(int i=0;i<32;i++)n+=pool[i].live;return n;}
static void finish(void){ticks+=220;for(int i=0;i<8;i++)if(animations[i].live){
    lv_anim_t a=animations[i];animations[i].live=false;a.exec(a.var,a.end);if(a.done)a.done(&a);}}
'''
        harness += source[source.index("static const uint8_t startup_leaf_bits"):source.index("void qh_set_settings_callback")]
        harness += source[source.index("/* Static-home phase:"):]
        harness += r'''
__declspec(dllexport) int exercise(void){
    lv_obj_t *old=lv_obj_create(0),*next=lv_obj_create(0);active=old;
    qh_screen_load(next,false);lv_obj_delete(old);
    if(active!=next || qh_navigation_busy() || count_objects()!=1 || count_anims())return 1;
    for(int n=0;n<100;n++){
        old=active;next=lv_obj_create(0);qh_screen_load(next,false);lv_obj_delete(old);
        lv_obj_t *last=lv_obj_create(0);qh_screen_load_camera(last,true);lv_obj_delete(next);
        if(active!=last || qh_navigation_busy() || count_objects()!=1 || count_anims())return 2;
    }
    lv_obj_t *local=lv_obj_create(active);qh_reveal(local);qh_reveal(local);
    if(count_anims())return 3;
    lv_obj_delete(local);qh_screen_load(active,false);
    if(qh_navigation_busy() || count_anims())return 4;
    lv_obj_delete(active);if(count_objects() || invalid_access)return 5;
    lv_obj_t *leaf=qh_startup_leaf(0);
    if(count_objects()!=1 || count_anims())return 10;
    for(unsigned elapsed=0;elapsed<2400;elapsed+=200)qh_startup_leaf_step(leaf,elapsed);
    int first_loop=invalidations;
    for(unsigned elapsed=2400;elapsed<60000;elapsed+=200)qh_startup_leaf_step(leaf,elapsed);
    if(invalidations<=first_loop || count_anims())return 11;
    int same=invalidations;qh_startup_leaf_step(leaf,59800);
    if(invalidations!=same)return 14;
    lv_event_t event={leaf};leaf->draw(&event);if(draws!=81 || invalid_access)return 12;
    lv_obj_delete(leaf);return count_objects() || invalid_access ? 13:0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / "logs") as folder:
            work = Path(folder)
            path = work / "motion.c"
            path.write_text(harness, encoding="utf-8")
            dll = work / "motion.dll"
            result = subprocess.run([str(TCC), "-shared", "-I" + str(MAIN), str(path), "-o", str(dll)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lib = ctypes.CDLL(str(dll))
            try:
                self.assertEqual(lib.exercise(), 0)
            finally:
                _ctypes.FreeLibrary(lib._handle)


if __name__ == "__main__":
    unittest.main()
