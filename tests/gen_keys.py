#!/usr/bin/env python3
CT = '-hdr "Content-Type: application/x-git-upload-pack-request" '
V2 = '-hdr "Git-Protocol: version=2" '

def pk(*lines):
    out = ""
    for l in lines:
        out += l if l in ("0000", "0001") else "%04x%s\\n" % (len(l) + 5, l)
    return out

def req(method, url, hdrs="", body=None, gz=False):
    b = ""
    if body is not None:
        b = ('-gzipbody "%s"' if gz else '-body "%s"') % body
        if gz:
            hdrs += '-hdr "Content-Encoding: gzip" '
    return f'\ttxreq -req {method} -url "{url}" {hdrs}{b}\n\trxresp\n'

def fetch(*w, agent="agent=git/2.43.0"):
    return pk("command=fetch", agent, "object-format=sha1", "0001", *[f"want {x}" for x in w], "done", "0000")

def up(url, body, extra="", gz=False, v2=True, ct=True):
    return req("POST", url, (V2 if v2 else "") + (CT if ct else "") + extra, body, gz)

def hit(n):
    return f'\texpect resp.http.X-Hits == "{n}"\n'

KEY_EMPTY = '\texpect resp.http.X-Key == ""\n'
AUTH_A = '-hdr "Authorization: Basic QUFBOnQtYWxpY2U=" '
AUTH_B = '-hdr "Authorization: Basic Ym9iOnQtYm9i" '
LS = pk("command=ls-refs", "0001", *["ref-prefix refs/heads/" + "x" * 60 + str(i) for i in range(12)], "0000")

v = '''varnishtest "gitcache: gzip, instance scope, credentials, protocol parameters, probes, dumb, LFS, limits"

server s1 {
	loop 80 {
		rxreq
		txresp
	}
} -start

varnish v1 -vcl+backend {
	import gitcache from "${vmod}";
	sub vcl_recv {
		if (gitcache.kind() == "") { return (pass); }
		if (req.http.X-Mode == "scope") {
			set req.http.X-Key = gitcache.cache_key(scope = req.http.X-Up);
		} else if (req.http.X-Mode == "share") {
			set req.http.X-Key = gitcache.cache_key(share_credentials = true);
		} else if (req.http.X-Mode == "small") {
			set req.http.X-Key = gitcache.cache_key(limit = 100B);
		} else if (req.http.X-Mode == "lfs") {
			set req.http.X-Key = gitcache.cache_key(lfs_batch = true);
		} else {
			set req.http.X-Key = gitcache.cache_key();
		}
		if (req.http.X-Key == "") { return (pass); }
		return (hash);
	}
	sub vcl_hash {
		hash_data(req.http.X-Key);
		return (lookup);
	}
	sub vcl_backend_fetch {
		if (bereq.http.Content-Length) { set bereq.method = "POST"; }
	}
	sub vcl_backend_response {
		set beresp.ttl = 60s;
		return (deliver);
	}
	sub vcl_deliver {
		set resp.http.X-Hits = obj.hits;
		set resp.http.X-Key = req.http.X-Key;
		set resp.http.X-Cred = gitcache.credential();
	}
} -start
'''

def client(n, title, body):
    global v
    v += f'\nclient c{n} {{\n{body}}} -run\n'

client(1, "1. gzip + reordered wants + different agent == same object as the plain request",
    up("/g/one.git/git-upload-pack", fetch("1111", "2222")) + hit(0) +
    up("/g/one.git/git-upload-pack", fetch("2222", "1111", agent="agent=git/2.50.1-Apple"), gz=True) + hit(1) +
    up("/g/one.git/git-upload-pack", fetch("3333")) + hit(0))

client(2, "2. two GHES instances behind one cache never share keys",
    up("/s/one.git/git-upload-pack", fetch("1111"), '-hdr "X-Mode: scope" -hdr "X-Up: ghes-a" ') + hit(0) +
    '\texpect resp.http.X-Key ~ "^ghes-a/s/one[.]git/"\n' +
    up("/s/one.git/git-upload-pack", fetch("1111"), '-hdr "X-Mode: scope" -hdr "X-Up: ghes-b" ') + hit(0) +
    '\texpect resp.http.X-Key ~ "^ghes-b/s/one[.]git/"\n' +
    up("/s/one.git/git-upload-pack", fetch("1111"), '-hdr "X-Mode: scope" -hdr "X-Up: ghes-a" ') + hit(1))

