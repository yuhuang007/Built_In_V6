"""Production page-font preparation: boot reuse, bounded lock failure, bind errors."""
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


class PageFontTests(unittest.TestCase):
    def test_new_visual_font_borrowers_rebind_before_temporary_cache_release(self):
        source=(MAIN/'speech_to_text_mode.c').read_text(encoding='utf-8')
        harness=r'''
#include <stdbool.h>
#include <stddef.h>
typedef struct {int id;} lv_font_t;
typedef struct {lv_font_t *font;} lv_obj_t;
static lv_font_t old={1},cached_font_fixture={2};
static lv_obj_t label,guide,heading;
static lv_obj_t *transcript_label=&label,*empty_guide=&guide,*transcript_heading=&heading;
static lv_obj_t *status_label,*control_labels[4],*text_screen;
static lv_font_t *cached_native_font=&cached_font_fixture,*native_font_14=&cached_font_fixture,*native_font_28=&cached_font_fixture;
static lv_font_t *transcript_font=&old;
static bool transcript_font_owned=true;
static unsigned subtitle_size;
static int invalid,freed;
static bool bsp_display_lock(int n){return true;}
static void bsp_display_unlock(void){}
static void lv_obj_set_style_text_font(lv_obj_t *o,const lv_font_t *f,int p){if(o)o->font=(lv_font_t*)f;}
static void asr_display_font_destroy(lv_font_t *f){
 if(label.font==f || guide.font==f || heading.font==f)invalid++;freed++;
}
static void resize_transcript(void){}
static void text_entry_set_font(lv_obj_t *o,const lv_font_t *f){}
'''+function(source,'static bool bind_native_mode_fonts(')+r'''
int main(void){
 label.font=guide.font=heading.font=&old;
 if(!bind_native_mode_fonts() || invalid || freed!=1 || transcript_font_owned)return 1;
 return label.font!=&cached_font_fixture || guide.font!=&cached_font_fixture || heading.font!=&cached_font_fixture;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT/'logs') as folder:
            work=Path(folder);path=work/'bind.c';path.write_text(harness,encoding='utf-8')
            result=subprocess.run([str(TCC),'-run',str(path)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_prepared_resources_reused_and_failures_finish(self):
        if not TCC.exists():
            self.skipTest("Host TCC unavailable")
        source = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#define atomic_load(p) (*(p))
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_FAIL 2
#define ESP_ERR_INVALID_STATE 3
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
static bool fonts_ready,return_requested,bind_ok,parse_ok,lock_ok;
static int fonts_stage,resources_lock=1,verified,bound,parsed,taken,given,verify_error;
static uint32_t now;
static uint32_t esp_log_timestamp(void){return now;}
static void set_status(const char *s){}
static int xSemaphoreTake(int lock,int timeout){taken++;now+=timeout;return lock_ok;}
static void xSemaphoreGive(int lock){given++;}
static int sd_font_upgrade_verify_installed(void){verified++;return verify_error;}
static bool prepare_native_fonts(void){parsed++;fonts_ready=parse_ok;return parse_ok;}
static bool bind_native_mode_fonts(void){bound++;return bind_ok;}
'''
        harness += function(source, "static esp_err_t prepare_page_fonts(")
        harness += r'''
__declspec(dllexport) int run(void){
    fonts_ready=bind_ok=true;
    for(int i=0;i<30;i++)if(prepare_page_fonts()!=ESP_OK)return 1;
    if(verified || parsed || taken || given || bound!=30)return 2;
    fonts_ready=false;lock_ok=false;
    if(prepare_page_fonts()!=ESP_ERR_TIMEOUT || now!=1000 || bound!=30)return 3;
    lock_ok=true;verify_error=2;
    if(prepare_page_fonts()!=2 || verified!=1 || parsed || given!=1)return 4;
    verify_error=0;parse_ok=true;
    if(prepare_page_fonts()!=ESP_OK || !fonts_ready || parsed!=1 || given!=2)return 5;
    bind_ok=false;if(prepare_page_fonts()!=ESP_FAIL || parsed!=1 || verified!=2)return 6;
    return_requested=true;
    if(prepare_page_fonts()!=ESP_ERR_INVALID_STATE)return 7;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / "logs") as folder:
            work = Path(folder)
            path = work / "fonts.c"
            path.write_text(harness, encoding="utf-8")
            dll = work / "fonts.dll"
            result = subprocess.run([str(TCC), "-shared", str(path), "-o", str(dll)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lib = ctypes.CDLL(str(dll))
            try:
                self.assertEqual(lib.run(), 0)
            finally:
                _ctypes.FreeLibrary(lib._handle)
