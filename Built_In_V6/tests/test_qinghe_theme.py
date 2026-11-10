"""Run the production palette walk with local-property and lazy-page doubles.

This checks state preservation and color selection, not LVGL rendering or NVS.
"""
import ctypes
import _ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / 'firmware/p4_main/main'
TCC = ROOT / 'logs/chat-host-tools/tcc/tcc.exe'

STUB = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef unsigned lv_color_t,lv_style_selector_t,lv_style_prop_t,lv_part_t,lv_state_t;
typedef union {lv_color_t color;int num;} lv_style_value_t;
typedef struct {unsigned prop,selector;lv_style_value_t value;} binding;
typedef struct obj {binding colors[16];int count,scroll,cursor,state;const char *draft;
 struct obj *children[4];unsigned child_count;} lv_obj_t;
typedef void lv_event_t;typedef void lv_font_t;typedef void (*lv_event_cb_t)(void*);
#define LV_USE_THEME_DEFAULT 0
#define LV_OPA_20 51
#define LV_STYLE_RES_FOUND 1
enum {LV_STYLE_BG_COLOR,LV_STYLE_TEXT_COLOR,LV_STYLE_BORDER_COLOR,LV_STYLE_IMAGE_RECOLOR,
LV_STYLE_LINE_COLOR,LV_STYLE_OUTLINE_COLOR,LV_STYLE_ARC_COLOR};
enum {LV_PART_MAIN=0,LV_PART_SCROLLBAR=256,LV_PART_INDICATOR=512,LV_PART_KNOB=768,
LV_PART_SELECTED=1024,LV_PART_ITEMS=1280,LV_PART_CURSOR=1536,LV_PART_TEXTAREA_PLACEHOLDER=1792};
enum {LV_STATE_PRESSED=1,LV_STATE_DISABLED=2,LV_STATE_CHECKED=4,LV_STATE_FOCUSED=8};
static lv_obj_t active,top;
static int saves,invalidations;
static lv_color_t lv_color_hex(unsigned x){return x;}
static unsigned lv_color_to_u32(lv_color_t x){return x;}
static bool lv_color_eq(lv_color_t x,lv_color_t y){return x==y;}
static lv_color_t lv_color_darken(lv_color_t x,int amount){
 unsigned y=0;for(int shift=0;shift<=16;shift+=8)y|=(((x>>shift)&255)*(255-amount)/255)<<shift;
 return y;
}
static int lv_obj_get_local_style_prop(lv_obj_t *o,unsigned p,lv_style_value_t *v,unsigned s){
 for(int i=0;i<o->count;i++)if(o->colors[i].prop==p && o->colors[i].selector==s){*v=o->colors[i].value;return 1;}
 return 0;
}
static void lv_obj_set_local_style_prop(lv_obj_t *o,unsigned p,lv_style_value_t v,unsigned s){
 for(int i=0;i<o->count;i++)if(o->colors[i].prop==p && o->colors[i].selector==s){o->colors[i].value=v;return;}
 o->colors[o->count++]=(binding){p,s,v};
}
static unsigned lv_obj_get_child_count(lv_obj_t *o){return o->child_count;}
static lv_obj_t *lv_obj_get_child(lv_obj_t *o,unsigned i){return o->children[i];}
static lv_obj_t *lv_screen_active(void){return &active;}
static lv_obj_t *lv_layer_top(void){return &top;}
static void lv_obj_invalidate(lv_obj_t *o){invalidations++;}
static void saved(void){saves++;}
static void put(lv_obj_t *o,unsigned p,unsigned s,unsigned c){lv_style_value_t v={.color=c};lv_obj_set_local_style_prop(o,p,v,s);}
static unsigned get(lv_obj_t *o,unsigned p,unsigned s){lv_style_value_t v;return lv_obj_get_local_style_prop(o,p,&v,s)?v.color:0;}
'''


class ThemeTests(unittest.TestCase):
    def test_saved_theme_valid_invalid_and_read_failure(self):
        from test_portrait_layout import function
        source=(MAIN/'main.c').read_text(encoding='utf-8')
        harness=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
static bool dark,settings_have_saved_dark,settings_saved_dark;
static int settings_nvs,value,error;
static void qh_theme_restore(bool v){dark=v;}
static bool qh_theme_dark(void){return dark;}
static int nvs_get_u8(int h,const char *key,uint8_t *out){
 if(strcmp(key,"ui_dark_v1"))return 1;*out=value;return error;
}
'''+function(source,'static void settings_restore_theme(')+r'''
__declspec(dllexport) int run(void){
 for(value=0;value<256;value++){
  dark=true;error=0;settings_restore_theme();
  if(dark!=(value==1) || settings_have_saved_dark!=(value<=1))return 1;
 }
 value=1;error=17;dark=true;settings_restore_theme();
 return dark || settings_have_saved_dark ? 2:0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT/'logs') as directory:
            work=Path(directory);path=work/'saved.c';path.write_text(harness,encoding='utf-8')
            dll=work/'saved.dll'
            result=subprocess.run([str(TCC),'-shared',str(path),'-o',str(dll)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            lib=ctypes.CDLL(str(dll))
            try:
                self.assertEqual(lib.run(),0)
            finally:
                _ctypes.FreeLibrary(lib._handle)

    def test_live_and_lazy_theme_updates_keep_business_state(self):
        if not TCC.exists():
            self.skipTest('Host TCC unavailable')
        source = (MAIN / 'qinghe_theme.c').read_text(encoding='utf-8')
        exercise = r'''