client(3, "3. credentials scope the key by default; sharing is opt-in",
    up("/c/one.git/git-upload-pack", fetch("1111")) + hit(0) +
    '\texpect resp.http.X-Key !~ "cred="\n\texpect resp.http.X-Cred == ""\n' +
    up("/c/one.git/git-upload-pack", fetch("1111"), AUTH_A) + hit(0) +
    '\texpect resp.http.X-Key ~ "#cred=[0-9a-f]+$"\n\texpect resp.http.X-Cred ~ "^[0-9a-f]+$"\n\texpect resp.http.X-Cred !~ "QUFB"\n' +
    up("/c/one.git/git-upload-pack", fetch("1111"), AUTH_A) + hit(1) +
    up("/c/one.git/git-upload-pack", fetch("1111"), AUTH_B) + hit(0) +
    up("/c/two.git/git-upload-pack", fetch("1111"), '-hdr "X-Mode: share" ' + AUTH_A) + hit(0) +
    '\texpect resp.http.X-Key !~ "cred="\n' +
    up("/c/two.git/git-upload-pack", fetch("1111"), '-hdr "X-Mode: share" ' + AUTH_B) + hit(1))

client(4, "4. refs advertisements are keyed per protocol version and per extra Git-Protocol parameter",
    req("GET", "/p/one.git/info/refs?service=git-upload-pack", V2) + hit(0) + '\texpect resp.http.X-Key ~ "/v2$"\n' +
    req("GET", "/p/one.git/info/refs?service=git-upload-pack") + hit(0) + '\texpect resp.http.X-Key ~ "/v0$"\n' +
    req("GET", "/p/one.git/info/refs?service=git-upload-pack", '-hdr "Git-Protocol: version=1" ') + hit(0) + '\texpect resp.http.X-Key ~ "/v1$"\n' +
    req("GET", "/p/one.git/info/refs?service=git-upload-pack", V2) + hit(1) +
    req("GET", "/p/one.git/info/refs?service=git-upload-pack", '-hdr "Git-Protocol: version=2:object-format=sha256" ') + hit(0) +
    '\texpect resp.http.X-Key ~ "/v2[+]object-format=sha256$"\n' +
    req("GET", "/p/one.git/info/refs?service=git-upload-pack", '-hdr "Git-Protocol: object-format=sha256:version=2" ') + hit(1) +
    req("GET", "/p/one.git/info/refs?service=git-upload-pack&x=1", V2) + hit(2) +
    req("GET", "/p/one.git/info/refs?service=git-receive-pack", V2) + KEY_EMPTY)

client(5, "5. limits, bad encodings, bad content types and probes are never cached",
    up("/l/one.git/git-upload-pack", LS, '-hdr "X-Mode: small" ') + KEY_EMPTY +
    up("/l/one.git/git-upload-pack", LS, '-hdr "X-Mode: small" ', gz=True) + KEY_EMPTY +
    up("/l/one.git/git-upload-pack", LS, gz=True) + '\texpect resp.http.X-Key ~ "^/l/one[.]git/git-upload-pack/v2/"\n' +
    req("POST", "/l/one.git/git-upload-pack", V2 + CT + '-hdr "Content-Encoding: br" ', "0000") + KEY_EMPTY +
    up("/l/one.git/git-upload-pack", fetch("1111"), ct=False) + KEY_EMPTY +
    req("POST", "/l/one.git/git-upload-pack", V2 + '-hdr "Content-Type: text/plain" ', fetch("1111")) + KEY_EMPTY +
    up("/l/one.git/git-upload-pack", "0000") + KEY_EMPTY +
    up("/l/one.git/git-upload-pack", "0000", v2=False) + KEY_EMPTY +
    up("/l/one.git/git-upload-pack", "zzzz") + KEY_EMPTY)

client(6, "6. dumb protocol: immutable objects share, mutable files expire by policy, credentials still scope",
    req("GET", "/d/one.git/objects/pack/pack-0123456789abcdef0123456789abcdef01234567.pack") + hit(0) +
    req("GET", "/d/one.git/objects/pack/pack-0123456789abcdef0123456789abcdef01234567.pack") + hit(1) +
    req("GET", "/d/one.git/objects/pack/pack-0123456789abcdef0123456789abcdef01234567.pack", AUTH_A) + hit(0) +
    req("GET", "/d/one.git/objects/ab/cdef0123456789abcdef0123456789abcdef01") + hit(0) +
    '\texpect resp.http.X-Key == "/d/one.git/objects/ab/cdef0123456789abcdef0123456789abcdef01"\n' +
    req("GET", "/d/one.git/info/refs") + hit(0) + '\texpect resp.http.X-Key == "/d/one.git/info/refs"\n' +
    req("GET", "/d/one.git/HEAD") + '\texpect resp.http.X-Key == "/d/one.git/HEAD"\n')

