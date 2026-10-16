#!/usr/bin/env python3
"""range_server.py - 支持 HTTP Range 的最小静态服务器（206 Partial Content）

用途：作业演示的“服务端”。Python 3.10 自带的 `python3 -m http.server`
至今不支持 Range（任何 GET 都回 200 + 整个文件），拿它当数据源时下载器
只能退化成“整文件流式跳过”，既慢、也看不出 Range 断点请求的价值。

用法：
    python3 range_server.py <目录> <端口>
    # 例： python3 range_server.py tests/work/demo 8099
"""
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RANGE_RE = re.compile(r"bytes=(\d*)-(\d*)")


class RangeHandler(BaseHTTPRequestHandler):
    server_version = "RangeServer/1.0"
    protocol_version = "HTTP/1.1"
    root = "."

    def _resolve(self):
        path = self.path.split("?", 1)[0]
        path = os.path.normpath(os.path.join(self.root, path.lstrip("/")))
        if not path.startswith(os.path.abspath(self.root)):
            return None
        return path if os.path.isfile(path) else None

    def do_HEAD(self):
        self.do_GET(head_only=True)

    def do_GET(self, head_only=False):
        fpath = self._resolve()
        if not fpath:
            self.send_error(404, "Not Found")
            return

        size = os.path.getsize(fpath)
        rng = self.headers.get("Range")
        start, end = 0, size - 1
        partial = False

        if rng:
            m = RANGE_RE.fullmatch(rng.strip())
            if m:
                s, e = m.group(1), m.group(2)
                if s == "" and e:                 # bytes=-N 末尾 N 字节
                    start = max(0, size - int(e))
                elif s:
                    start = int(s)
                    end = int(e) if e else size - 1
                if start >= size or start > end:  # 越界 -> 416
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                end = min(end, size - 1)
                partial = True

        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if head_only:
            return

        with open(fpath, "rb") as f:
            f.seek(start)
            left = length
            while left > 0:
                chunk = f.read(min(65536, left))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                left -= len(chunk)

    def log_message(self, fmt, *args):
        if os.environ.get("RANGE_SERVER_QUIET") != "1":
            sys.stderr.write("[range-server] %s\n" % (fmt % args))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8099
    RangeHandler.root = os.path.abspath(root)
    httpd = ThreadingHTTPServer(("127.0.0.1", port), RangeHandler)
    sys.stderr.write(f"[range-server] serving {RangeHandler.root} on "
                     f"http://127.0.0.1:{port}/ (Range enabled)\n")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
