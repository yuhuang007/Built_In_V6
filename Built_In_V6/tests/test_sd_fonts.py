"""Actual binary coverage/hash and font-only provisioning route tests."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from inspect_sd_font import inspect_font
import sd_font_server


class SdFontTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((ROOT / "assets/fonts/sd-fonts-v2-manifest.json").read_text())
        cls.fonts = [inspect_font(ROOT / "assets/fonts" / file["name"]) for file in cls.manifest["files"]]

    def test_same_real_coverage(self):
        self.assertEqual(set(self.fonts[0]["mapping"]), set(self.fonts[1]["mapping"]))
        self.assertEqual(len(self.fonts[0]["mapping"]), self.manifest["glyph_count"])

    def test_required_glyphs_present(self):
        for font in self.fonts:
            self.assertTrue(set(self.manifest["required"]).issubset(font["mapping"]))
            self.assertTrue(set(range(0x20, 0x7f)).issubset(font["mapping"]))

    def test_native_sizes_and_hashes(self):
        for entry, font in zip(self.manifest["files"], self.fonts):
            data = (ROOT / "assets/fonts" / entry["name"]).read_bytes()
            self.assertEqual(font["size_px"], entry["size_px"])
            self.assertEqual(font["bpp"], 4)
            self.assertEqual(font["compression"], 0)
            self.assertEqual(len(data), entry["bytes"])
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"])
            self.assertLessEqual(len(Path(entry["name"]).stem), 8)

    def test_compact_input_fits_actual_large_font_line_metrics(self):
        font = next(f for f in self.fonts if f["size_px"] == 28)
        line_height = font["ascent"] - font["descent"]
        import re
        source = (ROOT / "firmware/p4_main/main/text_entry.c").read_text(encoding="utf-8")
        height = int(re.search(r"lv_obj_set_size\(input, 432, (\d+)\)", source).group(1))
        self.assertGreaterEqual(height - 2, line_height)
        self.assertIn("lv_obj_set_style_pad_top(input, 0, LV_PART_MAIN)", source)
        self.assertIn("lv_obj_set_style_pad_bottom(input, 0, LV_PART_MAIN)", source)
        self.assertIn("lv_obj_set_size(language_label, 52, owner->font->line_height)", source)

    def test_homepage_punctuation_has_real_v2_glyphs(self):
        old = inspect_font(ROOT / "assets/fonts/p4x_zh_full_14.bin")
        small = self.fonts[0]
        self.assertEqual(small["size_px"], 14)
        for cp in (0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2026):
            with self.subTest(codepoint=cp):
                self.assertNotIn(cp, old["mapping"])
                self.assertIn(cp, small["mapping"])

    def test_home_borrows_only_published_font_and_rebinds_after_preload(self):
        mode = (ROOT / "firmware/p4_main/main/speech_to_text_mode.c").read_text(encoding="utf-8")
        accessor = mode[mode.index("const lv_font_t *speech_to_text_mode_font14"):
                        mode.index("speech_fonts_stage_t speech_to_text_mode_fonts_stage")]
        self.assertIn("atomic_load(&fonts_ready) ? native_font_14 : NULL", accessor)
        home = (ROOT / "firmware/p4_main/main/main.c").read_text(encoding="utf-8")
        selector = home[home.index("static const lv_font_t *project_ui_font"):
                        home.index("static void apply_project_font")]
        self.assertIn("speech_to_text_mode_font14()", selector)
        self.assertIn("&lv_font_project_zh", selector)
        boot = home[home.index("static void startup_resource_task"):
                    home.index("static void startup_status_timer")]
        self.assertLess(boot.index("speech_to_text_mode_preload_fonts()"),
                        boot.index("refresh_ui_font_from_sdcard()"))
        self.assertIn("error == ESP_OK && bsp_display_lock(0)", boot)
        self.assertNotIn("p4_binfont_destroy", boot)
        self.assertNotIn("p4_font_load(", boot)

    def test_only_font_files_and_bounded_ranges(self):
        path, offset, length = sd_font_server.resolve_request("/sd-font-v2?font=P4Z14V2.BIN&offset=0&length=16384")
        self.assertEqual(path.name, "P4Z14V2.BIN")
        for query in ["font=../../secrets/x&offset=0&length=1",
                      "font=P4Z14V2.BIN&offset=-1&length=1",
                      "font=P4Z14V2.BIN&offset=0&length=16385",
                      "font=P4Z14V2.BIN&offset=0&length=1&extra=x"]:
            with self.assertRaises(ValueError):
                sd_font_server.resolve_request("/sd-font-v2?" + query)

    def test_persistent_verification_fast_path(self):
        source = (ROOT / "firmware/p4_main/main/sd_font_upgrade.c").read_text(encoding="utf-8")
        provision = source[source.index("static esp_err_t provision"):]
        self.assertLess(provision.index("previously_verified(target, asset)"),
                        provision.index("verify_existing(target, asset"))
        self.assertLess(provision.index("return ESP_OK;"),
                        provision.index("if (progress) progress("))
        self.assertIn('nvs_get_blob(handle, receipt_key(asset)', source)
        self.assertIn('nvs_commit(handle)', source)
        self.assertIn('info.st_size != asset->bytes', source)
        self.assertIn('receipt->modified = info.st_mtime', source)
        self.assertIn('memcpy(receipt->hash, asset->hash', source)
        self.assertIn('if (error == ESP_OK) remember_verified(target, asset);', source)
        ensure = source[source.index("esp_err_t sd_font_upgrade_ensure"):]
        self.assertNotIn('progress(completed', ensure)


if __name__ == "__main__":
    unittest.main()
