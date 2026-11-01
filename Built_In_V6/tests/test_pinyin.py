import hashlib
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import sd_font_server

class PinyinTests(unittest.TestCase):
    def test_dictionary_identity_matches_firmware_and_manifest(self):
        folder = ROOT / "assets/pinyin"
        entry = json.loads((folder / "pinyin-manifest.json").read_text())["files"][0]
        data = (folder / entry["name"]).read_bytes()
        self.assertEqual(len(data), entry["bytes"])
        self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"])
        header = (ROOT / "firmware/p4_main/main/pinyin_asset_meta.h").read_text()
        self.assertIn(str(len(data)) + "u", header)
        self.assertIn(entry["sha256"], header)

    def test_dictionary_transfer_is_allowlisted_and_bounded(self):
        folder = ROOT / "assets/pinyin"
        with patch.object(sd_font_server, "ASSETS", folder), patch.object(sd_font_server, "ALLOWED", {"P4PYV1.DAT"}):
            path, offset, length = sd_font_server.resolve_request("/sd-font-v2?font=P4PYV1.DAT&offset=0&length=16384")
            self.assertEqual(path, folder / "P4PYV1.DAT")
            self.assertEqual((offset, length), (0, 16384))
            for name in ["../history", "P4Z28V2.BIN", "secrets"]:
                with self.assertRaises(ValueError):
                    sd_font_server.resolve_request(f"/sd-font-v2?font={name}&offset=0&length=10")

    def test_provision_never_overwrites_existing_or_accepts_unverified(self):
        source = (ROOT / "firmware/p4_main/main/pinyin_asset.c").read_text()
        self.assertIn('fopen(temporary, "wbx")', source)
        self.assertIn("if (error != ESP_ERR_NOT_FOUND) return error", source)
        self.assertIn("digest_matches(digest)", source)
        self.assertIn("access(PINYIN_ASSET_PATH, F_OK)", source)
        self.assertNotIn('fopen(PINYIN_ASSET_PATH, "w', source)

    def test_no_personal_learning_or_ui_thread_decoding(self):
        component = ROOT / "firmware/p4_main/components/p4_pinyin"
        self.assertIn("P4_PINYIN_READ_ONLY=1", (component / "CMakeLists.txt").read_text())
        ui = (ROOT / "firmware/p4_main/main/text_entry.c").read_text(encoding="utf-8")
        self.assertNotIn("p4_pinyin_query(", ui)
        self.assertIn("if (spelling[0]) { choose_candidate(0); return; }", ui)
        service = (ROOT / "firmware/p4_main/main/pinyin_service.c").read_text()
        self.assertIn("xQueueOverwrite", service)
        self.assertNotIn("lv_", service)
        self.assertNotIn("%s", service)
        self.assertIn("deferred_keys[deferred_count++] = key", ui)
        self.assertIn("pinyin_service_state() == PINYIN_READY", ui)
        main = (ROOT / "firmware/p4_main/main/main.c").read_text(encoding="utf-8")
        self.assertLess(main.index("pinyin_service_preload()"), main.index("atomic_store(&startup_fonts_done, true)"))
        self.assertIn('request_work(0, "", 0, -1, false)', service)

    def test_actual_32bit_decoder_and_production_wrapper(self):
        probe = ROOT / "logs/pinyin-host/probe.exe"
        if not probe.exists():
            self.skipTest("Run Build-PinyinAssets.ps1 with the optional Zig host compiler first")
        result = subprocess.run([str(probe), str(ROOT / "assets/pinyin/P4PYV1.DAT")],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("expected_present=1"), 6)
        self.assertIn("production_wrapper_cases=6", result.stdout)

    def test_editor_copy_fallback_is_scoped_and_camera_path_unchanged(self):
        ui = (ROOT / "firmware/p4_main/main/text_entry.c").read_text(encoding="utf-8")
        display = (ROOT / "firmware/p4_main/main/portrait_display.c").read_text()
        self.assertIn("portrait_editor_cpu_copy(true)", ui)
        self.assertIn("portrait_editor_cpu_copy(false)", ui)
        self.assertIn("if ((editor_cpu_copy && y2 >= editor_copy_top) || width * height < 4096)", display)
        self.assertIn("portrait_editor_cpu_region(cpu_region_top)", ui)
        self.assertIn("if (popup_top < cpu_region_top) cpu_region_top = popup_top", ui)
        camera = display[display.index("esp_err_t portrait_preview_frame("):display.index("void portrait_preview_stop(")]
        self.assertNotIn("editor_cpu_copy", camera)
        self.assertIn("ppa_do_scale_rotate_mirror", camera)
