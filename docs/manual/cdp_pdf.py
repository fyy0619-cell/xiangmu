# -*- coding: utf-8 -*-
"""Print an HTML file to PDF with a clean centered page-number footer only,
using the Edge/Chromium DevTools Protocol over a stdlib WebSocket client.
No third-party packages.
"""
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.request

EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
HERE = os.path.dirname(os.path.abspath(__file__))
SRC_URL = "file:///" + os.path.join(HERE, "thesis.html").replace("\\", "/")
OUT = os.path.join(HERE, "thesis.pdf")
PORT = 9333

FOOTER = ('<div style="font-size:9px;width:100%;text-align:center;color:#666;'
          'font-family:Microsoft YaHei,sans-serif;">'
          '第 <span class="pageNumber"></span> 页 / 共 <span class="totalPages"></span> 页</div>')
HEADER = '<div></div>'


def ws_connect(url):
    hostport, path = url[len("ws://"):].split("/", 1)
    host, port = hostport.split(":")
    s = socket.create_connection((host, int(port)), timeout=30)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
           "Sec-WebSocket-Version: 13\r\nOrigin: http://%s\r\n\r\n"
           % (path, hostport, key, hostport))
    s.sendall(req.encode())
    data = b""
    while b"\r\n\r\n" not in data:
        data += s.recv(4096)
    if b"101" not in data.split(b"\r\n")[0]:
        raise RuntimeError("WS handshake failed: " + data.split(b"\r\n")[0].decode())
    return s


def ws_send(s, obj):
    payload = json.dumps(obj).encode()
    ln = len(payload)
    header = b"\x81"
    mask = os.urandom(4)
    if ln < 126:
        header += struct.pack("B", 0x80 | ln)
    elif ln < 65536:
        header += struct.pack("B", 0x80 | 126) + struct.pack(">H", ln)
    else:
        header += struct.pack("B", 0x80 | 127) + struct.pack(">Q", ln)
    s.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def ws_recv(s):
    def readn(n):
        b = b""
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise IOError("socket closed")
            b += c
        return b
    out = b""
    while True:
        h = readn(2)
        fin = h[0] & 0x80
        ln = h[1] & 0x7f
        if ln == 126:
            ln = struct.unpack(">H", readn(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", readn(8))[0]
        out += readn(ln)
        if fin:
            break
    return json.loads(out.decode())


def main():
    proc = subprocess.Popen([
        EDGE, "--headless=new", "--disable-gpu", "--no-first-run",
        "--remote-allow-origins=*",
        "--remote-debugging-port=%d" % PORT,
        "--user-data-dir=%s" % os.path.join(HERE, ".edgeprofile"),
        SRC_URL,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wsurl = None
        for _ in range(60):
            try:
                j = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json/list" % PORT, timeout=2))
                for t in j:
                    if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                        wsurl = t["webSocketDebuggerUrl"]
                        break
                if wsurl:
                    break
            except Exception:
                pass
            time.sleep(0.5)
        if not wsurl:
            raise RuntimeError("no debuggable page target")

        s = ws_connect(wsurl)
        ws_send(s, {"id": 1, "method": "Page.enable"})
        ws_send(s, {"id": 2, "method": "Page.navigate", "params": {"url": SRC_URL}})
        # wait for load event
        loaded = False
        t0 = time.time()
        while time.time() - t0 < 30:
            m = ws_recv(s)
            if m.get("method") == "Page.loadEventFired":
                loaded = True
                break
        time.sleep(1.5)  # settle layout/fonts

        ws_send(s, {"id": 3, "method": "Page.printToPDF", "params": {
            "printBackground": True,
            "displayHeaderFooter": True,
            "headerTemplate": HEADER,
            "footerTemplate": FOOTER,
            "paperWidth": 8.27, "paperHeight": 11.69,
            "marginTop": 0.55, "marginBottom": 0.7,
            "marginLeft": 0.5, "marginRight": 0.5,
            "preferCSSPageSize": False,
        }})
        data = None
        while True:
            m = ws_recv(s)
            if m.get("id") == 3:
                if "error" in m:
                    raise RuntimeError("printToPDF error: %s" % m["error"])
                data = m["result"]["data"]
                break
        with open(OUT, "wb") as f:
            f.write(base64.b64decode(data))
        print("clean PDF written:", OUT, os.path.getsize(OUT), "bytes (loaded=%s)" % loaded)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
