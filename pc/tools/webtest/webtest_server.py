#!/usr/bin/env python3
"""webtest_server.py: tests for the iPhone web-page idea.

Serves over HTTPS with a self-signed certificate, so an iPhone on the same Wi-Fi can open it in Safari.

  https://<pc-ip>:47900/          test 1: camera + WebCodecs H.264 encoder support and speed
  https://<pc-ip>:47900/wstest    test 2: send encoded frames to this PC over wss:// and measure delay

The pages POST their results to /report; they are printed here and saved in ./results/.
Test 2 also prints a receive summary on this side when the WebSocket closes.

Run:  python pc\\tools\\webtest\\webtest_server.py
Needs Python 3.8+ and openssl (comes with Git for Windows). Nothing to pip install.
"""
import base64
import hashlib
import http.server
import json
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import time

PORT = 47900
HERE = os.path.dirname(os.path.abspath(__file__))
CERT_DIR = os.path.join(HERE, "cert")
RESULTS_DIR = os.path.join(HERE, "results")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PAGES = {"/": "index.html", "/index.html": "index.html", "/wstest": "wstest.html", "/wstest.html": "wstest.html"}


def lan_ip():
    """The PC's address on the local network (no packets are actually sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def find_openssl():
    found = shutil.which("openssl")
    if found:
        return found
    for candidate in (r"C:\Program Files\Git\usr\bin\openssl.exe",
                      r"C:\Program Files (x86)\Git\usr\bin\openssl.exe"):
        if os.path.exists(candidate):
            return candidate
    return None


def ensure_cert(ip):
    """Make (or reuse) a self-signed certificate that is valid for this PC's IP address."""
    os.makedirs(CERT_DIR, exist_ok=True)
    cert = os.path.join(CERT_DIR, "cert.pem")
    key = os.path.join(CERT_DIR, "key.pem")
    stamp = os.path.join(CERT_DIR, "ip.txt")
    if os.path.exists(cert) and os.path.exists(key) and os.path.exists(stamp):
        with open(stamp) as f:
            if f.read().strip() == ip:
                return cert, key
    openssl = find_openssl()
    if not openssl:
        sys.exit("openssl not found. Install Git for Windows (it includes openssl) or add openssl to PATH.")
    print("making a self-signed certificate for %s ..." % ip)
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    cmd = [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
           "-keyout", key, "-out", cert, "-days", "30",
           "-subj", "/CN=MyCam web test",
           "-addext", "subjectAltName=IP:%s,DNS:localhost" % ip]
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("openssl failed:\n" + result.stderr)
    with open(stamp, "w") as f:
        f.write(ip)
    return cert, key


# ---- minimal WebSocket (RFC 6455): enough for binary messages, ping/pong and close ----

def recv_exact(rfile, n):
    data = rfile.read(n)
    if data is None or len(data) < n:
        raise EOFError("connection closed")
    return data


def ws_read_frame(rfile):
    b1, b2 = recv_exact(rfile, 2)
    opcode = b1 & 0x0F
    length = b2 & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(rfile, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(rfile, 8))[0]
    if length > 8_000_000:
        raise ValueError("frame too big")
    mask = recv_exact(rfile, 4) if (b2 & 0x80) else None
    payload = recv_exact(rfile, length) if length else b""
    if mask and length:
        m = (mask * (length // 4 + 1))[:length]
        payload = (int.from_bytes(payload, "big") ^ int.from_bytes(m, "big")).to_bytes(length, "big")
    return opcode, payload


def ws_frame(opcode, payload=b""):
    n = len(payload)
    if n < 126:
        head = bytes([0x80 | opcode, n])
    elif n < 65536:
        head = bytes([0x80 | opcode, 126]) + struct.pack(">H", n)
    else:
        head = bytes([0x80 | opcode, 127]) + struct.pack(">Q", n)
    return head + payload


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, int(p * len(s)))]


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "MyCamWebTest"

    def log_message(self, fmt, *args):
        print("%s  %s  %s" % (time.strftime("%H:%M:%S"), self.client_address[0], fmt % args))

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/ws":
            self._websocket()
        elif path in PAGES:
            with open(os.path.join(HERE, PAGES[path]), "rb") as f:
                self._send(200, f.read(), "text/html; charset=utf-8")
        else:
            self._send(404, "not found")

    def do_POST(self):
        if self.path != "/report":
            self._send(404, "not found")
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(min(length, 1_000_000))
        try:
            report = json.loads(raw)
        except ValueError:
            self._send(400, "bad json")
            return
        os.makedirs(RESULTS_DIR, exist_ok=True)
        path = os.path.join(RESULTS_DIR, time.strftime("results-%Y%m%d-%H%M%S.json"))
        with open(path, "w") as f:
            json.dump(report, f, indent=2)
        print("\n===== results from the phone =====")
        print(json.dumps(report, indent=2))
        print("saved to %s\n" % path)
        self._send(200, "ok")

    def _websocket(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key or self.headers.get("Upgrade", "").lower() != "websocket":
            self._send(400, "expected a websocket upgrade")
            return
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        try:
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        print("%s  websocket open from %s" % (time.strftime("%H:%M:%S"), self.client_address[0]))

        video_msgs = 0
        video_bytes = 0
        first = last = None
        gaps = []
        try:
            while True:
                opcode, payload = ws_read_frame(self.rfile)
                now = time.perf_counter()
                if opcode == 0x2:  # binary
                    if len(payload) >= 12:
                        # Echo seq (4 bytes) + client timestamp (8 bytes) so the phone can time the round trip.
                        self.wfile.write(ws_frame(0x2, payload[:12]))
                    if len(payload) > 64:  # ignore the tiny idle pings; count video frames
                        video_msgs += 1
                        video_bytes += len(payload)
                        if last is not None:
                            gaps.append((now - last) * 1000.0)
                        if first is None:
                            first = now
                        last = now
                elif opcode == 0x8:  # close
                    self.wfile.write(ws_frame(0x8))
                    break
                elif opcode == 0x9:  # ping
                    self.wfile.write(ws_frame(0xA, payload))
        except (EOFError, ConnectionError, OSError, ssl.SSLError, ValueError):
            pass
        finally:
            if video_msgs and first is not None and last is not None and last > first:
                span = last - first
                print("%s  websocket closed. received %d video messages, %.1f MB, %.1f Mbps over %.1f s; "
                      "gap between messages avg %.1f ms, p95 %.1f ms, max %.1f ms" % (
                          time.strftime("%H:%M:%S"), video_msgs, video_bytes / 1e6,
                          video_bytes * 8 / span / 1e6, span,
                          sum(gaps) / len(gaps), pct(gaps, 0.95), max(gaps)))
            else:
                print("%s  websocket closed" % time.strftime("%H:%M:%S"))


def main():
    ip = lan_ip()
    cert, key = ensure_cert(ip)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print("MyCam web test server")
    print("  On the iPhone (same Wi-Fi), open Safari and go to:")
    print("      https://%s:%d/          (test 1: encoder)" % (ip, PORT))
    print("      https://%s:%d/wstest    (test 2: send frames to the PC)" % (ip, PORT))
    print("  Safari will warn the certificate is not private: that is expected.")
    print("  Ctrl+C to quit.\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("bye")


if __name__ == "__main__":
    main()
