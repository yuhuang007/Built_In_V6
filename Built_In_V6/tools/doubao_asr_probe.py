"""Small, bounded ASR connectivity probe. No credential or transcript output.

Uses only the project Python standard library. Sends 0.6 seconds of zero PCM,
never accesses a microphone and never writes a recording or response log.
"""
import argparse
import base64
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import ssl
import struct
import time
import uuid

HOST = "openspeech.bytedance.com"
PATH = "/api/v3/sauc/bigmodel_async"
CAPACITY = 65536


def pack_request(sequence, payload, audio=False, last=False):
    payload = gzip.compress(payload)
    return bytes([0x11, (0x20 if audio else 0x10) | (3 if last else 1), 0x11, 0]) + \
        struct.pack(">iI", -sequence if last else sequence, len(payload)) + payload


def parse_response(data):
    if len(data) < 4 or data[0] >> 4 != 1 or data[0] & 15 == 0:
        raise ValueError("invalid protocol header")
    flags = data[1] & 15
    offset = (data[0] & 15) * 4 + (4 if flags & 1 else 0) + (4 if flags & 4 else 0)
    error = 0
    if data[1] >> 4 == 15:
        error = struct.unpack_from(">I", data, offset)[0]
        offset += 4
    elif data[1] >> 4 != 9:
        raise ValueError("unexpected message type")
    size = struct.unpack_from(">I", data, offset)[0]
    payload = data[offset + 4:]
    if size != len(payload):
        raise ValueError("payload size mismatch")
    if data[2] & 15 == 1:
        with gzip.GzipFile(fileobj=__import__("io").BytesIO(payload)) as stream:
            payload = stream.read(131073)
        if len(payload) > 131072:
            raise ValueError("expanded response exceeds limit")
    elif data[2] & 15 != 0:
        raise ValueError("unsupported compression")
    body = json.loads(payload) if payload and data[2] >> 4 == 1 else {}
    return error, bool(flags & 2), body


def exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise ConnectionError("closed")
        data.extend(chunk)
    return bytes(data)


def send_frame(connection, payload, opcode=2):
    mask = os.urandom(4)
    header = bytes([0x80 | opcode])
    if len(payload) < 126:
        header += bytes([0x80 | len(payload)])
    else:
        header += bytes([0x80 | 126]) + struct.pack(">H", len(payload))
    connection.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def receive_frame(connection):
    a, b = exact(connection, 2)
    if b & 0x80:
        raise ValueError("server frame unexpectedly masked")
    size = b & 127
    if size == 126:
        size = struct.unpack(">H", exact(connection, 2))[0]
    elif size == 127:
        size = struct.unpack(">Q", exact(connection, 8))[0]
    if size > CAPACITY:
        raise ValueError("oversized frame")
    payload = exact(connection, size)
    opcode = a & 15
    if opcode == 9:
        send_frame(connection, payload, 10)
        return None
    if opcode != 2 or not a & 0x80:
        raise ValueError("expected unfragmented binary response")
    return payload


def probe():
    header_path = Path(__file__).resolve().parents[1] / "secrets" / "doubao_asr.h"
    credentials = header_path.read_text(encoding="utf-8")
    key = re.search(r'#define\s+DOUBAO_ASR_API_KEY\s+"([^"]+)"', credentials).group(1)
    resource = re.search(r'#define\s+DOUBAO_ASR_RESOURCE_ID\s+"([^"]+)"', credentials).group(1)
    ws_key = base64.b64encode(os.urandom(16)).decode()
    # No request headers, HTTP bodies, exception strings or transcripts printed.
    with socket.create_connection((HOST, 443), timeout=10) as tcp:
        with ssl.create_default_context().wrap_socket(tcp, server_hostname=HOST) as connection:
            connection.settimeout(10)
            request = f"GET {PATH} HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" \
                f"Sec-WebSocket-Key: {ws_key}\r\nSec-WebSocket-Version: 13\r\n" \
                f"X-Api-Key: {key}\r\nX-Api-Resource-Id: {resource}\r\nX-Api-Request-Id: {uuid.uuid4()}\r\n\r\n"
            connection.sendall(request.encode())
            response = bytearray()
            while not response.endswith(b"\r\n\r\n"):
                response.extend(exact(connection, 1))
                if len(response) > 16384:
                    raise ValueError("oversized handshake")
            status = int(response.split(b" ")[1])
            print(f"http_status={status} handshake_bytes={len(response)}", flush=True)
            if status != 101:
                return False
            accept = base64.b64encode(hashlib.sha1((ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
            if b"sec-websocket-accept: " + accept.lower() not in bytes(response).lower():
                raise ValueError("invalid WebSocket accept")
            config = {"user": {"uid": "p4-asr-probe"},
                "audio": {"format": "pcm", "codec": "raw", "rate": 16000, "bits": 16, "channel": 1},
                "request": {"model_name": "bigmodel", "result_type": "full", "enable_nonstream": True}}
            send_frame(connection, pack_request(1, json.dumps(config).encode()))
            for seq in range(2, 5):
                send_frame(connection, pack_request(seq, bytes(6400), audio=True, last=seq == 4))
                time.sleep(0.2)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                data = receive_frame(connection)
                if data is None:
                    continue
                error, last, body = parse_response(data)
                print(f"protocol_error={error} final={int(last)}", flush=True)
                if error or last:
                    return error == 0 and last
            return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", action="store_true", help="send 0.6 seconds of zero PCM to ASR")
    args = parser.parse_args()
    if not args.probe:
        parser.error("explicit --probe is required")
    try:
        raise SystemExit(0 if probe() else 1)
    except Exception as error:
        print(f"probe_failed type={type(error).__name__}")
        raise SystemExit(1)
