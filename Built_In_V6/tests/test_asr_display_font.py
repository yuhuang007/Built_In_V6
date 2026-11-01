"""Offline coverage and bounded-font architecture guards; no credentials/SD writes."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/p4_main/main"


class FontTests(unittest.TestCase):
    def test_ascii_coverage(self):
        source = (MAIN / "font_asr_symbols.c").read_text(encoding="utf-8")
        glyphs = {int(value, 16) for value in re.findall(r"/\* U\+([0-9A-F]+)", source)}
        self.assertTrue(set(range(0x20, 0x7f)).issubset(glyphs))

    def test_common_punctuation_coverage(self):
        source = (MAIN / "font_asr_symbols.c").read_text(encoding="utf-8")
        glyphs = {int(value, 16) for value in re.findall(r"/\* U\+([0-9A-F]+)", source)}
        for symbol in '，。！？；：、…—–·“”‘’（）【】《》〈〉「」『』〔〕［］｛｝～％＋－＝／＼＠＃＆＊＄＾＿｀｜＜＞．＂＇':
            with self.subTest(symbol=symbol):
                self.assertIn(ord(symbol), glyphs)

    def test_no_whole_label_transform(self):
        source = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        self.assertNotIn("set_style_transform", source)
        self.assertIn("asr_display_font_destroy(transcript_font)", source)

    def test_cache_limits(self):
        source = (MAIN / "asr_display_font.c").read_text(encoding="utf-8")
        self.assertIn("#define CACHE_SLOTS 512", source)
        self.assertIn("#define CACHE_BYTE_LIMIT (768 * 1024)", source)
        self.assertIn("MALLOC_CAP_SPIRAM", source)
        self.assertIn("lv_draw_buf_flush_cache(output, NULL)", source)


if __name__ == "__main__":
    unittest.main()
