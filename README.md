# vmod_gitcache

A VMOD that makes Varnish a safe, effective read-through cache for **git over HTTP**: the
smart protocol (v0, v1, v2), the dumb protocol, and Git LFS. It works with github.com,
GitHub Enterprise Server, and any server that speaks the same endpoints.

Supported: **Varnish Enterprise 6.0**, **Varnish Cache 9.x** and **Vinyl Cache 9.x**.

It solves the parts that make git awkward to cache: stable cache keys for `git-upload-pack`
POSTs (client telemetry stripped, sets sorted, gzip bodies handled, chunked bodies passed
through), classification of every endpoint, and keys that never let one user's private data
reach another.

[`PROTOCOL.md`](PROTOCOL.md) lists exactly what is and is not covered, with the test behind
every claim.

## Use

```vcl
vcl 4.1;

import gitcache;

backend default {
    .host = "github.com";
    .port = "443";
    .ssl = 1;
    .ssl_sni = 1;
    .ssl_verify_peer = 1;
    .ssl_verify_host = 1;
}

sub vcl_init {
    new git = gitcache.cache(scope = "github");
}

sub vcl_recv {
    if (git.recv()) {
        return (hash);
    }
}

sub vcl_hash {
    if (git.hash()) {
        return (lookup);
    }
}

sub vcl_backend_fetch {
    set bereq.http.Host = "github.com";
    git.backend_fetch();
}

sub vcl_backend_response {
    if (git.backend_response()) {
        return (deliver);
    }
}

sub vcl_deliver {
    if (git.deliver()) {
        return (restart);
    }
}
```

That is the whole policy. The `cache` object classifies every request, builds the keys, applies
short TTLs and no grace to refs, shares packs between users only after re-checking access with
the caller's own credentials, ignores the origin's `Cache-Control: no-cache`, and tags objects so
a repository can be purged. Anything that is not cacheable git traffic, including every push,
falls through to the rest of your VCL untouched.

## Configuring git

