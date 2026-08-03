"""relay.py - self-hosted signaling mailbox for webrtc_api.dll.
   20 lines. Any server that does this replaces the Cloudflare worker.

   Usage:  python relay.py [port]    (default 8000)
"""
from http.server import BaseHTTPRequestHandler, HTTPServer
import json, threading

box = {}
lock = threading.Lock()

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def do_POST(self):
        code = self.path.split("code=")[1]
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode() if n else "{}"
        with lock:
            box.setdefault(code, []).append(body)
        self.send_response(200); self.end_headers()

    def do_GET(self):
        code = self.path.split("code=")[1]
        with lock:
            msgs = box.pop(code, [])
        data = json.dumps({"ok": True, "msgs": msgs}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

if __name__ == "__main__":
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print(f"relay on http://localhost:{port}")
    HTTPServer(("", port), Handler).serve_forever()
