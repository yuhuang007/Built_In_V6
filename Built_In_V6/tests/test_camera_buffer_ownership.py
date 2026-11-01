"""Execute camera callbacks with deterministic queues and a slow consumer."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_portrait_layout import function, TCC, ROOT, MAIN


class CameraOwnershipTests(unittest.TestCase):
    def test_tone_curve_is_bounded_monotonic_and_lifts_midtones(self):
        if not TCC.exists():
            self.skipTest("Optional host TCC unavailable")
        source = (MAIN / "sign_language_mode.c").read_text(encoding="utf-8")
        code = "#include <stdint.h>\n" + function(source, "static uint32_t preview_tone_curve(")
        code += function(source, "static uint32_t preview_blue_curve(")
        code += "\n__declspec(dllexport) uint32_t tone(uint32_t x) { return preview_tone_curve(x); }"
        code += "\n__declspec(dllexport) uint32_t blue(uint32_t x) { return preview_blue_curve(x); }"
        with tempfile.TemporaryDirectory(dir=ROOT / "logs") as directory:
            work = Path(directory)
            (work / "tone.c").write_text(code, encoding="utf-8")
            result = subprocess.run([str(TCC), "-shared", str(work / "tone.c"),
                                     "-o", str(work / "tone.dll")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lib = ctypes.CDLL(str(work / "tone.dll"))
            try:
                values = [lib.tone(x) for x in range(256)]
                self.assertEqual((values[0], values[255]), (0, 255))
                self.assertEqual(values, sorted(values))
                self.assertTrue(all(x <= y <= 255 for x, y in enumerate(values)))
                self.assertEqual([values[x] for x in (64, 128, 192)], [128, 192, 230])
                self.assertLessEqual(max(y - x for x, y in enumerate(values)), 69)
                blue = [lib.blue(x) for x in range(256)]
                for channel in (blue,):
                    self.assertEqual((channel[0], channel[255]), (0, 255))
                    self.assertEqual(channel, sorted(channel))
                    self.assertTrue(all(0 <= y <= 255 for y in channel))
                self.assertTrue(all(b <= g for b, g in zip(blue, values)))
                self.assertLessEqual(max(g - b for g, b in zip(values, blue)), 5)
                self.assertNotIn("preview_red_curve", source)
                config = function(source, "static esp_err_t configure_preview_tone(")
                self.assertLess(config.index("COLOR_COMPONENT_R"),
                                config.index("preview_blue_curve"))
                self.assertLess(config.index("COLOR_COMPONENT_G"),
                                config.index("preview_blue_curve"))
            finally:
                free = ctypes.windll.kernel32.FreeLibrary
                free.argtypes = [ctypes.c_void_p]
                free(lib._handle)

    def test_isr_never_recycles_processing_frame_or_loses_buffers(self):
        if not TCC.exists():
            self.skipTest("Optional host TCC unavailable")
        source = (MAIN / "sign_language_mode.c").read_text(encoding="utf-8")
        stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define CAPTURE_BUFFER_COUNT 3
#define SENSOR_FRAME_BYTES 4096
#define pdFALSE 0
#define pdTRUE 1
typedef int BaseType_t;
typedef void *esp_cam_ctlr_handle_t;
typedef struct { void *buffer; unsigned buflen; } esp_cam_ctlr_trans_t;
typedef struct { void *items[3]; int count, capacity; } queue_t;
typedef queue_t *QueueHandle_t;
int xQueueReceiveFromISR(queue_t *q, void *out, int *wake) {
    if (!q->count) return 0;
    *(void **)out = q->items[0];
    memmove(q->items, q->items + 1, --q->count * sizeof(void *));
    return 1;
}
int xQueueSendFromISR(queue_t *q, const void *item, int *wake) {
    if (q->count == q->capacity) return 0;
    q->items[q->count++] = *(void *const *)item;
    return 1;
}
int xQueueOverwriteFromISR(queue_t *q, const void *item, int *wake) {
    assert(q->capacity == 1);
    q->items[0] = *(void *const *)item;
    q->count = 1;
    return 1;
}
'''
        end = source.index("} capture_context_t;") + len("} capture_context_t;")
        start = source.rindex("typedef struct {", 0, end)
        stub += source[start:end]
        stub += function(source, "static bool camera_get_new_buffer(")
        stub += function(source, "static bool camera_frame_finished(")
        stub += r'''
__declspec(dllexport) int ownership_case(void) {
    int buffers[3], wake = 0, errors = 0;
    queue_t free_q = {{&buffers[0], &buffers[1], &buffers[2]}, 3, 3};
    queue_t done_q = {{0}, 0, 1};
    capture_context_t c = {0};
    c.free_frames = &free_q;
    c.completed_frames = &done_q;
    esp_cam_ctlr_trans_t current = {0};
    camera_get_new_buffer(0, &current, &c);
    void *processing = 0;
    for (int frame = 0; frame < 1000; frame++) {
        esp_cam_ctlr_trans_t next = {0};
        /* Real CSI asks for the next destination BEFORE publishing current. */
        camera_get_new_buffer(0, &next, &c);
        if (!next.buffer || next.buffer == current.buffer ||
            (processing && next.buffer == processing)) errors++;
        camera_frame_finished(0, &current, &c);
        current = next;
        if (!processing) xQueueReceiveFromISR(&done_q, &processing, &wake);
        /* Hold an input frame across many capture interrupts. */
        if (processing && frame % 13 == 12) {
            if (!xQueueSendFromISR(&free_q, &processing, &wake)) errors++;
            processing = 0;
        }
        void *owned[3]; int count = 0;
        owned[count++] = current.buffer;
        if (processing) owned[count++] = processing;
        for (int i = 0; i < free_q.count; i++) {
            if (count >= 3) return 100;
            owned[count++] = free_q.items[i];
        }
        for (int i = 0; i < done_q.count; i++) {
            if (count >= 3) return 101;
            owned[count++] = done_q.items[i];
        }
        if (count != 3) errors++;
        for (int i = 0; i < count; i++)
            for (int j = i + 1; j < count; j++)
                if (owned[i] == owned[j]) errors++;
    }
    if (!c.dropped_frame_count) errors++;
    return errors;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / "logs") as directory:
            work = Path(directory)
            (work / "ownership.c").write_text(stub, encoding="utf-8")
            result = subprocess.run([str(TCC), "-shared", str(work / "ownership.c"),
                                     "-o", str(work / "ownership.dll")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lib = ctypes.CDLL(str(work / "ownership.dll"))
            try:
                self.assertEqual(lib.ownership_case(), 0)
            finally:
                free = ctypes.windll.kernel32.FreeLibrary
                free.argtypes = [ctypes.c_void_p]
                free(lib._handle)


if __name__ == "__main__":
    unittest.main()
