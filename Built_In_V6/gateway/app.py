"""Development-only tool gateway. Uses Python's standard library; no packages are installed.

Run from this directory: python app.py --port 8787
Do not expose this HTTP development server to the public Internet. Put HTTPS,
authentication, an API-backed weather provider, and a search provider in front
of it before field use.
"""
from __future__ import annotations

import argparse
import json
import os
import time
from collections import defaultdict, deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

CACHE: dict[tuple[str, str], tuple[float, dict]] = {}
REQUESTS: dict[str, deque[float]] = defaultdict(deque)
MAX_BODY = 2048


def load_local_env() -> None:
    """Load only project-local secrets; never print their values."""
    path = Path(__file__).parents[1] / "secrets" / "qianfan.env"
    if not path.exists():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip())


def qianfan_answer(query: str) -> dict:
    key = os.environ.get("QIANFAN_API_KEY", "")
    if not key:
        return {"ok": False, "answer": "千帆 API Key 尚未配置。", "source": "gateway"}
    payload = json.dumps({
        "model": os.environ.get("QIANFAN_MODEL", "ernie-4.5-turbo-128k"),
        "messages": [{"role": "system", "content": "用不超过80个汉字的简洁中文回答。"},
                     {"role": "user", "content": query}],
        "temperature": 0.3,
    }).encode("utf-8")
    request = Request("https://qianfan.baidubce.com/v2/chat/completions", data=payload,
                      headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
                      method="POST")
    try:
        with urlopen(request, timeout=4) as response:
            data = json.loads(response.read(2048))
        text = data["choices"][0]["message"]["content"].strip()
        return {"ok": bool(text), "answer": text[:512], "source": "qianfan"}
    except (HTTPError, URLError, TimeoutError, KeyError, ValueError):
        return {"ok": False, "answer": "联网问答暂时不可用，请稍后重试。", "source": "gateway"}


def reply(tool: str, query: str) -> dict:
    if tool == "llm":
        return qianfan_answer(query)
    if tool == "weather":
        return {"ok": False, "answer": "天气服务尚未配置 API 提供商。", "source": "gateway"}
    if tool == "search":
        return {"ok": False, "answer": "联网搜索服务尚未配置。", "source": "gateway"}
    return {"ok": False, "answer": "不支持的联网工具。", "source": "gateway"}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:
        if self.path != "/v1/tool/query":
            self.send_error(404); return
        device_token = os.environ.get("GATEWAY_DEVICE_TOKEN", "")
        if device_token and self.headers.get("X-Device-Token") != device_token:
            self._json({"ok": False, "answer": "设备鉴权失败。", "source": "gateway"}, 401); return
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_BODY:
            self.send_error(413); return
        ip = self.client_address[0]
        now = time.monotonic()
        window = REQUESTS[ip]
        while window and now - window[0] > 60: window.popleft()
        if len(window) >= 6:
            self._json({"ok": False, "answer": "请求过于频繁，请稍后再试。", "source": "gateway"}, 429); return
        window.append(now)
        try:
            data = json.loads(self.rfile.read(length))
            tool, query = data["tool"], str(data["query"]).strip()
            if len(query) > 256: raise ValueError
        except (ValueError, KeyError, json.JSONDecodeError):
            self._json({"ok": False, "answer": "联网请求格式无效。", "source": "gateway"}, 400); return
        key = (tool, query)
        cached = CACHE.get(key)
        if cached and now - cached[0] < 300:
            result = {**cached[1], "cached": True}
        else:
            result = {**reply(tool, query), "cached": False}
            CACHE[key] = (now, result)
        self._json(result, 200)

    def _json(self, payload: dict, status: int) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status); self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    def log_message(self, *_: object) -> None: pass


if __name__ == "__main__":
    load_local_env()
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--host", default="127.0.0.1",
                        help="Use 0.0.0.0 only behind HTTPS and device-token authentication.")
    args = parser.parse_args(); ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
