#!/usr/bin/env python3
"""上传测试服务端（配合 apps/curl-verify/curl-verify -u）

协议：客户端先发一行 "PUT <size>\\n"，随后发 size 字节原始数据；
服务端收满后回 "OK <sha256>\\n"（不足 size 字节回 "ERR short <sha256>\\n"）。

用法: python3 tests/upload-server.py [PORT] [BIND_ADDR]
默认: 8081, 0.0.0.0
"""

import hashlib
import socketserver
import sys

CHUNK = 65536


class UploadHandler(socketserver.BaseRequestHandler):
    def handle(self):
        f = self.request.makefile("rb")
        line = f.readline().decode(errors="replace").strip()
        if not line.startswith("PUT "):
            self.request.sendall(b"ERR bad header\n")
            print(f"{self.client_address[0]} bad header: {line!r}", flush=True)
            return

        size = int(line.split()[1])
        h = hashlib.sha256()
        remaining = size
        while remaining > 0:
            data = f.read(min(CHUNK, remaining))
            if not data:
                break
            h.update(data)
            remaining -= len(data)

        digest = h.hexdigest()
        status = "OK" if remaining == 0 else "ERR short"
        self.request.sendall(f"{status} {digest}\n".encode())
        print(f"{self.client_address[0]} PUT {size} {status} {digest}", flush=True)


class UploadServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8081
    addr = sys.argv[2] if len(sys.argv) > 2 else "0.0.0.0"
    print(f"upload-server listening on {addr}:{port}", flush=True)
    UploadServer((addr, port), UploadHandler).serve_forever()
