#!/usr/bin/env python3
"""Live view-only stream of a QEMU VM's screen over HTTP.

Grabs PNG screenshots through QEMU's QMP socket a few times per second and
serves them with a small web page that keeps refreshing the picture. Nothing
here can send input to the VM, so the page is safe to expose publicly.

usage: stream_server.py --dir DIR [--port 8080]
DIR must contain qmp.sock (QEMU -qmp unix:DIR/qmp.sock,server=on,wait=off) and
may contain status.json written by autopilot.py."""
import argparse
import json
import os
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FRAME_INTERVAL = 0.3

PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fresh OS live</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; background: #0b0e13; color: #cdd5df; font: 15px/1.5 system-ui, sans-serif; }
  header { padding: 14px 16px 6px; max-width: 1100px; margin: 0 auto; }
  h1 { font-size: 20px; margin: 0; color: #7ee2a8; }
  p { margin: 4px 0; color: #9aa4b2; }
  main { max-width: 1100px; margin: 0 auto; padding: 8px 16px 24px; }
  .screen { width: 100%; aspect-ratio: 4 / 3; background: #0e1116; border: 1px solid #263040;
            border-radius: 8px; overflow: hidden; display: flex; align-items: center; justify-content: center; }
  .screen img { width: 100%; height: 100%; object-fit: contain; image-rendering: pixelated; }
  .bar { display: flex; flex-wrap: wrap; gap: 6px 18px; margin-top: 10px; font-size: 14px; }
  .dot { display: inline-block; width: 9px; height: 9px; border-radius: 50%; background: #e5534b; margin-right: 6px; }
  .dot.live { background: #57c785; }
  code { color: #cdd5df; }
</style></head>
<body>
<header>
  <h1>Fresh OS &mdash; live</h1>
  <p>A from-scratch operating system for PCs, booted in a virtual machine on a cloud server.
     This is a read-only view of its screen; an autopilot is typing demo commands.</p>
</header>
<main>
  <div class="screen"><img id="screen" alt="Fresh OS screen"></div>
  <div class="bar">
    <span><span id="dot" class="dot"></span><span id="state">connecting&hellip;</span></span>
    <span>Now: <code id="step">&ndash;</code></span>
    <span id="until"></span>
  </div>
</main>
<script>
const img = document.getElementById('screen');
const dot = document.getElementById('dot');
const state = document.getElementById('state');
let lastUrl = null, lastOk = 0;
async function frame() {
  try {
    const r = await fetch('frame.png?t=' + Date.now(), {cache: 'no-store'});
    if (!r.ok) throw new Error(r.status);
    const url = URL.createObjectURL(await r.blob());
    img.src = url;
    if (lastUrl) URL.revokeObjectURL(lastUrl);
    lastUrl = url;
    lastOk = Date.now();
    setTimeout(frame, 300);
  } catch (e) {
    setTimeout(frame, 2000);
  }
}
async function status() {
  try {
    const s = await (await fetch('status.json?t=' + Date.now(), {cache: 'no-store'})).json();
    document.getElementById('step').textContent = s.step || '-';
    if (s.until) {
      const t = new Date(s.until * 1000);
      document.getElementById('until').textContent = 'Stream ends around ' + t.toLocaleTimeString();
    }
  } catch (e) {}
  const live = Date.now() - lastOk < 5000;
  dot.className = 'dot' + (live ? ' live' : '');
  state.textContent = live ? 'live' : 'waiting for the VM';
}
frame();
status();
setInterval(status, 2000);
</script>
</body></html>
"""


class Grabber:
    def __init__(self, directory):
        self.dir = directory
        self.sock = None
        self.buf = b""
        self.frame = None
        self.frame_time = 0.0
        self.lock = threading.Lock()

    def _connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(os.path.join(self.dir, "qmp.sock"))
        self.sock, self.buf = s, b""
        self._read_msg()  # greeting
        self._cmd({"execute": "qmp_capabilities"})

    def _read_msg(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("QMP closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def _cmd(self, obj):
        self.sock.sendall(json.dumps(obj).encode() + b"\n")
        while True:
            msg = self._read_msg()
            if "return" in msg or "error" in msg:
                return msg

    def run(self):
        shot = os.path.join(self.dir, "shot.png")
        while True:
            try:
                if self.sock is None:
                    self._connect()
                r = self._cmd({"execute": "screendump", "arguments": {"filename": shot, "format": "png"}})
                if "return" in r:
                    with open(shot, "rb") as f:
                        data = f.read()
                    with self.lock:
                        self.frame, self.frame_time = data, time.time()
            except (OSError, ValueError, ConnectionError):
                if self.sock:
                    self.sock.close()
                self.sock = None
                time.sleep(1)
                continue
            time.sleep(FRAME_INTERVAL)


def make_handler(grabber, directory):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def _send(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path in ("/", "/index.html"):
                self._send(200, "text/html; charset=utf-8", PAGE.encode())
            elif path == "/frame.png":
                with grabber.lock:
                    frame = grabber.frame
                if frame is None:
                    self._send(503, "text/plain", b"no frame yet\n")
                else:
                    self._send(200, "image/png", frame)
            elif path == "/status.json":
                try:
                    with open(os.path.join(directory, "status.json"), "rb") as f:
                        body = f.read()
                except OSError:
                    body = b"{}"
                self._send(200, "application/json", body)
            elif path == "/health":
                with grabber.lock:
                    age = time.time() - grabber.frame_time if grabber.frame else None
                self._send(200, "application/json", json.dumps({"frame_age": age}).encode())
            else:
                self._send(404, "text/plain", b"not found\n")

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="127.0.0.1")
    a = ap.parse_args()
    g = Grabber(a.dir)
    threading.Thread(target=g.run, daemon=True).start()
    srv = ThreadingHTTPServer((a.bind, a.port), make_handler(g, a.dir))
    srv.daemon_threads = True
    srv.serve_forever()


if __name__ == "__main__":
    main()
