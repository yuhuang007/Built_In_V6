"""Offline binary framing tests. No credentials or network calls."""
import gzip
import importlib.util
import json
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("asr_probe", Path(__file__).resolve().parents[1] / "tools/doubao_asr_probe.py")
protocol = importlib.util.module_from_spec(spec)
spec.loader.exec_module(protocol)


class ProtocolTests(unittest.TestCase):
    def test_firmware_handshake_capacity(self):
        config = (Path(__file__).resolve().parents[1] /
                  "firmware/p4_main/sdkconfig.sign").read_text(encoding="utf-8")
        import re
        capacity = int(re.search(r"^CONFIG_WS_BUFFER_SIZE=(\d+)$", config, re.M).group(1))
        self.assertGreaterEqual(capacity, 4096)

    def test_configuration(self):
        packet = protocol.pack_request(1, b'{"audio":{"rate":16000}}')
        self.assertEqual(packet[:4], bytes.fromhex("11111100"))
        sequence, size = struct.unpack_from(">iI", packet, 4)
        self.assertEqual(sequence, 1)
        self.assertEqual(size, len(packet) - 12)
        self.assertEqual(gzip.decompress(packet[12:]), b'{"audio":{"rate":16000}}')

    def test_last_audio(self):
        packet = protocol.pack_request(5, bytes(6400), audio=True, last=True)
        self.assertEqual(packet[:4], bytes.fromhex("11231100"))
        self.assertEqual(struct.unpack_from(">i", packet, 4)[0], -5)
        self.assertEqual(gzip.decompress(packet[12:]), bytes(6400))

    def test_final_json(self):
        payload = gzip.compress(json.dumps({"result": {"text": "test"}}).encode())
        packet = bytes.fromhex("11931100") + struct.pack(">iI", -5, len(payload)) + payload
        error, last, body = protocol.parse_response(packet)
        self.assertEqual(error, 0)
        self.assertTrue(last)
        self.assertEqual(body["result"]["text"], "test")

    def test_error_and_bad_size(self):
        payload = gzip.compress(b'{"message":"withheld"}')
        packet = bytes.fromhex("11f01100") + struct.pack(">II", 45000000, len(payload)) + payload
        self.assertEqual(protocol.parse_response(packet)[0], 45000000)
        with self.assertRaises(ValueError):
            protocol.parse_response(packet[:-1])


if __name__ == "__main__":
    unittest.main()
