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
        # The retained GIF manifest describes the previous design; verify current literals below.
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
        self.assertIn("startup_stages[i]",timer)
        self.assertIn("pinyin_service_state()",timer)
        self.assertIn("wifi_link_up && network_prepared",timer)
        self.assertIn("startup_last_mask",timer)
        self.assertNotIn("percent = elapsed",timer)
        self.assertNotIn("percent = 100",timer)
        self.assertIn("elapsed >= 20000",timer)

    def test_qinghe_leaf_replaces_boot_logo_without_fullscreen_assets(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        cmake = (MAIN / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotIn("lv_gif_create", source)
        self.assertNotIn("startup/loading.gif", cmake)
        self.assertIn("qh_startup_leaf(startup_overlay)", source)
        self.assertNotIn("qh_logo(startup_overlay", source)
        self.assertIn("qh_screen(startup_overlay)", source)
        self.assertIn("lv_obj_delete(startup_overlay)", source)
        logo = (MAIN / "qinghe_logo.c").read_text(encoding="utf-8")
        self.assertIn("LV_COLOR_FORMAT_RGB565A8", logo)
        self.assertIn(".w=64,.h=64", logo)
        self.assertNotIn("lv_label_set_text(startup_percent_label", source)

    def test_leaf_bitmap_matches_selected_qinghe_prototype(self):
        # Approved design fixture, independent of the optional untracked HTML preview.
        pattern = "".join(["000010000", "000111000", "011010110",
                           "111010111", "011010110", "000010000",
                           "000010000", "001111100", "000010000"])
        source = (MAIN / "qinghe_ui.c").read_text(encoding="utf-8")
        packed = source.split("startup_leaf_bits[11] = {", 1)[1].split("};", 1)[0]
        data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", packed))
        self.assertEqual(len(pattern), 81)
        self.assertEqual(len(data), 11)
        self.assertEqual("".join(str((data[i // 8] >> (i % 8)) & 1) for i in range(81)), pattern)



if __name__ == "__main__":
    unittest.main()
