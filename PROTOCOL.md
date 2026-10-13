# vmod_gitcache: protocol coverage

This document maps every git transport feature and gotcha we researched to the
behaviour of the vmod (and the example VCL in `tests/e2e/git.vcl`) and to the
test that pins it. Sources: git `http-protocol`, `protocol-v2`,
`protocol-capabilities`, the Git LFS batch API, and the GitHub Enterprise Server
docs (subdomain isolation, REST base URL).

Test layers:

| Layer | What it proves |
|---|---|
| unit (`tests/unit*.c`) | the pkt-line canonicaliser, URL router, LFS body check and gunzip, under ASan/UBSan |
| vtc (`tests/*.vtc`) | the vmod functions inside a real Varnish |
| e2e (`tests/e2e/run.sh`) | real `git` clients, a real Varnish, a simulated GHES-style origin |
| upstream (`tests/e2e/upstream.sh`) | real `git` through Varnish and TLS to github.com, or any GHES host |

## 1. The spec says "do not cache". Why caching is still safe here

`http-protocol` requires `Cache-Control: no-cache` on `info/refs`, upload-pack
and receive-pack responses and says clients MUST NOT reuse them. A shared
read-through cache overrides that on purpose, so safety has to come from the
design, not the headers:

* **Packs are a function of the request.** A `fetch` response is determined by
  the exact `want`/`have`/capability set, so it is keyed by a hash of the
  canonical request body. Same request, same bytes.
* **Refs are the only mutable thing**, so they get a short TTL, **no grace**
  (never serve a stale tip while revalidating) and can be purged by webhook.
* **Access is re-verified before any shared pack is served** (see 3).
* Everything that mutates (`receive-pack`, LFS upload batches, LFS locks) is
  never cached.

The example VCL skips Varnish's builtin `vcl_backend_response` (which would
honour the origin's `no-cache`) with `return (deliver)`. Without that, nothing is
cached; this was found with a real clone, not a mock, and every GHES/GitHub
response carries `Cache-Control: no-cache`.

## 2. Request classification (`gitcache.kind()`)

| kind | request | cached by default? | test |
|---|---|---|---|
| `info-refs` | `GET .../info/refs?service=git-upload-pack` | yes, short TTL | kind.vtc, keys.vtc c4, e2e A/B |
| `info-refs-push` | `...?service=git-receive-pack` | never | keys.vtc c4 |
| `ls-refs` | v2 `command=ls-refs` | yes, short TTL, cred-scoped | kind.vtc, e2e A |
| `fetch` | v2 `command=fetch` (and unknown v2 commands) | yes, shared + preflight | e2e A, D, E, H |
| `object-info`, `bundle-uri` | v2 commands | yes (bundle-uri short TTL) | kind.vtc |
| `upload-pack` | v0/v1 `POST git-upload-pack` | yes, shared + preflight | e2e B |
| `probe` | bare `0000` POST git sends before a large request | never | kind.vtc, keys.vtc c5, e2e G |
| `receive-pack` | `POST git-receive-pack` | never | kind.vtc, keys.vtc c3/c4 |
| `lfs-batch` | `POST info/lfs/objects/batch` | **opt-in**, download only | keys.vtc c7 |
| `lfs-object` | `GET info/lfs/objects/<sha256>`, or any `GET` whose last path segment is a 64-hex SHA-256 (a server-chosen transfer URL, e.g. `/repository/<repo>/<oid>`) | yes (content-addressed; the client verifies the oid) | keys.vtc c7/c8 |
| `lfs-other` | locks, upload `verify`, PUT, ... | never | kind.vtc, keys.vtc c7/c8 |
| `dumb-refs` | dumb `info/refs`, `HEAD`, `objects/info/*` | yes, short TTL | kind.vtc, keys.vtc c6, e2e I |
| `dumb-object` | `objects/xx/...`, `objects/pack/pack-*.{pack,idx,rev}` | yes, immutable | kind.vtc, keys.vtc c6, e2e I |

URL handling: owner/repo is lowercased (GitHub paths are case-insensitive),
`.git` suffix optional, leading slashes ignored, extra query parameters on
`info/refs` do not fragment the key.

## 3. Access control (the part that decides whether this is deployable)

GitHub and GHES authorise per user, almost always with Basic auth
(`user:token`), while a cached pack is the same for everyone who may read the repo.

* **Default key includes `credential()`** (a truncated SHA-256 of the
  `Authorization` header, never the header itself): private data can never be
  served to a client that did not present the same credentials.
  (keys.vtc c3, e2e C)
