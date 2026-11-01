"""Exercise the production C codec on the host; no device/chat content needed."""
import ctypes
import pathlib
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
TCC = ROOT / 'logs/chat-host-tools/tcc/tcc.exe'

class ChatStoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TCC.exists():
            raise unittest.SkipTest('Optional E-drive host TCC unavailable')
        cls.temp = tempfile.TemporaryDirectory(dir=ROOT / 'logs')
        cls.addClassCleanup(cls.temp.cleanup)
        work = pathlib.Path(cls.temp.name)
        source = ROOT / 'firmware/p4_main/main/chat_store_codec.c'
        policy = ROOT / 'firmware/p4_main/main/screen_idle_policy.c'
        result = subprocess.run([str(TCC), '-DCHAT_HOST_TEST', '-shared', str(source), str(policy), '-o', str(work / 'codec.dll')],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError('Host codec compile failed: ' + result.stderr)
        cls.lib = ctypes.CDLL(str(work / 'codec.dll'))
        free = ctypes.windll.kernel32.FreeLibrary
        free.argtypes = [ctypes.c_void_p]
        cls.addClassCleanup(free, cls.lib._handle)
        cls.lib.chat_crc_update.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.chat_crc_update.restype = ctypes.c_uint32
        cls.lib.chat_header_encode.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
        cls.lib.chat_header_decode.argtypes = [ctypes.c_void_p, *[ctypes.POINTER(ctypes.c_uint32)] * 3]
        cls.lib.chat_header_decode.restype = ctypes.c_bool
        cls.lib.chat_record_encode.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint32]
        cls.lib.chat_record_decode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint),
                                             ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint32)]
        cls.lib.chat_record_decode.restype = ctypes.c_bool
        cls.lib.chat_utf8_copy.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p]
        cls.lib.chat_utf8_copy.restype = ctypes.c_size_t
        cls.lib.chat_stream_update.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_int, ctypes.c_bool]
        cls.lib.chat_stream_update.restype = ctypes.c_bool
        cls.lib.screen_idle_startup_can_arm.argtypes = [ctypes.c_bool] * 4 + [ctypes.c_uint32] * 2
        cls.lib.screen_idle_startup_can_arm.restype = ctypes.c_bool
        cls.lib.screen_idle_welcome_pcm_finished.argtypes = [ctypes.c_bool] * 2 + [ctypes.c_uint32]
        cls.lib.screen_idle_welcome_pcm_finished.restype = ctypes.c_bool

    def test_screen_idle_missing_welcome_events_require_real_playback_and_quiet(self):
        finished = self.lib.screen_idle_welcome_pcm_finished
        self.assertFalse(finished(False, True, 60000))
        self.assertFalse(finished(True, False, 60000))
        self.assertFalse(finished(True, True, 4999))
        self.assertTrue(finished(True, True, 5000))
        ready = self.lib.screen_idle_startup_can_arm
        self.assertFalse(ready(False, finished(True, True, 5000), False, False, 10000, 5000))
        self.assertTrue(ready(True, finished(True, True, 5000), False, False, 10000, 5000))

    def test_screen_idle_waits_for_home_welcome_and_playback_tail(self):
        ready = self.lib.screen_idle_startup_can_arm
        self.assertFalse(ready(False, True, False, False, 60000, 5000))
        self.assertFalse(ready(True, False, False, False, 60000, 5000))
        self.assertFalse(ready(True, True, False, False, 10000, 0))
        self.assertFalse(ready(True, True, False, False, 10000, 1499))
        self.assertTrue(ready(True, True, False, False, 10000, 1500))

    def test_screen_idle_offline_fallback_and_deliberate_user_turn(self):
        ready = self.lib.screen_idle_startup_can_arm
        self.assertFalse(ready(True, False, False, True, 29999, 5000))
        self.assertTrue(ready(True, False, False, True, 30000, 5000))
        self.assertFalse(ready(True, False, False, False, 30000, 5000))
        self.assertTrue(ready(True, False, True, False, 1000, 0))
        self.assertFalse(ready(False, False, True, True, 60000, 5000))

    def test_crc_matches_standard_and_incremental_chunks(self):
        data = b'123456789'
        whole = self.lib.chat_crc_update(0xffffffff, data, len(data)) ^ 0xffffffff
        first = self.lib.chat_crc_update(0xffffffff, data[:3], 3)
        chunked = self.lib.chat_crc_update(first, data[3:], 6) ^ 0xffffffff
        self.assertEqual(whole, zlib.crc32(data))
        self.assertEqual(chunked, whole)

    def test_header_version_count_and_generation_validation(self):
        out = ctypes.create_string_buffer(20)
        generation, count, crc = (ctypes.c_uint32() for _ in range(3))
        for n, g, expected in [(0, 1, True), (200, 123, True), (201, 1, False), (1, 0, False)]:
            self.lib.chat_header_encode(out, g, n, 42)
            self.assertEqual(self.lib.chat_header_decode(out, ctypes.byref(generation),
                             ctypes.byref(count), ctypes.byref(crc)), expected)
            self.assertEqual(bytes(out.raw), b'P4CHAT01' + struct.pack('<III', g, n, 42))
        out[0] = b'X'
        self.assertFalse(self.lib.chat_header_decode(out, ctypes.byref(generation), ctypes.byref(count), ctypes.byref(crc)))

    def test_record_rejects_oversize_roles_empty_and_reserved_bits(self):
        out = ctypes.create_string_buffer(8)
        role, length, turn = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint32()
        for r, n, expected in [(0, 1, True), (1, 4095, True), (2, 20, True),
                               (3, 20, False), (0, 0, False), (0, 4096, False)]:
            self.lib.chat_record_encode(out, r, n, 19)
            self.assertEqual(self.lib.chat_record_decode(out, ctypes.byref(role), ctypes.byref(length), ctypes.byref(turn)), expected)
        out[1] = b'\x01'
        self.assertFalse(self.lib.chat_record_decode(out, ctypes.byref(role), ctypes.byref(length), ctypes.byref(turn)))

    def test_utf8_copy_never_splits_multibyte_text(self):
        original = 'A测试🙂B'.encode()
        for capacity in range(1, len(original) + 3):
            out = ctypes.create_string_buffer(capacity)
            copied = self.lib.chat_utf8_copy(out, capacity, original)
            self.assertLess(copied, capacity)
            self.assertTrue(original.startswith(out.value))
            out.value.decode('utf-8', errors='strict')

    def test_invalid_utf8_rejected_including_overlong_and_surrogates(self):
        for value in (b'\xc0\xaf', b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'\xe4\xb8', b'\x80'):
            out = ctypes.create_string_buffer(64)
            self.assertEqual(self.lib.chat_utf8_copy(out, 64, value), 0)

    def test_empty_cleared_snapshot_has_valid_crc(self):
        out = ctypes.create_string_buffer(20)
        self.lib.chat_header_encode(out, 15, 0, 0)
        crc = self.lib.chat_crc_update(0xffffffff, out, 16) ^ 0xffffffff
        self.lib.chat_header_encode(out, 15, 0, crc)
        self.assertEqual(struct.unpack('<I', out.raw[16:])[0], zlib.crc32(out.raw[:16]))

    def test_mutation_of_role_turn_or_text_is_detected(self):
        header, record = ctypes.create_string_buffer(20), ctypes.create_string_buffer(8)
        text = '离线测试'.encode()
        self.lib.chat_header_encode(header, 1, 1, 0)
        self.lib.chat_record_encode(record, 1, len(text), 2)
        content = header.raw[:16] + record.raw + text
        crc = self.lib.chat_crc_update(0xffffffff, content, len(content)) ^ 0xffffffff
        for index in (8, 12, 16, 20, len(content) - 1):
            changed = bytearray(content); changed[index] ^= 1
            self.assertNotEqual(self.lib.chat_crc_update(0xffffffff, bytes(changed), len(changed)) ^ 0xffffffff, crc)

    def test_stream_updates_coalesce_partial_duplicate_and_late_results(self):
        buffer = ctypes.create_string_buffer(64)
        prefix, sequence = ctypes.c_size_t(0), ctypes.c_int(-1)
        def update(text, seq):
            return self.lib.chat_stream_update(buffer, 64, ctypes.byref(prefix), ctypes.byref(sequence), text, seq, False)
        self.assertTrue(update(b'hel', 0))
        self.assertTrue(update(b'hello', 1))
        self.assertEqual(buffer.value, b'hello')
        self.assertFalse(update(b'hello', 1))
        self.assertTrue(update(b'world', 2))
        self.assertEqual(buffer.value, b'hello\nworld')
        self.assertFalse(update(b'old', 0))
        self.assertEqual(buffer.value, b'hello\nworld')
        self.assertTrue(update(b'world!', 2))
        self.assertEqual(buffer.value, b'hello\nworld!')

    def test_stream_without_sequence_replaces_hypothesis(self):
        buffer = ctypes.create_string_buffer(64)
        prefix, sequence = ctypes.c_size_t(0), ctypes.c_int(-1)
        for text in (b'one', b'one two', b'one two three'):
            self.lib.chat_stream_update(buffer, 64, ctypes.byref(prefix), ctypes.byref(sequence), text, -1, False)
        self.assertEqual(buffer.value, b'one two three')

    def test_user_asr_revisions_replace_punctuation_and_corrected_words(self):
        buffer = ctypes.create_string_buffer(256)
        prefix, sequence = ctypes.c_size_t(0), ctypes.c_int(-1)
        for seq, text in enumerate(('你好。你好', '你好，你好，背一首中学的', '你好。你好，背一首中学的诗。')):
            self.assertTrue(self.lib.chat_stream_update(buffer, 256, ctypes.byref(prefix),
                ctypes.byref(sequence), text.encode(), seq, True))
            self.assertEqual(buffer.value.decode(), text)
            self.assertNotIn(b'\n', buffer.value)
        final = buffer.value
        self.assertFalse(self.lib.chat_stream_update(buffer, 256, ctypes.byref(prefix),
            ctypes.byref(sequence), '旧结果'.encode(), 0, True))
        self.assertEqual(buffer.value, final)
        self.assertFalse(self.lib.chat_stream_update(buffer, 256, ctypes.byref(prefix),
            ctypes.byref(sequence), final, 4, True))

    def test_user_asr_can_replace_with_shorter_or_unrelated_hypothesis(self):
        buffer = ctypes.create_string_buffer(64)
        prefix, sequence = ctypes.c_size_t(0), ctypes.c_int(-1)
        for seq, text in enumerate(('识别出了很长的错误内容', '请背诗', '请讲故事。')):
            self.lib.chat_stream_update(buffer, 64, ctypes.byref(prefix),
                ctypes.byref(sequence), text.encode(), seq, True)
            self.assertEqual(buffer.value.decode(), text)

if __name__ == '__main__':
    unittest.main()
