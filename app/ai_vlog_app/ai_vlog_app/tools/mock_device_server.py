"""模拟开发板的 HTTP 服务，用于联调 App 的"接收 Vlog"功能。

提供两个接口（与 App 约定一致）：
  GET /list                  -> [{"name": "...", "size": 123}]
  GET /download?name=xxx.mp4 -> 该 mp4 文件流

用法：
  python tools/mock_device_server.py [视频目录] [端口]
  默认视频目录用 firmware 里的示例 mp4；默认端口 8080。
  启动后 App 的"设备地址"填：<本机局域网IP>:8080
"""
import os, sys, json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
# 默认拿 firmware 里那个示例 mp4 所在目录
DEFAULT_DIR = os.path.join(
    HERE, "..", "..", "firmware", "managed_components", "lvgl__lvgl",
    "examples", "libs", "ffmpeg")

VIDEO_DIR = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath(DEFAULT_DIR)
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080


def list_videos():
    items = []
    if os.path.isdir(VIDEO_DIR):
        for f in os.listdir(VIDEO_DIR):
            if f.lower().endswith(".mp4"):
                items.append({"name": f, "size": os.path.getsize(os.path.join(VIDEO_DIR, f))})
    return items


class Handler(BaseHTTPRequestHandler):
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/list":
            body = json.dumps(list_videos()).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self._cors()
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif parsed.path == "/download":
            name = parse_qs(parsed.query).get("name", [""])[0]
            path = os.path.join(VIDEO_DIR, os.path.basename(name))
            if not name or not os.path.isfile(path):
                self.send_response(404)
                self.end_headers()
                return
            size = os.path.getsize(path)
            self.send_response(200)
            self.send_header("Content-Type", "video/mp4")
            self._cors()
            self.send_header("Content-Length", str(size))
            self.end_headers()
            with open(path, "rb") as fp:
                while True:
                    chunk = fp.read(64 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, fmt, *args):
        print("[mock-device]", fmt % args)


if __name__ == "__main__":
    print(f"视频目录: {VIDEO_DIR}")
    print(f"可下载: {[v['name'] for v in list_videos()]}")
    print(f"监听 0.0.0.0:{PORT}  (App 设备地址填: <本机IP>:{PORT})")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
