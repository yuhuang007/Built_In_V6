"""Execute unchanged-value suppression; geometry/rotation use existing tests."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_portrait_layout import function

ROOT = Path(__file__).resolve().parents[1]


class ScrollUpdateTests(unittest.TestCase):
    def test_settings_idle_values_do_not_invalidate_and_changes_still_update(self):
        compiler = ROOT / 'logs/chat-host-tools/tcc/tcc.exe'
        if not compiler.exists():
            self.skipTest('Host TCC unavailable')
        source = (ROOT / 'firmware/p4_main/main/main.c').read_text(encoding='utf-8')
        harness = r'''
#include <string.h>
typedef struct {char text[64];int writes;} lv_obj_t;
static const char *lv_label_get_text(lv_obj_t *o){return o->text;}
static void lv_label_set_text(lv_obj_t *o,const char *s){strcpy(o->text,s);o->writes++;}
''' + function(source, 'static void settings_value_text(') + r'''
int main(void){
 lv_obj_t value={"45%",0};
 for(int i=0;i<400;i++)settings_value_text(&value,"45%");
 if(value.writes)return 1;
 settings_value_text(&value,"0%");settings_value_text(&value,"静音");
 for(int i=0;i<400;i++)settings_value_text(&value,"静音");
 settings_value_text(0,"45%");
 return value.writes!=2 || strcmp(value.text,"静音");
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'logs') as folder:
            path = Path(folder) / 'unchanged.c'
            path.write_text(harness, encoding='utf-8')
            result = subprocess.run([str(compiler), '-run', str(path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