__declspec(dllexport) int run(void){
 qh_theme_restore(false);qh_theme_set_save_callback(saved);
 lv_obj_t editor={0},hidden={0};active.children[active.child_count++]=&editor;
 editor.scroll=311;editor.cursor=5;editor.state=99;editor.draft="retained text";
 put(&active,LV_STYLE_BG_COLOR,0,QH_BG);
 put(&editor,LV_STYLE_TEXT_COLOR,0,QH_INK);
 put(&editor,LV_STYLE_BG_COLOR,LV_PART_CURSOR,QH_INK);
 put(&editor,LV_STYLE_TEXT_COLOR,LV_PART_TEXTAREA_PLACEHOLDER,QH_MUTED);
 put(&editor,LV_STYLE_BG_COLOR,LV_PART_ITEMS|LV_STATE_PRESSED,lv_color_darken(QH_SOFT,51));
 put(&top,LV_STYLE_BG_COLOR,0,QH_SURFACE);
 put(&hidden,LV_STYLE_BG_COLOR,0,QH_USER);
 for(int n=0;n<20;n++){
  qh_theme_toggle(0);
  if(!qh_theme_dark() || get(&active,LV_STYLE_BG_COLOR,0)!=0x18221E)return 1;
  if(get(&editor,LV_STYLE_TEXT_COLOR,0)!=qh_color(QH_INK))return 2;
  if(get(&editor,LV_STYLE_TEXT_COLOR,LV_PART_TEXTAREA_PLACEHOLDER)!=qh_color(QH_MUTED))return 3;
  if(get(&editor,LV_STYLE_BG_COLOR,LV_PART_CURSOR)!=qh_color(QH_INK))return 4;
  if(get(&top,LV_STYLE_BG_COLOR,0)!=qh_color(QH_SURFACE))return 5;
  qh_theme_apply(&hidden);if(get(&hidden,LV_STYLE_BG_COLOR,0)!=qh_color(QH_USER))return 6;
  qh_theme_toggle(0);qh_theme_apply(&hidden);
  if(qh_theme_dark() || get(&hidden,LV_STYLE_BG_COLOR,0)!=QH_USER)return 7;
  if(editor.scroll!=311 || editor.cursor!=5 || editor.state!=99 || strcmp(editor.draft,"retained text"))return 8;
  if(get(&editor,LV_STYLE_BG_COLOR,LV_PART_ITEMS|LV_STATE_PRESSED)!=lv_color_darken(QH_SOFT,51))return 9;
 }
 return saves==40 && invalidations==40 ? 0:10;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT/'logs') as directory:
            work=Path(directory)
            (work/'lvgl.h').write_text('#pragma once\n'+STUB,encoding='utf-8')
            (work/'stdatomic.h').write_text('#define atomic_bool bool\n#define atomic_load(p) (*(p))\n#define atomic_store(p,v) (*(p)=(v))\n')
            (work/'theme.c').write_text(source+exercise,encoding='utf-8')
            dll=work/'theme.dll'
            p=subprocess.run([str(TCC),'-shared','-I'+str(work),'-I'+str(MAIN),str(work/'theme.c'),'-o',str(dll)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            lib=ctypes.CDLL(str(dll))
            try:
                self.assertEqual(lib.run(),0)
            finally:
                _ctypes.FreeLibrary(lib._handle)


if __name__ == '__main__':
    unittest.main()
