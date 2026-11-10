"""Execute the landing builder: fonts, callback ownership and idle readiness.

The double models line boxes/parent origins, not rendering or hardware flush.
"""
import ctypes
import _ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/p4_main/main"
TCC = ROOT / "logs/chat-host-tools/tcc/tcc.exe"

STUB = r'''
#ifndef QH_HOME_TEST_LVGL_H
#define QH_HOME_TEST_LVGL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define LV_SIZE_CONTENT 20000
#define LV_OPA_COVER 255
#define LV_OPA_TRANSP 0
#define LV_TEXT_FLAG_NONE 0
#define LV_OBJ_FLAG_USER_1 1
#define LV_TEXT_ALIGN_LEFT 0
#define LV_TEXT_ALIGN_CENTER 1
#define LV_TEXT_ALIGN_RIGHT 2
#define LV_LABEL_LONG_WRAP 0
#define LV_STATE_DISABLED 1
#define LV_EVENT_DRAW_MAIN 1
#define LV_EVENT_CLICKED 2
typedef struct {int line_height,px,base_line;} lv_font_t;
typedef struct {int ignored;} lv_image_dsc_t;
typedef int lv_color_t;
typedef struct {int x1,y1,x2,y2;} lv_area_t;
typedef struct {int x,y;} lv_point_t;
typedef struct {int box_h,ofs_y;} lv_font_glyph_dsc_t;
typedef int lv_layer_t;
typedef struct {int radius,bg_color;} lv_draw_rect_dsc_t;
typedef struct object lv_obj_t;
typedef struct {lv_obj_t *target;} lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t*);
struct object {lv_obj_t *parent;int x,y,w,h,kind,border,clickable,flags;
    const char *text;const lv_font_t *font;lv_event_cb_t callback;};
static lv_font_t small={27,14},large={54,28};
#define LV_FONT_DEFAULT (&small)
static lv_obj_t pool[96];static int allocated,style_writes;
static lv_obj_t *make(lv_obj_t *p,int k){lv_obj_t *o=&pool[allocated++];o->parent=p;o->kind=k;return o;}
static lv_obj_t *lv_obj_create(lv_obj_t *p){return make(p,0);}
static lv_obj_t *lv_button_create(lv_obj_t *p){return make(p,1);}
static lv_obj_t *lv_image_create(lv_obj_t *p){return make(p,2);}
static void lv_obj_set_size(lv_obj_t *o,int w,int h){o->w=w;o->h=h;}
static void lv_obj_set_pos(lv_obj_t *o,int x,int y){o->x=x;o->y=y;}
static int lv_obj_get_y(lv_obj_t *o){return o->y;}
static void lv_obj_set_x(lv_obj_t *o,int x){o->x=x;}
static void lv_obj_set_y(lv_obj_t *o,int y){o->y=y;}
static void lv_obj_set_width(lv_obj_t *o,int w){o->w=w;}
static const lv_font_t *lv_obj_get_style_text_font(lv_obj_t *o,int p){return o->font;}
static int lv_obj_get_style_text_letter_space(lv_obj_t *o,int p){return 0;}
static void lv_obj_set_style_bg_opa(lv_obj_t *o,int v,int p){}
static int lv_font_get_glyph_dsc(const lv_font_t *f,lv_font_glyph_dsc_t *g,unsigned cp,unsigned n){g->box_h=f->px;g->ofs_y=0;return 1;}
static void lv_text_get_size(lv_point_t *p,const char *s,const lv_font_t *f,int a,int b,int w,int flag){
 int width=0;for(const unsigned char *c=(const unsigned char*)s;*c;++c)
  if((*c&0xc0)!=0x80)width+=*c<128?f->px/2:f->px;
 p->x=width;p->y=f->line_height;
}
static void lv_obj_get_coords(lv_obj_t *o,lv_area_t *a){
    int x=o->x,y=o->y;for(lv_obj_t *p=o->parent;p;p=p->parent){x+=p->x+p->border;y+=p->y+p->border;}
    *a=(lv_area_t){x,y,x+o->w-1,y+o->h-1};
}
static int lv_obj_get_style_border_width(lv_obj_t *o,int s){return o->border;}
static int lv_obj_get_height(lv_obj_t *o){return o->kind==3 ? o->font->line_height : o->h;}
static void lv_obj_update_layout(lv_obj_t *o){}
static void lv_obj_set_style_text_font(lv_obj_t *o,const lv_font_t *f,int s){o->font=f;}
static void lv_obj_set_style_text_align(lv_obj_t *o,int a,int s){}
static void lv_obj_set_style_image_recolor(lv_obj_t *o,int c,int s){}
static void lv_obj_set_style_image_recolor_opa(lv_obj_t *o,int v,int s){}
static void lv_obj_set_style_radius(lv_obj_t *o,int r,int s){}
static void lv_obj_set_style_pad_all(lv_obj_t *o,int v,int s){}
static void lv_obj_set_style_border_width(lv_obj_t *o,int w,int s){o->border=w;}
static void lv_obj_set_style_border_color(lv_obj_t *o,int c,int s){}
static void lv_obj_set_style_bg_color(lv_obj_t *o,int c,int s){style_writes++;}
static void lv_obj_set_scrollable(lv_obj_t *o,bool v){}
static void lv_obj_set_clickable(lv_obj_t *o,bool v){o->clickable=v;}
static void lv_obj_add_flag(lv_obj_t *o,int f){o->flags|=f;}
static void lv_obj_add_state(lv_obj_t *o,int s){}
static void lv_obj_remove_style_all(lv_obj_t *o){}
static void lv_image_set_src(lv_obj_t *o,const void *s){}
static void lv_label_set_long_mode(lv_obj_t *o,int m){}
static const char *lv_label_get_text(lv_obj_t *o){return o->text;}
static void lv_label_set_text(lv_obj_t *o,const char *s){o->text=s;}
static void lv_obj_add_event_cb(lv_obj_t *o,lv_event_cb_t cb,int e,void *u){if(e==LV_EVENT_CLICKED)o->callback=cb;}
static lv_obj_t *lv_event_get_target_obj(lv_event_t *e){return e->target;}
static lv_layer_t *lv_event_get_layer(lv_event_t *e){return 0;}
static int lv_color_hex(int c){return c;}
static void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *r){memset(r,0,sizeof(*r));}
static void lv_draw_rect(lv_layer_t *l,const lv_draw_rect_dsc_t *r,const lv_area_t *a){}
#endif
'''