Git needs no plugin. Reads are sent to the cache by rewriting the remote URL in your
`~/.gitconfig` (or a repository's `.git/config`), so every existing clone, script and CI job
keeps using the normal `https://github.com/...` URLs:

```ini
# fetch, clone and pull go through the cache
[url "https://git-cache.example.com/"]
    insteadOf = https://github.com/

# push goes straight to GitHub, never through the cache
[url "git@github.com:"]
    pushInsteadOf = https://github.com/

# the cache serves protocol v2, which is the default in git 2.26 and newer
[protocol]
    version = 2
```

`git clone https://github.com/torvalds/linux.git` now fetches from `git-cache.example.com`,
and `git push` still goes to GitHub over SSH. Drop the second block if you would rather push
through the cache as well; pushes are never cached, only forwarded.

**Credentials.** Git sends them to the host it is talking to, which is now the cache, so tell
your credential helper about that host as well as `github.com`:

```ini
[credential "https://git-cache.example.com"]
    helper = store
```

The `Authorization` header is forwarded to GitHub unchanged. The cache never holds a token,
and a private repository is only served to a client that presented credentials for it.

**GitHub Enterprise Server** is the same, with its own host and its own `scope`:

```ini
[url "https://ghes-cache.example.com/"]
    insteadOf = https://ghes.example.com/
```

**Git LFS** follows the rewritten URL, so `git lfs pull` is routed through the cache with no
extra setting. LFS batch responses are cached only if you set `lfs_batch = 1`.

**One repository only**, without touching the global config:

```sh
git config url."https://git-cache.example.com/".insteadOf https://github.com/
```

**Try it for a single command**, then remove it again:

```sh
git -c url."https://git-cache.example.com/".insteadOf=https://github.com/ clone https://github.com/octocat/Hello-World.git
```

Varnish does not terminate TLS, so put hitch, haproxy or a cloud load balancer in front of it
for the `https://git-cache.example.com` address, or use `http://` on a trusted network.

## Configuration

### Scope

`scope` is a label put at the front of every cache key. Cached objects are named by repository
(`owner/name`), so two different git servers behind the same Varnish, say github.com and a GitHub
Enterprise Server, could both have an `acme/tools` and would otherwise share cache entries. Give
each upstream its own scope (`"github"`, `"ghes1"`) and they can never mix. With a single
upstream the value does not matter and can be left out.

### Options

| Option | Default | Meaning |
|---|---|---|
| `scope` | `""` | label separating upstreams that share this Varnish (see above) |
| `refs_ttl` | `5s` | how long ref advertisements (`info/refs`) are cached |
| `pack_ttl` | `7d` | how long packs and content-addressed objects are cached; they never change, so keep this long, and [purge](#purging) a repository after rewriting or deleting its history |
| `preflight` | `1` | before serving a shared pack, re-check the caller's access with their own credentials; turn off only if clients are anonymous |
| `lfs_batch` | `0` | cache Git LFS download batch responses |
| `lfs_batch_ttl` | `10s` | how long those are cached (they contain time-limited URLs) |
| `limit` | `10MB` | largest request body that is buffered and keyed; larger bodies pass through uncached |
| `transfer_timeout` | `1h` | longest a single pack transfer may take, waiting for the origin and sending to the client; Varnish's own defaults (60 s, 600 s) cut off a cold clone of a large repository |
| `purge_token` | `""` | secret enabling the purge endpoint (see Purging) |
| `debug` | `0` | add `X-GC-Kind` and `X-GC-Hits` response headers |

### Storage

A pack is cached whole, so the storage must hold the largest repository you expect to serve,
plus everything else you want to keep. Varnish's default is 256 MB. A full clone of the Linux
kernel is a single 6.5 GB object, and was cached without eviction in `-s malloc,7g`:

```sh
varnishd -a :80 -f git.vcl -s malloc,16g
```

### TLS to the origin

The vmod does not open connections; TLS belongs to the backend definition. The `backend` block
above uses native backend TLS, available on Varnish Enterprise and Varnish Cache 9.x: it sends
the server name (SNI) and verifies the origin's certificate and hostname. Point `.host` at your
GitHub Enterprise Server instead to cache that.

**Vinyl Cache 9.x** cannot originate TLS, so point its backend at a local TLS client such as
stunnel or haproxy (`.host = "127.0.0.1"; .port = "8443";`) that connects to `github.com:443`
with certificate verification on.

## Purging

To purge a repository when it changes, set `purge_token` and add this to `vcl_recv`, before the
`git.recv()` call:

```vcl
sub vcl_recv {
    if (req.url ~ "^/_gitcache/purge") {
        return (synth(git.purge()));
    }
}
```

Then send `POST /_gitcache/purge?repo=owner/name` with an `X-Purge-Token` header from your push
webhook. [`tests/e2e/git.vcl`](tests/e2e/git.vcl) is the complete policy with this hook.

## Lower-level API

The `cache` object is built on these, which you can use to write a different policy:

| Function | Returns |
|---|---|
| `kind()` | what the request is: `info-refs`, `ls-refs`, `fetch`, `upload-pack`, `receive-pack`, `lfs-batch`, `lfs-object`, `dumb-refs`, ... or `""` |
| `repo()` | the lowercased `owner/name` |
| `protocol()` | 0, 1 or 2, from `Git-Protocol` |
| `credential()` | a short non-reversible fingerprint of `Authorization` |
| `body_hash(limit)` | hash of the canonical upload-pack request body |
| `cache_key(limit, scope, share_credentials, lfs_batch)` | a complete cache key, or `""` for anything that must not be cached |

Full reference in `src/vmod_gitcache.vcc`.

## Safety

* A cached object is only served to a client that presented the same credentials, except packs:
  those are shared between users, and only after a request with the caller's own credentials
  has been accepted by the origin (`preflight`, on by default).
* Pushes, LFS uploads and LFS locks are never cached; LFS batch responses only on request.
* Request bodies, URLs and gzip payloads are parsed by fuzzed, sanitizer-tested code and
  anything malformed is passed through uncached.

## Performance

What the cache saves, measured against real github.com with `libgit2/libgit2` (77 MB): the same
operations straight from GitHub and through Varnish, median of 5 runs, on Varnish Enterprise and
Varnish Cache 9.1 (the two runs agree apart from network noise on the direct side).

| Operation | Straight from GitHub | Through the cache |
|---|---|---|
| `git clone` (cached) | 7.0 - 10.8 s | **3.3 s** |
| `git clone` (first, cache empty) | - | 7.3 - 8.2 s, same as direct |
| `git fetch`, nothing new | 0.25 s | **0.01 - 0.02 s** |
| `git ls-remote` | 0.47 s | **0.01 s** |

| Six clones of the same repository | Straight from GitHub | Through the cache |
|---|---|---|
| Data downloaded from GitHub | about 460 MB | **72 MB** |

The first clone pays full price and fills the cache; every later clone, from any machine behind
it, is served from memory. The 3.3 s left on a cached clone is git unpacking and checking the
objects on the client, so the saving grows with a slower or more distant link to GitHub, and
shrinks to nothing for a repository nobody else clones. Ref advertisements are cached for
`refs_ttl` (5 s by default), so fetches and `ls-remote` repeated inside that window never leave
your network, and a new push is visible to clients within that time.

**GitHub Enterprise Server.** The same setup has been run against a GitHub Enterprise Server
instance. A full clone of the Linux kernel through the cache was about 30% faster than going to
the server directly. Several clients cloning the same repository at the same time should gain
more, because they all share one cached copy instead of each pulling its own from the server;
that case has not been measured here. A repository of that size needs storage to match (see
[Storage](#storage)) and, on a slow link, a `transfer_timeout` long enough for a cold clone.

These figures are from a single machine (an aarch64 container on a laptop, over its ordinary
internet connection); your numbers will track your own link. To reproduce them:
[`tests/bench/cache_vs_direct.sh`](tests/bench/cache_vs_direct.sh) (`REPO=owner/name` picks another).

## Supported Varnish versions

| Target | unit | varnishtest | end-to-end (real git) | real github.com over TLS |
|---|---|---|---|---|
| Varnish Enterprise 6.0.18r5 | pass | 6/6 | 62/62 | 16/16 |
| Varnish Cache 9.1.0 | pass | 6/6 | 62/62 | 16/16 |
| Vinyl Cache 9.1.0 | pass | 6/6 | 62/62 | 16/16 |

Varnish Cache 9.0.x and Vinyl Cache 9.0.x use the same API as 9.1 and were also tested.

The Varnish API differs between Enterprise 6.0 and the 9.x line in how header names are written
and how the cached request body is read; `src/compat.h` covers both. Vinyl Cache is the renamed
open-source line (`vinyld`, `libvinylapi`, the `vinyl` test command). Vinyl Cache has no backend
TLS, so put a TLS sidecar such as stunnel or haproxy in front of an HTTPS origin.

## License

BSD 2-Clause. See [`LICENSE`](LICENSE).
