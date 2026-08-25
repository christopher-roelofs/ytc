#!/usr/bin/env python3
# POC HLS proxy: relays a YouTube HLS manifest + its segments through THIS machine
# (which holds the IP-locked googlevideo URLs) so a Cast device on a different IP
# can play them. Rewrites every playlist/segment URL to point back at the proxy.
#
#   ./hls_proxy.py <master-m3u8-url> <this-machine-lan-ip> [port]
# then cast  http://<lan-ip>:<port>/master.m3u8  (content-type application/x-mpegURL)
import sys, re, urllib.request, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MASTER = sys.argv[1]
IP     = sys.argv[2]
PORT   = int(sys.argv[3]) if len(sys.argv) > 3 else 8899
BASE   = f"http://{IP}:{PORT}"
UA     = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/131 Safari/537.36"

def fetch(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=20)

def rewrite(text):
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#EXT-X-MEDIA") and 'URI="' in s:
            line = re.sub(r'URI="([^"]+)"',
                          lambda m: 'URI="%s/p?u=%s"' % (BASE, urllib.parse.quote(m.group(1), safe="")),
                          line)
            out.append(line)
        elif s and not s.startswith("#"):
            out.append("%s/p?u=%s" % (BASE, urllib.parse.quote(s, safe="")))
        else:
            out.append(line)
    return ("\n".join(out) + "\n").encode()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, body, ctype):
        self.send_response(200); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass
    def do_GET(self):
        p = urllib.parse.urlparse(self.path)
        try:
            if p.path == "/master.m3u8":
                data = fetch(MASTER).read().decode("utf-8", "replace")
                print("[proxy] master -> rewritten"); self._send(rewrite(data), "application/x-mpegURL"); return
            if p.path == "/p":
                u = urllib.parse.parse_qs(p.query)["u"][0]
                r = fetch(u); body = r.read()
                if body[:7] == b"#EXTM3U":
                    print("[proxy] media playlist -> rewritten"); self._send(rewrite(body.decode("utf-8","replace")), "application/x-mpegURL"); return
                ct = r.headers.get("Content-Type") or "video/mp2t"
                print(f"[proxy] segment {len(body)}B {ct}"); self._send(body, ct); return
        except Exception as e:
            print("[proxy] ERROR", e); self.send_response(502); self.end_headers(); return
        self.send_response(404); self.end_headers()

print(f"proxy on {BASE}/master.m3u8")
ThreadingHTTPServer(("0.0.0.0", PORT), H).serve_forever()
