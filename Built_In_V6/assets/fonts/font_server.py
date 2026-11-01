"""Temporary LAN-only segmented server for P4 font provisioning."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from time import sleep
from urllib.parse import parse_qs, urlparse

FONT = Path(__file__).with_name("p4x_zh_full_14.bin")


class FontHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        query = parse_qs(urlparse(self.path).query)
        try:
            offset = int(query.get("offset", ["0"])[0])
            length = int(query.get("length", ["0"])[0])
        except ValueError:
            self.send_error(400)
            return
        size = FONT.stat().st_size
        if offset < 0 or length <= 0 or offset + length > size:
            self.send_error(416)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.end_headers()
        with FONT.open("rb") as source:
            source.seek(offset)
            remaining = length
            while remaining:
                block = source.read(min(1024, remaining))
                self.wfile.write(block)
                self.wfile.flush()
                remaining -= len(block)
                sleep(0.003)

    def log_message(self, fmt, *args):
        print(fmt % args, flush=True)


if __name__ == "__main__":
    ThreadingHTTPServer(("0.0.0.0", 8123), FontHandler).serve_forever()
