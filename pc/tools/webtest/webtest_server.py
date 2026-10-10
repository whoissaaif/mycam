#!/usr/bin/env python3
"""webtest_server.py: step 1 of the iPhone web-page idea.

Serves index.html over HTTPS with a self-signed certificate, so an iPhone on the same
Wi-Fi can open it in Safari and answer three questions:
  1. Does Safari allow the camera on a self-signed page?
  2. Is the WebCodecs H.264 VideoEncoder available, and at what sizes / frame rates?
  3. Roughly how fast does it encode at 1080p30 and 1080p60?

The page POSTs its results to /report; they are printed here and saved in ./results/.

Run:  python pc\\tools\\webtest\\webtest_server.py
Needs Python 3.8+ and openssl (comes with Git for Windows). Nothing to pip install.
"""
import http.server
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import time

PORT = 47900
HERE = os.path.dirname(os.path.abspath(__file__))
CERT_DIR = os.path.join(HERE, "cert")
RESULTS_DIR = os.path.join(HERE, "results")


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
        if self.path.split("?")[0] in ("/", "/index.html"):
            with open(os.path.join(HERE, "index.html"), "rb") as f:
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


def main():
    ip = lan_ip()
    cert, key = ensure_cert(ip)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print("MyCam web test server")
    print("  On the iPhone (same Wi-Fi), open Safari and go to:")
    print("      https://%s:%d/" % (ip, PORT))
    print("  Safari will warn the certificate is not private: that is expected.")
    print("  Ctrl+C to quit.\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("bye")


if __name__ == "__main__":
    main()