* **Shared-pack mode** (`share_credentials = true`) gives one pack object for
  all users, which is what makes a team's CI hit rate good. It is only used
  together with the **preflight** in `git.vcl`: before serving a shared pack the
  VCL fetches the *credential-scoped* `info/refs` for the same repo with the
  caller's own credentials (cached for the refs TTL), and restarts to serve the
  pack only on a 200. A refusal (401/403/404) from the origin is returned to the
  client unchanged, which is exactly what makes git retry with credentials.
* Evidence (e2e F): a newly granted user reuses another user's cached pack
  (only their own `ls-refs` reaches the origin); a user with a valid token but
  no access gets refused and **zero pack requests** reach the origin on their
  behalf; revoking access to a repo that was cached anonymously refuses
  anonymous clones within the refs TTL.
* Residual window: access revocation takes effect within the refs TTL (5 s in
  the example). Purge (below) can close it to near zero.
* `.recv()` strips client-supplied `X-GC-*` headers the first time it runs for a
  request (tracked per task, not by `req.restarts`, since VCL may restart before
  `.recv()` runs), and `.hash()` only uses a key `.recv()` set, so a client cannot
  forge the "preflight done" marker or a key. Every later `.recv()` recomputes
  its state, so a restart that changes the URL never keeps the old key.
  (object.vtc)

## 4. Cache-key correctness

What goes into a pack key, and why (`src/pktline.c`):

| input | treatment | why |
|---|---|---|
| `agent=`, `session-id=` | dropped | client telemetry, never changes the response |
| `want`, `want-ref`, `shallow`, `ref-prefix`, `oid`, `deepen*`, `filter`, `packfile-uris` | sorted, de-duplicated | set semantics |
| bare flags (`thin-pack`, `ofs-delta`, `no-progress`, `include-tag`, `sideband-all`, `wait-for-done`, `done`, `peel`, `symrefs`, `unborn`, `size`) | sorted, de-duplicated | order-insensitive but **kept**: they change the response bytes |
| `have` and any unknown argument | original order | conservative: unknown future arguments are never reordered |
| `object-format=`, `server-option=` | kept | change the response |
| v0/v1 capabilities on the first `want` | split off, `agent`/`session-id` dropped, rest sorted | same |
| `Git-Protocol` header | version **and** extra parameters, sorted, in the key of refs and packs | a v2 advertisement is invalid for a v0 client; `object-format=sha256` differs |
| ambiguous `Git-Protocol` (repeated header, two versions, unknown or malformed `version=`, over 16 keys) | not cached | the origin may read it differently, e.g. git takes the highest version (keys.vtc c4) |

Bodies are rejected (no key, request passes through) when: framing is invalid,
a packet exceeds 65520 bytes, `command=` is not first, data follows the final
flush, there is no flush, a v0 body has no `want`, the body is the bare `0000`
probe, `Content-Type` is not `application/x-git-upload-pack-request`, the
`Content-Encoding` is anything but `gzip`/`identity`, or the (decompressed) body
exceeds `limit`. A body whose `Content-Length` exceeds `limit` is never buffered,
so it still reaches the origin. A chunked body (no `Content-Length`) is never
buffered: it is not keyed and passes through uncached (chunked.vtc).

**gzip**: git compresses request bodies over 1 KB. The body is inflated before
canonicalisation so the key does not depend on the client's compressor, with the
inflated size capped at `limit` (zip-bomb guard). Found with a real fetch: before
this, every large negotiation bypassed the cache. (keys.vtc c1/c5, e2e E)

Tests: unit.c (every row above), keys.vtc c1 c4 c5.

## 5. Found by testing against real git (not predicted by the docs)

1. **`Cache-Control: no-cache` silently disables all caching** unless the VCL
   skips the builtin. (e2e A, upstream)
2. **`info/refs` key must include the protocol version.** A cached v2
   advertisement served to a v0 client made `git clone` report an *empty
   repository*. (e2e B)
3. **Default grace serves stale refs.** Varnish's 10 s default grace returned an
   expired tip as a hit; refs now use `grace = 0s`. (e2e E, D)
4. **gzip request bodies** defeated keying until inflated. (e2e E)
5. **Probe POSTs**: git sends `0000` first when a request exceeds
   `http.postBuffer`; caching it would be pointless and would hide auth
   failures, so it is classified and bypassed. (kind.vtc, e2e G)
