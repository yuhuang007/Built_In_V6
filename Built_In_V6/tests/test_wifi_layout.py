"""Execute the production layout with natural line-box heights, no real SSID."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_portrait_layout import function, ROOT, TCC


class WifiLayoutTests(unittest.TestCase):
    def test_wrapped_status_grows_above_list_and_footer_stays_fixed(self):
        source = (ROOT / 'firmware/p4_main/main/wifi_settings.c').read_text(encoding='utf-8')
        harness = r'''
#include <stdbool.h>
#define LV_SIZE_CONTENT -1
#define UI_HEIGHT 1024
#define UI_MARGIN 20
#define UI_CONTENT_WIDTH 560
typedef struct {int x,y,w,h,natural;} lv_obj_t;
static lv_obj_t objects[6];
static lv_obj_t *screen=&objects[0],*scan_button=&objects[1],*current_network=&objects[2],
                *status=&objects[3],*list=&objects[4],*footer=&objects[5];
static void lv_obj_set_height(lv_obj_t *o,int v){o->h=v==-1?o->natural:v;}
static void lv_obj_update_layout(lv_obj_t *o){}
static int lv_obj_get_height(lv_obj_t *o){return o->h;}
static int lv_obj_get_y(lv_obj_t *o){return o->y;}
static void lv_obj_set_pos(lv_obj_t *o,int x,int y){o->x=x;o->y=y;}
static void lv_obj_set_size(lv_obj_t *o,int w,int h){o->w=w;o->h=h;}
''' + function(source, 'static void layout_network_page(') + r'''
int main(void){
 scan_button->y=100;scan_button->h=70;
 for(int line=27;line<=54;line+=27){
  for(int connection_lines=1;connection_lines<=2;connection_lines++){
   for(int state_lines=1;state_lines<=4;state_lines++){
    current_network->natural=line*connection_lines;
    status->natural=line*state_lines;footer->natural=line;
    layout_network_page();
    if(current_network->y!=182)return 1;
    if(status->y!=current_network->y+current_network->h+8)return 2;
    if(list->y!=status->y+status->h+12)return 3;
    if(list->y+list->h!=footer->y-12)return 4;
    if(footer->y+footer->h!=1002 || list->h<300)return 5;
   }
  }
 }
 return 0;
}
'''
        if not TCC.exists():
            self.skipTest('Host TCC unavailable')
        with tempfile.TemporaryDirectory(dir=ROOT / 'logs') as folder:
            path = Path(folder) / 'layout.c'
            path.write_text(harness, encoding='utf-8')
            result = subprocess.run([str(TCC), '-run', str(path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

