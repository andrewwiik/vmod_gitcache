#!/usr/bin/env python3
import base64, gzip, json, os, re, subprocess, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = sys.argv[1]
PORT = int(sys.argv[2])
PRIVATE = {"corp/secret": {"alice"}}
TOKENS = {"alice": "t-alice", "bob": "t-bob", "carol": "t-carol"}
stats, lock = {}, threading.Lock()

def count(k):
    with lock:
        stats[k] = stats.get(k, 0) + 1

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass

    def send(self, code, body=b"", ctype="text/plain", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra: self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def user(self):
        a = self.headers.get("Authorization", "")
        if a.startswith("Basic "):
            u, _, p = base64.b64decode(a[6:]).decode().partition(":")
            if TOKENS.get(u) == p: return u
        return None

    def static(self, repo, rel):
        fp = os.path.realpath(f"{ROOT}/{repo}.git/{rel}")
        if not fp.startswith(os.path.realpath(ROOT)) or not os.path.isfile(fp):
            return self.send(404)
        count("dumb-file")
        with open(fp, "rb") as f: data = f.read()
        self.send(200, data, "application/octet-stream")

    def repo(self, path):
        for suf in ("/info/refs", "/git-upload-pack", "/git-receive-pack"):
            if path.endswith(suf):
                return path[:-len(suf)].strip("/").removesuffix(".git").lower(), suf
        return None, None

    def authorize(self, repo):
        allowed = PRIVATE.get(repo)
        if allowed is None: return True
        u = self.user()
        if u in allowed: return True
        self.send(401, b"auth required", extra=[("WWW-Authenticate", 'Basic realm="ghes"')])
        count("denied")
        return False

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/_stats":
            with lock: return self.send(200, json.dumps(stats).encode(), "application/json")
        m = re.match(r"^/(dumb/[^/]+?)(?:\.git)?/(.+)$", u.path)
        if m: return self.static(m.group(1), m.group(2))
        repo, suf = self.repo(u.path)
        if suf != "/info/refs": return self.send(404)
        if not self.authorize(repo): return
        svc = parse_qs(u.query).get("service", [""])[0]
        if svc != "git-upload-pack": return self.send(403, b"push disabled")
        count("info-refs")
        env = dict(os.environ, GIT_PROTOCOL=self.headers.get("Git-Protocol", ""))
        out = subprocess.run(["git", "upload-pack", "--stateless-rpc", "--advertise-refs",
                              f"{ROOT}/{repo}.git"], capture_output=True, env=env).stdout
        if "version=2" not in env["GIT_PROTOCOL"]:
            out = b"001e# service=git-upload-pack\n0000" + out
        self.send(200, out, "application/x-git-upload-pack-advertisement",
                  [("Cache-Control", "no-cache")])

    def read_body(self):
        if "chunked" in self.headers.get("Transfer-Encoding", ""):
            count("chunked-request")
            out = b""
            while True:
                size = int(self.rfile.readline().strip().split(b";")[0], 16)
                if size == 0:
                    self.rfile.readline(); return out
                out += self.rfile.read(size); self.rfile.readline()
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def do_POST(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        if u.path == "/_reset":
            with lock: stats.clear()
            return self.send(200)
        if u.path == "/_private":                       # make a repo private, alice only
            PRIVATE[q["repo"][0]] = {"alice"}; return self.send(200)
        if u.path == "/_public":
            PRIVATE.pop(q["repo"][0], None); return self.send(200)
        if u.path == "/_grant":
            PRIVATE.setdefault(q["repo"][0], set()).add(q["user"][0]); return self.send(200)
        body = self.read_body()
        repo, suf = self.repo(urlparse(self.path).path)
        if suf != "/git-upload-pack": return self.send(404)
        if not self.authorize(repo): return
        enc = self.headers.get("Content-Encoding", "")
        if enc == "gzip":
            count("gzip-request"); body = gzip.decompress(body)
        if body == b"0000": count("probe")
        count("upload-pack")
        env = dict(os.environ, GIT_PROTOCOL=self.headers.get("Git-Protocol", ""))
        out = subprocess.run(["git", "upload-pack", "--stateless-rpc", f"{ROOT}/{repo}.git"],
                             input=body, capture_output=True, env=env).stdout
        self.send(200, out, "application/x-git-upload-pack-result", [("Cache-Control", "no-cache")])

ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