6. **Chunked request bodies** (git sends them only above `http.postBuffer`,
   1 MiB by default) are never buffered: they pass through uncached and cannot
   fail. (chunked.vtc, e2e G)

## 6. Feature coverage (real `git` through the cache, `fsck --strict` clean)

| Feature | evidence |
|---|---|
| clone, protocol v2 and v0 | e2e A, B; upstream (github.com) |
| incremental fetch, new commits visible after TTL | e2e D |
| shallow clone (`--depth`) | e2e H |
| partial clone (`--filter=blob:none`) + lazy blob fetch | e2e H |
| SHA-256 repositories (`object-format`) | e2e H |
| gzip requests, chunked passthrough, probe | e2e E, G |
| 8 simultaneous identical clones -> 2 origin pack requests (16 uncoalesced) | e2e H |
| dumb HTTP clone served entirely from cache | e2e I |
| private repos, per-user isolation, shared packs, revocation | e2e C, F |
| webhook purge on `push` | e2e J |
| real TLS upstream github.com, v2 and v0, zero extra upstream requests on second clone | `upstream` |

## 7. GitHub Enterprise Server

From the docs: git is served at `https://HOSTNAME/owner/repo.git/...` with the
same smart-HTTP endpoints as github.com; the REST API is under `/api/v3`;
subdomain isolation moves `media`, `codeload`, `raw`, `uploads`, ... to
subdomains but git stays on `HOSTNAME`; authentication is a personal access
token as the Basic-auth password.

* One config serves any host (`tests/e2e/upstream.vcl.in`, TLS verified, SNI on).
* `scope` in `cache_key()` separates instances that share `owner/name` space
  (keys.vtc c2).
* The `push` webhook can drive the purge hook in the example VCL so refs refresh
  as soon as data lands instead of after a TTL.
* **Not tested against a real GHES instance**: none was available; behaviour is
  derived from the docs and from the github.com run.
* A private or internal CA on the GHES host must be in the trust store
  (`ssl_verify_peer` and `ssl_verify_host` are on).

## 8. Git LFS

* **Transfer URLs are server-chosen.** Some servers download from
  `/repository/<repo>/<oid>`, not under `/info/lfs/`. A classifier that only
  knows the spec's `info/lfs/objects/<oid>` caches the batch but none of the
  bytes. Objects are therefore recognised by a final path segment that is a
  64-hex SHA-256, which clients verify against their pointer file.
* **Pre-signed URLs are capabilities.** Many servers (and GitHub's S3-backed
  storage) put an expiring signature in the query string. If the key ignored it,
  anyone who knew an oid (they are visible in pointer files) could pull the
  object from cache without a valid signature. The key therefore includes a hash
  of the query: an object is shared only between requests presenting the same
  signed URL. (keys.vtc c8)
* The batch response itself is only cached with `lfs_batch = true`, downloads
  only, with a TTL shorter than the transfer-URL expiry.
* **GitHub's own LFS flow works through the cache** (`tests/e2e/upstream.sh` with
  `LFS_REPO`): `git lfs pull` against a real public repository passes `git lfs fsck`,
  the batch request goes through the proxy, and the objects are then downloaded
  directly from GitHub's storage host named in the batch response, so they do not
  pass through this cache. With `lfs_batch = true` a second clone and pull needs no
  origin request at all; without it, only the batch itself reaches GitHub.
* git-lfs also probes `<lfs-url>/info/refs?service=git-upload-pack`; it is
  classified `lfs-other` and never cached. Uploads, `verify` callbacks and the
  locking API are classified `lfs-other` too.

Not covered: serving LFS objects themselves through the cache when a server hands
out their URLs on another host (that needs the batch response rewritten), and
GitLab/Gitea URL shapes.

## 9. Hardening: fuzzing and what it found

The request body, URL, LFS body and `Content-Encoding: gzip` payload are all
attacker-controlled and parsed inside `varnishd`. The parsers are plain C with no
Varnish dependency (`src/pktline.c`, `src/route.c`) so they can be fuzzed with
libFuzzer under ASan + UBSan (harnesses in `tests/fuzz/`).

Properties asserted on every input, beyond "does not crash": canonicalisation is
deterministic and **idempotent**; every emitted pkt-line is within the protocol
limit; every pointer the router returns lies inside the URL; gunzip never exceeds
its cap, leaves the buffer untouched on failure, and gzip->gunzip round-trips.

