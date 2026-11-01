"""Execute the production local transcript model, without ASR or cloud calls."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/p4_main/main"
TCC = ROOT / "logs/chat-host-tools/tcc/tcc.exe"


class Model(ctypes.Structure):
    _fields_ = [("text", ctypes.c_void_p), ("capacity", ctypes.c_size_t),
                ("length", ctypes.c_size_t), ("turn_start", ctypes.c_size_t)]


class TranscriptTurnsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TCC.exists():
            raise unittest.SkipTest("Optional host TCC unavailable")
        cls.temp = tempfile.TemporaryDirectory(dir=ROOT / "logs")
        cls.addClassCleanup(cls.temp.cleanup)
        dll = Path(cls.temp.name) / "turns.dll"
        wrapper = Path(cls.temp.name) / "exports.c"
        wrapper.write_text('''#include "transcript_turns.c"
__declspec(dllexport) void probe_init(transcript_turns_t *m, char *s, size_t n) { transcript_turns_init(m, s, n); }
__declspec(dllexport) bool probe_update(transcript_turns_t *m, const char *s) { return transcript_turns_update(m, s); }
__declspec(dllexport) bool probe_append(transcript_turns_t *m, const char *s) { return transcript_turns_append(m, s); }
__declspec(dllexport) void probe_begin(transcript_turns_t *m) { transcript_turns_begin(m); }
__declspec(dllexport) void probe_commit(transcript_turns_t *m) { transcript_turns_commit(m); }
''', encoding="ascii")
        result = subprocess.run([str(TCC), "-shared", "-I" + str(MAIN),
                                 str(wrapper), "-o", str(dll)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        cls.lib = ctypes.CDLL(str(dll))
        for name in ["init", "update", "append", "begin", "commit"]:
            setattr(cls.lib, "transcript_turns_" + name, getattr(cls.lib, "probe_" + name))
        free = ctypes.WinDLL("kernel32", use_last_error=True).FreeLibrary
        free.argtypes = [ctypes.c_void_p]
        cls.addClassCleanup(free, cls.lib._handle)
        cls.lib.transcript_turns_init.argtypes = [ctypes.POINTER(Model), ctypes.c_void_p, ctypes.c_size_t]
        for name in ["update", "append"]:
            fn = getattr(cls.lib, "transcript_turns_" + name)
            fn.argtypes = [ctypes.POINTER(Model), ctypes.c_char_p]
            fn.restype = ctypes.c_bool
        for name in ["begin", "commit"]:
            getattr(cls.lib, "transcript_turns_" + name).argtypes = [ctypes.POINTER(Model)]

    def setUp(self):
        self.storage = ctypes.create_string_buffer(128)
        self.model = Model()
        self.lib.transcript_turns_init(ctypes.byref(self.model), self.storage, len(self.storage))

    def append(self, text):
        return self.lib.transcript_turns_append(ctypes.byref(self.model), text.encode())

    def update(self, text):
        return self.lib.transcript_turns_update(ctypes.byref(self.model), text.encode())

    def test_each_typed_message_has_its_own_paragraph(self):
        self.assertTrue(self.append("你好"))
        self.assertTrue(self.append("谢谢"))
        self.assertEqual(self.storage.value.decode(), "你好\n谢谢")

    def test_recording_hypotheses_replace_only_current_turn(self):
        self.append("上一条")
        self.lib.transcript_turns_begin(ctypes.byref(self.model))
        self.assertTrue(self.update("你"))
        self.assertTrue(self.update("你好"))
        self.assertEqual(self.storage.value.decode(), "上一条\n你好")
        self.lib.transcript_turns_commit(ctypes.byref(self.model))
        self.assertTrue(self.append("再见"))
        self.assertEqual(self.storage.value.decode(), "上一条\n你好\n再见")

    def test_empty_recording_and_shortened_hypothesis(self):
        self.append("已有")
        self.lib.transcript_turns_begin(ctypes.byref(self.model))
        self.update("更长的临时文字")
        self.update("短")
        self.assertEqual(self.storage.value.decode(), "已有\n短")
        self.update("")
        self.assertEqual(self.storage.value.decode(), "已有")

    def test_capacity_failure_never_erases_previous_text_or_splits_utf8(self):
        self.append("已保存")
        before = self.storage.value
        self.assertFalse(self.append("字" * 100))
        self.assertEqual(self.storage.value, before)
        self.lib.transcript_turns_begin(ctypes.byref(self.model))
        self.update("临时")
        before = self.storage.value
        self.assertFalse(self.update("字" * 100))
        self.assertEqual(self.storage.value, before)

    def test_clear_resets_the_next_turn_boundary(self):
        self.append("之前")
        self.lib.transcript_turns_init(ctypes.byref(self.model), self.storage, len(self.storage))
        self.assertTrue(self.append("之后"))
        self.assertEqual(self.storage.value.decode(), "之后")
