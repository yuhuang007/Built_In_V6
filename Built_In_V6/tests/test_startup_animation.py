"""Asset/boot animation guards; requires only standard library for build checks."""
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/p4_main/main"


class AnimationTests(unittest.TestCase):
    def test_asset_header_hash_loop_budget(self):
        data = (ROOT / "assets/startup/loading.gif").read_bytes()
        manifest = json.loads((ROOT / "assets/startup/manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(data[:6], b"GIF89a")
        self.assertEqual(struct.unpack_from("<HH", data, 6), (320, 320))
        self.assertIn(b"NETSCAPE2.0\x03\x01\x00\x00\x00", data)
        self.assertLess(len(data), 220000)
        self.assertEqual(len(data), manifest["bytes"])
        self.assertEqual(hashlib.sha256(data).hexdigest(), manifest["sha256"])
        self.assertEqual(sum(manifest["frame_durations_ms"]), 3600)

    def test_bootstrap_chinese_glyphs(self):
        font = (MAIN / "font_startup_zh.c").read_text(encoding="utf-8")
        glyphs = {int(cp, 16) for cp in re.findall(r"/\* U\+([0-9A-F]+)", font)}
        manifest = json.loads((ROOT / "assets/startup/manifest.json").read_text(encoding="utf-8"))
        self.assertTrue({ord(c) for c in manifest["bootstrap_symbols"]}.issubset(glyphs))
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        startup = source[source.index("static void startup_status_timer"):
                         source.index("void app_main(void)")]
        for literal in re.findall(r'"([^"\n]+)"', startup):
            for char in literal:
                if ord(char) > 127:
                    self.assertIn(ord(char), glyphs, char)

    def test_stage_progress_not_elapsed_or_fake_completion(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        timer = source[source.index("static void startup_status_timer"):
                       source.index("static void create_startup_placeholder")]
        self.assertIn("lv_bar_set_value(startup_bar, (int)percent", timer)
        self.assertNotIn("percent = elapsed", timer)
        self.assertNotIn("percent = 100", timer)
        self.assertIn("elapsed >= 20000", timer)
        self.assertIn("startup_last_percent", timer)

    def test_flash_rgb565_infinite_loop_and_release(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        self.assertIn("LV_COLOR_FORMAT_RGB565", source)
        self.assertIn("lv_gif_set_loop_count(animation, 0)", source)
        self.assertIn("startup_gif_asset.data = startup_gif_start", source)
        self.assertIn("lv_obj_delete(startup_overlay)", source)
        config = (ROOT / "firmware/p4_main/sdkconfig.sign").read_text()
        self.assertIn("CONFIG_LV_USE_GIF=y", config)

    def test_black_single_line_and_library_loop_semantics(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        self.assertIn("lv_obj_set_style_bg_color(startup_overlay, lv_color_black()", source)
        self.assertNotIn("startup_network_label", source)
        self.assertIn("LV_LABEL_LONG_CLIP", source)
        self.assertNotIn("startup_exit_pending", source)
        manifest = json.loads((ROOT / "assets/startup/manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["background"], "black")
        library = (ROOT / "firmware/p4_main/managed_components/lvgl__lvgl/src/widgets/gif/lv_gif.c").read_text(encoding="utf-8")
        self.assertIn("else if(gifobj->loop_count < 0)", library)


if __name__ == "__main__":
    unittest.main()