LFS_DL = '{\\"operation\\": \\"download\\", \\"objects\\": [{\\"oid\\": \\"aa\\", \\"size\\": 1}]}'
LFS_DL2 = '{\\"operation\\": \\"download\\", \\"objects\\": [{\\"oid\\": \\"aa\\", \\"size\\": 1}, {\\"oid\\": \\"bb\\", \\"size\\": 2}], \\"transfers\\": [\\"lfs-standalone-file\\", \\"basic\\", \\"ssh\\"]}'
LFS_DL_PERM = '{\\"operation\\": \\"download\\", \\"objects\\": [{\\"oid\\": \\"bb\\", \\"size\\": 2}, {\\"oid\\": \\"aa\\", \\"size\\": 1}], \\"transfers\\": [\\"ssh\\", \\"lfs-standalone-file\\", \\"basic\\"]}'
LFS_UP = '{\\"operation\\": \\"upload\\", \\"objects\\": [{\\"oid\\": \\"aa\\", \\"size\\": 1}]}'
OID = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
client(7, "7. LFS: batch is not cached by default; opt-in is download-only; locks are never cached",
    req("POST", "/f/one.git/info/lfs/objects/batch", '-hdr "Content-Type: application/vnd.git-lfs+json" ', LFS_DL) + KEY_EMPTY +
    req("POST", "/f/one.git/info/lfs/objects/batch", '-hdr "X-Mode: lfs" ', LFS_DL) + hit(0) +
    '\texpect resp.http.X-Key ~ "^/f/one[.]git/info/lfs/objects/batch/[0-9a-f]+$"\n' +
    req("POST", "/f/one.git/info/lfs/objects/batch", '-hdr "X-Mode: lfs" ', LFS_DL) + hit(1) +
    req("POST", "/f/one.git/info/lfs/objects/batch", '-hdr "X-Mode: lfs" ', LFS_DL2) + hit(0) +
    req("POST", "/f/one.git/info/lfs/objects/batch", '-hdr "X-Mode: lfs" ', LFS_DL_PERM) + hit(1) +
    req("POST", "/f/one.git/info/lfs/objects/batch", '-hdr "X-Mode: lfs" ', LFS_UP) + KEY_EMPTY +
    req("POST", "/f/one.git/info/lfs/locks/verify", '-hdr "X-Mode: lfs" ', "{}") + KEY_EMPTY +
    req("GET", f"/f/one.git/info/lfs/objects/{OID}") + hit(0) +
    req("GET", f"/f/one.git/info/lfs/objects/{OID}") + hit(1) +
    req("PUT", f"/f/one.git/info/lfs/objects/{OID}", "", "x") + KEY_EMPTY)

client(8, "8. LFS transfer URLs: content-addressed, query (pre-signed signature) is part of the key, verify/upload never cached",
    req("GET", f"/repository/lfs-hosted/{OID}") + hit(0) +
    '\texpect resp.http.X-Key == "/repository/lfs-hosted.git/lfs-href/' + OID + '"\n' +
    req("GET", f"/repository/lfs-hosted/{OID}") + hit(1) +
    req("GET", f"/repository/lfs-hosted/{OID}", AUTH_A) + hit(0) +
    req("GET", f"/repository/lfs-hosted/{OID}?sig=aaa") + hit(0) +
    '\texpect resp.http.X-Key ~ "[?]q=[0-9a-f]+$"\n' +
    req("GET", f"/repository/lfs-hosted/{OID}?sig=aaa") + hit(1) +
    req("GET", f"/repository/lfs-hosted/{OID}?sig=bbb") + hit(0) +
    req("POST", f"/repository/lfs-hosted/{OID}/verify", "", "{}") + KEY_EMPTY +
    req("PUT", f"/repository/lfs-hosted/{OID}", "", "x") + KEY_EMPTY +
    req("GET", "/repository/lfs-hosted/not-an-oid") + KEY_EMPTY)

open("tests/keys.vtc", "w").write(v)