class HomeTests(unittest.TestCase):
    def test_font_switch_actions_and_readiness_do_not_recreate_widgets(self):
        if not TCC.exists():
            self.skipTest("Host TCC unavailable")
        source = (MAIN / "qinghe_home.c").read_text(encoding="utf-8")
        harness = '''#include "qinghe_home.h"
#include "qinghe_home_art.h"
const lv_image_dsc_t qinghe_logo_64={0}, qh_home_slogan={0}, qh_home_settings={0},
 qh_home_arrow_small={0},qh_home_arrow={0},qh_home_mic={0},qh_home_document={0},
 qh_home_camera={0},qh_home_keyboard={0};
int qh_color(unsigned c){return c;}
lv_obj_t *qh_theme_button(lv_obj_t *p){return make(p,2);}
void qh_screen(lv_obj_t *o){}
void qh_panel(lv_obj_t *o,unsigned c,int r){}
void qh_button(lv_obj_t *o,unsigned c){}
lv_obj_t *qh_label(lv_obj_t *p,const lv_font_t *f,const char *t,unsigned c){
 lv_obj_t *o=make(p,3);o->text=t;o->font=f;return o;
}
static int clicked;
static void voice(lv_event_t *e){clicked|=1;}
static void transcript(lv_event_t *e){clicked|=2;}
static void camera(lv_event_t *e){clicked|=4;}
static void settings(lv_event_t *e){clicked|=8;}
static void help(lv_event_t *e){clicked|=16;}
'''
        # Same unit makes the production builder use the double's state.
        harness += source
        harness += r'''
__declspec(dllexport) int run(void){
    qh_home_t home={0};qh_home_create(&home,&small,&large,voice,transcript,camera,settings,help);
    if(allocated!=QHH_COUNT+2)return 1;
    const int actions[]={QHH_VOICE,QHH_TRANSCRIPT,QHH_CAMERA,QHH_SETTINGS,QHH_HELP};
    for(int i=0;i<5;i++){
        lv_obj_t *o=home.objects[actions[i]];lv_event_t e={o};
        if(!o->clickable || o->w<48 || o->h<48 || !o->callback)return 2;
        o->callback(&e);
    }
    if(clicked!=31 || home.objects[QHH_MIC]->parent!=home.objects[QHH_VOICE])return 3;
    for(int i=0;i<QHH_COUNT;i++)if(qhh_items[i].kind!=QHH_BUTTON && home.objects[i]->clickable)return 4;
    for(int round=0;round<30;round++){
        qh_home_set_fonts(&home,round%2 ? &small : &large,&large);
        if(home.objects[QHH_PRODUCT]->font!=&large || home.objects[QHH_VOICE_TITLE]->font!=&large)return 5;
        for(int i=0;i<QHH_COUNT;i++){
            lv_area_t a;lv_obj_get_coords(home.objects[i],&a);
            int h=lv_obj_get_height(home.objects[i]);
            if(a.x1<0 || a.y1<0 || a.x2>=600 || a.y1+h>1024)return 6;
        }
        if(allocated!=QHH_COUNT+2)return 7;
        lv_obj_t *usb=home.objects[QHH_USB_CAPTION],*icon=home.objects[QHH_USB_ICON];
        if(icon->x+usb->x+usb->w<599 || icon->x+usb->x+usb->w>600 || usb->x-icon->x!=36)return 13;
        lv_obj_t *conn=home.objects[QHH_CONNECTION],*dot=home.objects[QHH_CONNECTION_DOT];
        if(conn->x+conn->w!=570 || conn->x-dot->x!=12)return 14;
    }
    qh_home_set_readiness(&home,false,false,false);
    if(strcmp(home.objects[QHH_CONNECTION]->text,"未联网"))return 8;
    qh_home_set_readiness(&home,true,false,false);
    if(strcmp(home.objects[QHH_CONNECTION]->text,"服务准备中"))return 9;
    qh_home_set_readiness(&home,true,true,true);
    if(strcmp(home.objects[QHH_STATE]->text,"欢迎播报中"))return 10;
    qh_home_set_readiness(&home,true,true,false);
    if(strcmp(home.objects[QHH_STATE]->text,"准备好交流"))return 11;
    const int writes=style_writes;
    for(int i=0;i<100;i++)qh_home_set_readiness(&home,true,true,false);
    return writes!=style_writes ? 12 : 0;
}
'''
        # Header normally first included by the harness must expose layout data here.
        harness = '#define QH_HOME_LAYOUT_DATA\n' + harness
        with tempfile.TemporaryDirectory(dir=ROOT / "logs") as folder:
            work = Path(folder)
            (work / "lvgl.h").write_text(STUB, encoding="utf-8")
            (work / "home.c").write_text(harness, encoding="utf-8")
            dll = work / "home.dll"
            result = subprocess.run([str(TCC), "-shared", "-I"+str(work), "-I"+str(MAIN),
                                     str(work / "home.c"), "-o", str(dll)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lib = ctypes.CDLL(str(dll))
            try:
                self.assertEqual(lib.run(), 0)
            finally:
                _ctypes.FreeLibrary(lib._handle)


if __name__ == "__main__":
    unittest.main()
