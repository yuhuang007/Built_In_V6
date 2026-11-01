"""Bounded font-only LAN provisioning server; no key, audio or request logs."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
from pathlib import Path
import re
import time
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets/fonts"
ALLOWED = {"P4Z14V2.BIN", "P4Z28V2.BIN"}


def resolve_request(request):
    query = parse_qs(urlparse(request).query, strict_parsing=True)
    if set(query) != {"font", "offset", "length"} or any(len(v) != 1 for v in query.values()):
        raise ValueError("invalid fields")
    name = query["font"][0]
    if name not in ALLOWED:
        raise ValueError("invalid asset")
    offset, length = int(query["offset"][0]), int(query["length"][0])
    size = (ASSETS / name).stat().st_size
    if offset < 0 or length <= 0 or length > 16384 or offset + length > size:
        raise ValueError("invalid range")
    return ASSETS / name, offset, length


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            if urlparse(self.path).path != "/sd-font-v2":
                raise ValueError("invalid path")
            source, offset, length = resolve_request(self.path)
        except (ValueError, OSError):
            self.send_error(400)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.end_headers()
        try:
            with source.open("rb") as font:
                font.seek(offset)
                remaining = length
                while remaining:
                    block = font.read(min(2048, remaining))
                    if not block:
                        break
                    self.wfile.write(block)
                    self.wfile.flush()
                    remaining -= len(block)
                    time.sleep(0.003)
            self.server.segments += 1
            if self.server.segments % 100 == 0:
                print(f"served_segments={self.server.segments}", flush=True)
        except (ConnectionError, OSError):
            print("segment_interrupted=1", flush=True)

    def log_message(self, *args):
        pass


def serve(minutes, manifest_name="sd-fonts-v2-manifest.json"):
    manifest = json.loads((ASSETS / manifest_name).read_text())
    for entry in manifest["files"]:
        if entry["name"] not in ALLOWED:
            raise ValueError("invalid manifest")
        data = (ASSETS / entry["name"]).read_bytes()
        if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError("asset verification failed")
    header = (ROOT / "secrets/sd_font_transfer.h").read_text()
    url = urlparse(re.search(r'#define\s+SD_FONT_TRANSFER_URL\s+"([^"]+)"', header).group(1))
    if (url.scheme != "http" or not ipaddress.ip_address(url.hostname).is_private or
            url.username or url.password or url.query or url.path != "/sd-font-v2"):
        raise ValueError("invalid LAN configuration")
    with ThreadingHTTPServer((url.hostname, url.port), Handler) as server:
        server.timeout = 0.5
        server.segments = 0
        deadline = time.monotonic() + minutes * 60
        print(f"font_server_ready=1 lifetime_minutes={minutes}", flush=True)
        while time.monotonic() < deadline:
            server.handle_request()
    print("font_server_closed=1", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--minutes", type=int, default=10)
    args = parser.parse_args()
    if not args.serve or not 1 <= args.minutes <= 30:
        parser.error("explicit --serve and lifetime 1..30 minutes required")
    try:
        serve(args.minutes)
    except KeyboardInterrupt:
        print("font_server_closed=1", flush=True)
    except Exception as error:
        print(f"font_server_failed type={type(error).__name__}", flush=True)
        raise SystemExit(1)