Run so far (after the fixes below): **44.5 M** pkt-line executions and
**49.4 M** router/LFS/gunzip executions, no findings.

Bugs found by it (all fixed, each pinned by a regression input in
`tests/fuzz/regress/` and a unit test):

1. **Heap buffer overflow (remotely triggerable).** The v0/v2 canonicalisers sized
   their line arrays at `len/5`, but the empty pkt-line `0004` is 4 bytes, so a
   POST body of repeated `0004` wrote past the allocation. Confirmed by
   re-running the regression test against the old bound: ASan reports
   `heap-buffer-overflow` in `canon_v0`.
2. **Non-idempotent v0 canonical form** (capabilities were emitted on a synthetic
   `caps` line). Canonical v0 now keeps them on the first `want`, as a client
   would, so the output is itself a valid request.
3. **Ambiguous input accepted.** `want`/`have`/`oid`/`shallow` lines with a
   non-hex or trailing-junk object id, and capabilities on a non-first `want`,
   made the canonical form depend on parse order. They are now rejected
   (uncacheable, passed through).

5. **LFS batch bodies were scanned by substring, not parsed.** Finding `"transfers"`,
   `"objects"` and `"operation"` with a plain text search matched them inside string
   values. That could rewrite part of a value and make two different requests share a key,
   and an `upload` batch whose string contained `"operation":"download"` would have been
   classified as a download. They are now read by walking the top-level JSON object only,
   and anything that is not well-formed JSON is hashed verbatim. (Found by the router
   fuzzer's idempotence check; pinned in `unit_route.c` and
   `tests/fuzz/corpus-route/regress-lfs-canon-substring-search`.)
6. **The module exported every internal helper and the vendored gzip library's API**, so
   another copy of either in the process could have taken over its calls. A linker version
   script now exports a single symbol, `Vmod_gitcache_Data`; found by listing the built
   module's symbols.

Found by code review rather than fuzzing:

4. **Workspace exhaustion silently degraded to protocol v0**, which would have put
   a v2 request under a v0 key. It now calls `VRT_fail` and fails the request.
   `tests/oom.vtc` starves the workspace with `vtc.workspace_alloc` and asserts the
   vmod logs `gitcache: out of workspace`. (A first version of that test passed
   without ever reaching the vmod: Varnish had already closed the connection with
   `RX_OVERFLOW`. The log showed it; the test was rebuilt to be deterministic.)

7. **Refs keyed under the wrong protocol version.** `Git-Protocol: version=1:version=2`
   was keyed as v1 while git answers v2 (it takes the highest), and
   `version=0: version=2` or a repeated header could be keyed as v2 while the origin
   answered v0: one request could poison the refs every client of that version gets.
   Ambiguous headers are now uncacheable. (keys.vtc c4)
8. **Forged or stale state across restarts.** The client's `X-GC-*` headers were only
   removed when `req.restarts == 0`, so a VCL that restarts before `.recv()` let a
   client pick any cache key, including another credential's; and a restart that
   changed the URL kept the previous key. (object.vtc)
9. **Bodies over the limit were destroyed.** `kind()` buffered with 1 MB, not the
   documented 10 MB, and a body over the limit was read and discarded, so the request
   failed instead of passing through. (kind.vtc, object.vtc)
10. **Per-thread memory leaked when a worker thread exited** (scratch buffers, the
    inflater and the digest context, about 12 KB each). They are now freed by a
    thread-exit hook. (unit_route.c, under LeakSanitizer)
11. **The credential fingerprint failed open**: if SHA-256 was unavailable the key
    silently lost its credential. It now fails the request.

## 10. Known limits, deliberately not hidden

* **LFS batch caching is opt-in** (`lfs_batch = true`, downloads only). The
  response contains time-limited transfer URLs; keep the TTL below their expiry.
  The LFS spec says not to persist them. The actual LFS *object* downloads
  usually go to `media.HOSTNAME` or a CDN with pre-signed URLs and are not
  rewritten through this cache (that needs response-body rewriting; out of scope).
* **SSH and `git://`** transports are not HTTP and out of scope.
* **Very large packs**: nothing here bounds object size for chunked responses;
  configure storage (MSE4 or malloc size) and consider
  `beresp.uncacheable` over a size threshold in VCL.
* **Not run against a real GHES instance.** Verified against github.com
  (public repo) and a faithful local smart-HTTP origin (real `git upload-pack`).
* **LFS objects on a separate storage host are not cached** (section 8): the
  client downloads them directly.
