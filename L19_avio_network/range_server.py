#!/usr/bin/env python3
"""支持 Range 的最小 HTTP 文件服务器，只用来做 L19 的实验。

libavformat 内置的 HTTP 服务器（官方示例 avio_http_serve_files.c、本讲的 avio_serve.c 用的就是它）不支持 Range，
客户端没办法“从第 N 个字节开始取”。要看清楚“服务器支持 Range”和“不支持”对客户端意味着什么，需要一个对照：
Python 自带的 http.server 也不支持 Range（它总是回整个文件），所以这里在它的基础上补上。

用法：
    range_server.py 目录 端口 [--no-range]

    --no-range   不支持 Range：不发 Accept-Ranges，请求里的 Range 头也不理，总是回 200 和整个文件
                 （和 libavformat 内置的服务器一样，用来对照）

支持的 Range 写法：bytes=A-B、bytes=A-、bytes=-N（单个范围）。
每个请求在 stderr 打印一行：方法、路径、请求的 Range、回复的状态码、发送的字节数。
"""
import os
import re
import sys
from functools import partial
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class Handler(SimpleHTTPRequestHandler):
    ranges_enabled = True

    def log_message(self, fmt, *args):      # 默认的日志格式不带 Range，改成自己的 log_request
        pass

    def do_GET(self):
        self.serve(send_body=True)

    def do_HEAD(self):
        self.serve(send_body=False)

    def serve(self, send_body):
        path = self.translate_path(self.path)
        if os.path.isdir(path):             # 目录：交给基类（列目录），不支持 Range
            return SimpleHTTPRequestHandler.do_GET(self) if send_body else SimpleHTTPRequestHandler.do_HEAD(self)
        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND, "File not found")
            self.report(404, 0)
            return
        with f:
            size = os.fstat(f.fileno()).st_size
            rng = self.headers.get("Range") if self.ranges_enabled else None
            start, end, code = 0, size - 1, 200
            if rng:
                m = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
                if not m or (m.group(1) == "" and m.group(2) == ""):
                    rng = None              # 看不懂的 Range 头按规矩忽略，回整个文件
                else:
                    if m.group(1) == "":    # bytes=-N：最后 N 个字节
                        start = max(0, size - int(m.group(2)))
                    else:
                        start = int(m.group(1))
                        if m.group(2) != "":
                            end = min(size - 1, int(m.group(2)))
                    if start >= size or start > end:
                        self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                        self.send_header("Content-Range", "bytes */%d" % size)
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        self.report(416, 0)
                        return
                    code = 206
            self.send_response(code)
            self.send_header("Content-Type", self.guess_type(path))
            if self.ranges_enabled:
                self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(end - start + 1))
            if code == 206:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
            self.end_headers()
            sent = 0
            if send_body:
                f.seek(start)
                left = end - start + 1
                try:
                    while left > 0:
                        chunk = f.read(min(65536, left))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        sent += len(chunk)
                        left -= len(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    pass
            self.report(code, sent, rng)

    def report(self, code, sent, rng=None):
        sys.stderr.write("%s %s Range=%s -> %d %d\n" % (self.command, self.path, rng or "-", code, sent))
        sys.stderr.flush()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    Handler.ranges_enabled = "--no-range" not in sys.argv
    handler = partial(Handler, directory=args[0])
    ThreadingHTTPServer(("127.0.0.1", int(args[1])), handler).serve_forever()


if __name__ == "__main__":
    main()
