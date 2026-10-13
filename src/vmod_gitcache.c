#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cache/cache.h"
#include "vcl.h"
#include "vrt_obj.h"
#ifdef GC_HAVE_OPENSSL
#include <openssl/evp.h>
#else
#include "vsha256.h"
#endif

#include "pktline.h"
#include "route.h"
#include "compat.h"
#include "vcc_gitcache_if.h"

#define GC_DIGEST_LEN	32
#define GC_PRINT_LEN	8		/* bytes of SHA-256 in a credential or query fingerprint */
#define GC_KIND_LIMIT	((size_t)10485760)
#define GC_MAXKEYS	16		/* Git-Protocol keys besides the version */
#define GC_PACK_CTYPE	"application/x-git-upload-pack-request"
#define GC_PACK_SFX	"/git-upload-pack"

#define KF_PACK		1		/* shared behind the preflight; pack_ttl */
#define KF_REFS		2		/* changes on push: refs_ttl, no grace */
#define KF_LFSB		4		/* lfs_batch_ttl, no grace */
#define KF_POST		8		/* fetched with POST */

static const struct {
	const char	*name;
	unsigned	flags;
} kinds[] = {
	{ "info-refs",		KF_REFS },
	{ "dumb-refs",		KF_REFS },
	{ "ls-refs",		KF_REFS | KF_POST },
	{ "bundle-uri",		KF_REFS | KF_POST },
	{ "fetch",		KF_PACK | KF_POST },
	{ "upload-pack",	KF_PACK | KF_POST },
	{ "object-info",	KF_PACK | KF_POST },
	{ "lfs-batch",		KF_LFSB | KF_POST },
};

static const char * const knames[] = {
	[K_NONE] =		"",
	[K_INFO_REFS] =		"info-refs",
	[K_INFO_REFS_PUSH] =	"info-refs-push",
	[K_UPLOAD_PACK] =	"upload-pack",
	[K_RECEIVE_PACK] =	"receive-pack",
	[K_LFS_BATCH] =		"lfs-batch",
	[K_LFS_OBJECT] =	"lfs-object",
	[K_LFS_OTHER] =		"lfs-other",
	[K_DUMB_REFS] =		"dumb-refs",
	[K_DUMB_OBJECT] =	"dumb-object",
};

/* v2 commands that are a kind of their own; any other command is a fetch */
static const char * const v2cmds[] = { "ls-refs", "object-info", "bundle-uri", NULL };

/* Per-request state, kept in request headers across restarts; the backend needs the last two. */
static const gc_hdr_t state[] = {
	GC_H_STAGE, GC_H_ORIGURL, GC_H_ORIGMETH, GC_H_OKIND, GC_H_FINAL, GC_H_KEY,
	GC_H_KIND, GC_H_REPO,
};
#define GC_NSTATE	(sizeof state / sizeof state[0])
#define GC_NFETCH	(GC_NSTATE - 2)

struct gc_sha {
#ifdef GC_HAVE_OPENSSL
	EVP_MD_CTX	*c;
#else
	VSHA256_CTX	c;
#endif
};

#ifdef GC_HAVE_OPENSSL
/*
 * OpenSSL 3 looks the algorithm up in its providers on every EVP_sha256() use, which costs
 * more than hashing a typical request (252 ns against 117 ns for 207 bytes). Fetch it once.
 */
static const EVP_MD *
sha256_md(void)
{
#if defined(OPENSSL_VERSION_MAJOR) && OPENSSL_VERSION_MAJOR >= 3
	static EVP_MD *md;
	EVP_MD *m = __atomic_load_n(&md, __ATOMIC_ACQUIRE), *expected = NULL;

	if (m != NULL)
		return (m);
	m = EVP_MD_fetch(NULL, "SHA256", NULL);
	if (m == NULL)
		return (EVP_sha256());
	if (!__atomic_compare_exchange_n(&md, &expected, m, 0, __ATOMIC_ACQ_REL,
	    __ATOMIC_ACQUIRE)) {
		EVP_MD_free(m);
		m = expected;
	}
	return (m);
#else
	return (EVP_sha256());
#endif
}

static void *
md_new(void)
{
	return (EVP_MD_CTX_new());
}

static void
md_free(void *p)
{
	EVP_MD_CTX_free(p);
}

static int
sha_begin(struct gc_sha *s)
{
	s->c = gc_tls_obj(GC_OBJ_DIGEST, md_new, md_free);
	return (s->c != NULL && EVP_DigestInit_ex(s->c, sha256_md(), NULL) == 1 ? 0 : -1);
}

static void
sha_update(struct gc_sha *s, const void *p, size_t n)
{
	(void)EVP_DigestUpdate(s->c, p, n);
}

static void
sha_end(struct gc_sha *s, uint8_t *out)
{
	unsigned l;

	(void)EVP_DigestFinal_ex(s->c, out, &l);
}
#else
static int
sha_begin(struct gc_sha *s)
{
	VSHA256_Init(&s->c);
	return (0);
}

static void
sha_update(struct gc_sha *s, const void *p, size_t n)
{
	VSHA256_Update(&s->c, p, n);
}

static void
sha_end(struct gc_sha *s, uint8_t *out)
{
	VSHA256_Final(out, &s->c);
}
#endif

/* Batches the canonicaliser's many small writes into fewer digest updates. */
struct hsink {
	struct gc_sha	sha;
	size_t		n;
	uint8_t		buf[4096];
};

static void
hsink_flush(struct hsink *h)
{
	if (h->n > 0)
		sha_update(&h->sha, h->buf, h->n);
	h->n = 0;
}

static int
hsink_write(void *priv, const void *p, size_t len)
{
	struct hsink *h = priv;

	if (h->n + len > sizeof h->buf)
		hsink_flush(h);
	if (len >= sizeof h->buf) {
		sha_update(&h->sha, p, len);
		return (0);
	}
	memcpy(h->buf + h->n, p, len);
	h->n += len;
	return (0);
}

static void *
ws_alloc(VRT_CTX, size_t n)
{
	void *p = n <= UINT_MAX ? WS_Alloc(ctx->ws, (unsigned)n) : NULL;

	if (p == NULL)
		VRT_fail(ctx, "gitcache: out of workspace");
	return (p);
}

struct piece {
	const char	*p;
	size_t		n;
};

#define PC(s)	((struct piece){ (s), sizeof(s) - 1 })
#define PS(s)	((struct piece){ (s), strlen(s) })

static char *
ws_cat(VRT_CTX, const struct piece *v, size_t cnt)
{
	size_t total = 1, i;
	char *s, *w;

	for (i = 0; i < cnt; i++)
		total += v[i].n;
	s = ws_alloc(ctx, total);
	if (s == NULL)
		return (NULL);
	for (w = s, i = 0; i < cnt; w += v[i].n, i++)
		if (v[i].n > 0)
			memcpy(w, v[i].p, v[i].n);
	*w = '\0';
	return (s);
}

static const char *
ws_hex(VRT_CTX, const uint8_t *d, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	char *s = ws_alloc(ctx, n * 2 + 1);
	size_t i;

	if (s == NULL)
		return (NULL);
	for (i = 0; i < n; i++) {
		s[2 * i] = hx[d[i] >> 4];
		s[2 * i + 1] = hx[d[i] & 0xf];
	}
	s[2 * n] = '\0';
	return (s);
}

static const char *
no_digest(VRT_CTX)
{
	VRT_fail(ctx, "gitcache: SHA-256 unavailable");
	return (NULL);
}

/* Hex of the first n bytes of the SHA-256 of p. */
static const char *
ws_sha(VRT_CTX, const void *p, size_t len, size_t n)
{
	struct gc_sha s;
	uint8_t dig[GC_DIGEST_LEN];

	if (sha_begin(&s))
		return (no_digest(ctx));
	sha_update(&s, p, len);
	sha_end(&s, dig);
	return (ws_hex(ctx, dig, n));
}

static const char *
ws_lower(VRT_CTX, const char *s, size_t n)
{
	char *d = ws_alloc(ctx, n + 1);
	size_t i;

	if (d == NULL)
		return (NULL);
	for (i = 0; i < n; i++)
		d[i] = (char)tolower((unsigned char)s[i]);
	d[n] = '\0';
	return (d);
}

static const char *
hget(const struct http *hp, gc_hdr_t h)
{
	const char *p;

	if (hp != NULL && http_GetHdr(hp, h, &p))
		return (p);
	return (NULL);
}

static int
hseq(const struct http *hp, gc_hdr_t h, const char *v)
{
	const char *p = hget(hp, h);

	return (p != NULL && strcmp(p, v) == 0);
}

static unsigned
hcount(const struct http *hp, gc_hdr_t h)
{
	unsigned u, n = 0;

	for (u = HTTP_HDR_FIRST; u < hp->nhd; u++)
		if (hp->hd[u].b != NULL && GC_IS_HDR(&hp->hd[u], h))
			n++;
	return (n);
}

static void
unset(struct http *hp, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		http_Unset(hp, state[i]);
}

static unsigned
kind_flags(const char *k)
{
	size_t i;

	for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
		if (strcmp(k, kinds[i].name) == 0)
			return (kinds[i].flags);
	return (0);
}

/*
 * Reads Git-Protocol the way git does (a version is an exact "version=N" key) into the version
 * and the tag that keys refs and packs: "v<N>" and any other keys, sorted. The tag is NULL when
 * the origin could read the header differently: it is repeated, names two versions, has a
 * malformed or unknown version, or more keys than are kept.
 */
static int
proto_tag(VRT_CTX, int *ver, const char **tag)
{
	const struct http *hp = ctx->http_req;
	const char *h, *toks[GC_MAXKEYS], *x;
	char *buf, *t, *save, *o;
	size_t len, n = 0, i, j;
	int v = -1, amb;

	*ver = 0;
	*tag = "v0";
	h = hget(hp, GC_H_PROTO);
	if (h == NULL)
		return (0);
	amb = hcount(hp, GC_H_PROTO) > 1;
	len = strlen(h);
	buf = ws_alloc(ctx, len + 1);
	if (buf == NULL)
		return (-1);
	memcpy(buf, h, len + 1);
	for (t = strtok_r(buf, ":", &save); t != NULL; t = strtok_r(NULL, ":", &save)) {
		if (strlen(t) == 9 && strncmp(t, "version=", 8) == 0 && t[8] >= '0' &&
		    t[8] <= '2') {
			amb |= v >= 0 && v != t[8] - '0';
			v = t[8] - '0' > v ? t[8] - '0' : v;
			continue;
		}
		t += strspn(t, " ");
		if (*t == '\0')
			continue;
		if (strncmp(t, "version=", 8) == 0 || n == GC_MAXKEYS)
			amb = 1;
		else
			toks[n++] = t;
	}
	*ver = v > 0 ? v : 0;
	if (amb) {
		*tag = NULL;
		return (0);
	}
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && strcmp(toks[j - 1], toks[j]) > 0; j--) {
			x = toks[j];
			toks[j] = toks[j - 1];
			toks[j - 1] = x;
		}
	o = ws_alloc(ctx, len + 4);
	if (o == NULL)
		return (-1);
	*tag = o;
	*o++ = 'v';
	*o++ = (char)('0' + *ver);
	for (i = 0; i < n; i++) {
		*o++ = '+';
		j = strlen(toks[i]);
		memcpy(o, toks[i], j);
		o += j;
	}
	*o = '\0';
	return (0);
}

/*
 * One analysis per request: the URL is routed once, the body is read and decoded once into
 * reusable per-thread buffers, and the canonical hash is computed once.
 */
struct gc_an {
	struct gc_req	r;
	const char	*kind;
	const char	*repo;
	const char	*ptag;		/* NULL if Git-Protocol is ambiguous */
	const char	*bhash;
	const uint8_t	*body;
	size_t		blen;
	int		proto;
	int		fail;
	int		bstate;		/* 0 not read yet, 1 read, -1 unusable */
};

static void
an_init(VRT_CTX, struct gc_an *an)
{
	const struct http *hp = ctx->http_req;

	memset(an, 0, sizeof *an);
	an->ptag = "v0";
	if (hp != NULL)
		gc_route(hp->hd[HTTP_HDR_URL].b, http_GetMethod(hp), &an->r);
	if (an->r.k != K_NONE && proto_tag(ctx, &an->proto, &an->ptag))
		an->fail = 1;
}

static void
an_done(struct gc_an *an)
{
	(void)an;
	gc_tls_release(GC_SLOT_BODY);
	gc_tls_release(GC_SLOT_INFLATE);
}

static int
an_load(VRT_CTX, struct gc_an *an, size_t limit)
{
	struct req *req = ctx->req;
	struct gc_buf *raw, *inf;
	const char *enc;
	uint64_t blen;

	if (req == NULL || ctx->http_req == NULL)
		return (-1);
	/* only a body of known size within the limit is buffered: caching anything else
	 * fails and discards it, so chunked bodies bypass and reach the origin untouched */
	if (!GC_BODY_CACHED(req)) {
		ssize_t cl = GC_BODY_NONE(req) ? -1 : http_GetContentLength(ctx->http_req);

		if (cl < 0 || cl > (ssize_t)limit || VRT_CacheReqBody(ctx, (VCL_BYTES)limit) < 0)
			return (-1);
	}
	if (!GC_BODY_CACHED(req) || req->body_oc == NULL)
		return (-1);
	blen = ObjGetLen(req->wrk, req->body_oc);
	if (blen > (uint64_t)limit)
		return (-1);
	raw = gc_tls_buf(GC_SLOT_BODY, (size_t)blen + 1);
	if (raw->oom || gc_read_body(req, raw) || raw->oom)
		return (-1);
	an->body = raw->p;
	an->blen = raw->len;
	enc = hget(ctx->http_req, GC_H_CENC);
	if (enc == NULL || strcasecmp(enc, "identity") == 0)
		return (0);
	if (strcasecmp(enc, "gzip") != 0 && strcasecmp(enc, "x-gzip") != 0)
		return (-1);
	inf = gc_tls_buf(GC_SLOT_INFLATE, 0);
	if (gc_gunzip_to(raw->p, raw->len, inf, limit))
		return (-1);
	an->body = inf->p;
	an->blen = inf->len;
	return (0);
}

static int
an_body(VRT_CTX, struct gc_an *an, size_t limit)
{
	if (an->bstate == 0)
		an->bstate = an_load(ctx, an, limit) ? -1 : 1;
	return (an->bstate > 0 ? 0 : -1);
}

static const char *
pack_kind(VRT_CTX, struct gc_an *an, size_t limit)
{
	const char * const *c;
	char cmd[32];

	if (an_body(ctx, an, limit) == 0 && an->blen == 4 && memcmp(an->body, "0000", 4) == 0)
		return ("probe");
	if (an->proto != 2)
		return ("upload-pack");
	if (an->bstate > 0 && gc_v2_command(an->body, an->blen, cmd, sizeof cmd) == 0)
		for (c = v2cmds; *c != NULL; c++)
			if (strcmp(cmd, *c) == 0)
				return (*c);
	return ("fetch");
}

static const char *
an_kind(VRT_CTX, struct gc_an *an, size_t limit)
{
	if (an->kind == NULL)
		an->kind = an->r.k == K_UPLOAD_PACK ? pack_kind(ctx, an, limit) : knames[an->r.k];
	return (an->kind);
}

static const char *
an_repo(VRT_CTX, struct gc_an *an)
{
	if (an->repo == NULL && an->r.k != K_NONE)
		an->repo = ws_lower(ctx, an->r.repo, an->r.repolen);
	return (an->repo);
}

/* "" if the body has no canonical form, NULL if the request failed. */
static const char *
an_bodyhash(VRT_CTX, struct gc_an *an, size_t limit)
{
	struct hsink h;
	uint8_t dig[GC_DIGEST_LEN];

	if (an->fail)
		return (NULL);
	if (an->bhash != NULL)
		return (an->bhash);
	if (an->ptag == NULL || an_body(ctx, an, limit) != 0 || an->blen == 0)
		return (an->bhash = "");
	if (sha_begin(&h.sha))
		return (no_digest(ctx));
	h.n = 0;
	if (gc_canonicalize_sink(an->body, an->blen, an->proto, hsink_write, &h) != 0)
		return (an->bhash = "");
	hsink_flush(&h);
	sha_end(&h.sha, dig);
	return (an->bhash = ws_hex(ctx, dig, sizeof dig));
}

static const char *
lfs_hash(VRT_CTX, const struct gc_an *an)
{
	struct gc_buf c;
	const char *h = "";

	gc_buf_init(&c);
	if (gc_lfs_canon(an->body, an->blen, &c) == 0)
		h = ws_sha(ctx, c.p, c.len, GC_DIGEST_LEN);
	gc_buf_fini(&c);
	return (h);
}

static const char *
credential(VRT_CTX)
{
	const char *a = hget(ctx->http_req, GC_H_AUTH);

	if (a == NULL || *a == '\0')
		return ("");
	return (ws_sha(ctx, a, strlen(a), GC_PRINT_LEN));
}

static int
pack_ctype(VRT_CTX)
{
	const char *ct = hget(ctx->http_req, GC_H_CTYPE);

	return (ct != NULL && strncasecmp(ct, GC_PACK_CTYPE, sizeof GC_PACK_CTYPE - 1) == 0);
}

/* The key after "<scope>/<repo>.git/" into v: piece count, 0 if uncacheable, -1 on failure. */
static int
an_tail(VRT_CTX, struct gc_an *an, size_t limit, int lfs_batch, struct piece *v)
{
	const struct gc_req *r = &an->r;
	const char *h;
	int n = 0;

	switch (r->k) {
	case K_INFO_REFS:
		if (an->ptag == NULL)
			return (0);
		v[0] = PC("info/refs?service=git-upload-pack/");
		v[1] = PS(an->ptag);
		return (2);
	case K_DUMB_REFS:
	case K_DUMB_OBJECT:
		v[0] = (struct piece){ r->tail, r->taillen };
		return (1);
	case K_LFS_OBJECT:
		if (r->href)
			v[n++] = PC("lfs-href/");
		v[n++] = (struct piece){ r->tail, r->taillen };
		if (r->query != NULL) {
			h = ws_sha(ctx, r->query, strlen(r->query), GC_PRINT_LEN);
			if (h == NULL)
				return (-1);
			v[n++] = PC("?q=");
			v[n++] = PS(h);
		}
		return (n);
	case K_LFS_BATCH:
		if (!lfs_batch || an_body(ctx, an, limit) != 0 ||
		    !gc_lfs_is_download(an->body, an->blen))
			return (0);
		h = lfs_hash(ctx, an);
		if (h == NULL || *h == '\0')
			return (h == NULL ? -1 : 0);
		v[0] = PC("info/lfs/objects/batch/");
		v[1] = PS(h);
		return (2);
	case K_UPLOAD_PACK:
		if (!pack_ctype(ctx))
			return (0);
		h = an_bodyhash(ctx, an, limit);
		if (h == NULL || *h == '\0')
			return (h == NULL ? -1 : 0);
		v[0] = PC("git-upload-pack/");
		v[1] = PS(an->ptag);
		v[2] = PC("/");
		v[3] = PS(h);
		return (4);
	default:
		return (0);
	}
}

static const char *
an_key(VRT_CTX, struct gc_an *an, size_t limit, const char *scope, int share, int lfs_batch)
{
	struct piece v[12];
	const char *repo, *cred;
	int n;

	if (an->fail)
		return (NULL);
	n = an_tail(ctx, an, limit, lfs_batch, v + 4);
	if (n <= 0)
		return (n < 0 ? NULL : "");
	repo = an_repo(ctx, an);
	cred = share ? "" : credential(ctx);
	if (repo == NULL || cred == NULL)
		return (NULL);
	v[0] = PS(scope != NULL ? scope : "");
	v[1] = PC("/");
	v[2] = PS(repo);
	v[3] = PC(".git/");
	n += 4;
	if (*cred != '\0') {
		v[n++] = PC("#cred=");
		v[n++] = PS(cred);
	}
	return (ws_cat(ctx, v, (size_t)n));
}

VCL_STRING
vmod_kind(VRT_CTX)
{
	struct gc_an an;
	const char *k;

	an_init(ctx, &an);
	k = an_kind(ctx, &an, GC_KIND_LIMIT);
	an_done(&an);
	return (k);
}

VCL_STRING
vmod_repo(VRT_CTX)
{
	struct gc_an an;

	an_init(ctx, &an);
	return (an.r.k == K_NONE ? "" : an_repo(ctx, &an));
}

VCL_INT
vmod_protocol(VRT_CTX)
{
	const char *t;
	int v;

	(void)proto_tag(ctx, &v, &t);
	return (v);
}

VCL_STRING
vmod_body_hash(VRT_CTX, VCL_BYTES limit)
{
	struct gc_an an;
	const char *h = "";

	an_init(ctx, &an);
	if (an.r.k == K_UPLOAD_PACK)
		h = an_bodyhash(ctx, &an, (size_t)limit);
	an_done(&an);
	return (h);
}

VCL_STRING
vmod_credential(VRT_CTX)
{
	return (credential(ctx));
}

VCL_STRING
vmod_cache_key(VRT_CTX, VCL_BYTES limit, VCL_STRING scope,
    VCL_BOOL share_credentials, VCL_BOOL lfs_batch)
{
	struct gc_an an;
	const char *k;

	an_init(ctx, &an);
	k = an_key(ctx, &an, (size_t)limit, scope, share_credentials, lfs_batch);
	an_done(&an);
	return (k);
}

struct vmod_gitcache_cache {
	unsigned	magic;
#define GC_CACHE_MAGIC	0x3b6f1a7d
	char		*scope;
	char		*purge_token;
	double		refs_ttl;
	double		pack_ttl;
	double		lfs_ttl;
	int		preflight;
	int		lfs_batch;
	int		debug;
	size_t		limit;
	double		transfer;
};

VCL_VOID
vmod_cache__init(VRT_CTX, struct vmod_gitcache_cache **objp, const char *vcl_name,
    VCL_STRING scope, VCL_DURATION refs_ttl, VCL_DURATION pack_ttl,
    VCL_DURATION lfs_batch_ttl, VCL_BOOL preflight, VCL_BOOL lfs_batch,
    VCL_BOOL debug, VCL_STRING purge_token, VCL_BYTES limit, VCL_DURATION transfer_timeout)
{
	struct vmod_gitcache_cache *c;

	(void)vcl_name;
	AN(objp);
	AZ(*objp);
	c = calloc(1, sizeof *c);
	if (c != NULL) {
		c->scope = strdup(scope != NULL ? scope : "");
		c->purge_token = strdup(purge_token != NULL ? purge_token : "");
	}
	if (c == NULL || c->scope == NULL || c->purge_token == NULL) {
		if (c != NULL) {
			free(c->scope);
			free(c->purge_token);
			free(c);
		}
		VRT_fail(ctx, "gitcache: out of memory");
		return;
	}
	c->magic = GC_CACHE_MAGIC;
	c->refs_ttl = refs_ttl;
	c->pack_ttl = pack_ttl;
	c->lfs_ttl = lfs_batch_ttl;
	c->preflight = preflight;
	c->lfs_batch = lfs_batch;
	c->debug = debug;
	c->limit = (size_t)limit;
	c->transfer = transfer_timeout;
	*objp = c;
}

VCL_VOID
vmod_cache__fini(struct vmod_gitcache_cache **objp)
{
	struct vmod_gitcache_cache *c;

	if (objp == NULL || *objp == NULL)
		return;
	c = *objp;
	*objp = NULL;
	CHECK_OBJ(c, GC_CACHE_MAGIC);
	free(c->scope);
	free(c->purge_token);
	free(c);
}

/*
 * The state headers can be trusted once the first .recv() of the task has removed the client's
 * copies: req.restarts cannot tell, since VCL may restart before .recv() ever runs.
 */
static int
task_first(VRT_CTX, struct vmod_gitcache_cache *c)
{
	struct vmod_priv *p = VRT_priv_task(ctx, c);

	if (p == NULL) {
		VRT_fail(ctx, "gitcache: out of workspace");
		return (-1);
	}
	if (p->priv != NULL)
		return (0);
	p->priv = c;
	return (1);
}

/* After a restart: serve the pack a preflight cleared, or restore one cut short in preflight. */
static int
resume(struct http *hp)
{
	const char *st = hget(hp, GC_H_STAGE), *url, *meth, *kind, *key;

	if (st == NULL)
		return (0);
	url = hget(hp, GC_H_ORIGURL);
	meth = hget(hp, GC_H_ORIGMETH);
	kind = hget(hp, GC_H_OKIND);
	key = hget(hp, GC_H_FINAL);
	if (url == NULL || meth == NULL)
		return (0);
	http_SetH(hp, HTTP_HDR_URL, url);
	http_SetH(hp, HTTP_HDR_METHOD, meth);
	if (strcmp(st, "go") != 0 || kind == NULL || key == NULL)
		return (0);
	http_ForceHeader(hp, GC_H_KIND, kind);
	http_ForceHeader(hp, GC_H_KEY, key);
	return (1);
}

/* Turns a pack request into a GET of the caller's own info/refs, to check access first. */
static int
preflight(VRT_CTX, const struct vmod_gitcache_cache *c, struct http *hp, struct gc_an *an,
    const char *kind, const char *key)
{
	const char *url = hp->hd[HTTP_HDR_URL].b, *meth = hp->hd[HTTP_HDR_METHOD].b, *refs, *k;
	size_t n = strcspn(url, "?"), sl = sizeof GC_PACK_SFX - 1;

	if (n > sl && memcmp(url + n - sl, GC_PACK_SFX, sl) == 0)
		n -= sl;
	refs = WS_Printf(ctx->ws, "%.*s/info/refs?service=git-upload-pack", (int)n, url);
	if (refs == NULL) {
		VRT_fail(ctx, "gitcache: out of workspace");
		return (0);
	}
	http_SetH(hp, HTTP_HDR_URL, refs);
	http_SetH(hp, HTTP_HDR_METHOD, "GET");
	an_init(ctx, an);
	k = an_key(ctx, an, c->limit, c->scope, 0, c->lfs_batch);
	if (k == NULL || *k == '\0') {
		http_SetH(hp, HTTP_HDR_URL, url);
		http_SetH(hp, HTTP_HDR_METHOD, meth);
		unset(hp, GC_NSTATE);
		return (0);
	}
	http_ForceHeader(hp, GC_H_ORIGURL, url);
	http_ForceHeader(hp, GC_H_ORIGMETH, meth);
	http_ForceHeader(hp, GC_H_OKIND, kind);
	http_ForceHeader(hp, GC_H_FINAL, key);
	http_ForceHeader(hp, GC_H_STAGE, "preflight");
	http_ForceHeader(hp, GC_H_KIND, "info-refs");
	http_ForceHeader(hp, GC_H_KEY, k);
	return (1);
}

static int
classify(VRT_CTX, const struct vmod_gitcache_cache *c, struct http *hp)
{
	struct gc_an an;
	const char *kind, *key, *repo;
	int pack, ok = 0;

	an_init(ctx, &an);
	kind = an_kind(ctx, &an, c->limit);
	if (*kind == '\0')
		goto done;
	pack = c->preflight && (kind_flags(kind) & KF_PACK);
	key = an_key(ctx, &an, c->limit, c->scope, pack, c->lfs_batch);
	if (key == NULL || *key == '\0')
		goto done;
	repo = an_repo(ctx, &an);
	if (repo == NULL)
		goto done;
	http_ForceHeader(hp, GC_H_KIND, kind);
	http_ForceHeader(hp, GC_H_REPO, repo);
	if (pack) {
		ok = preflight(ctx, c, hp, &an, kind, key);
	} else {
		http_ForceHeader(hp, GC_H_KEY, key);
		ok = 1;
	}
done:
	an_done(&an);
	return (ok);
}

VCL_BOOL
vmod_cache_recv(VRT_CTX, struct vmod_gitcache_cache *c)
{
	struct http *hp = ctx->http_req;
	int first;

	CHECK_OBJ_NOTNULL(c, GC_CACHE_MAGIC);
	if (hp == NULL)
		return (0);
	first = task_first(ctx, c);
	if (first < 0)
		return (0);
	if (!first && resume(hp))
		return (1);
	unset(hp, GC_NSTATE);
	return (classify(ctx, c, hp));
}

VCL_BOOL
vmod_cache_hash(VRT_CTX, struct vmod_gitcache_cache *c)
{
	struct vmod_priv *p;
	const char *key;

	CHECK_OBJ_NOTNULL(c, GC_CACHE_MAGIC);
	key = hget(ctx->http_req, GC_H_KEY);
	if (key == NULL)
		return (0);
	p = VRT_priv_task(ctx, c);
	if (p == NULL) {
		VRT_fail(ctx, "gitcache: out of workspace");
		return (0);
	}
	if (p->priv == NULL)
		return (0);
	gc_hashdata(ctx, key);
	return (1);
}

VCL_VOID
vmod_cache_backend_fetch(VRT_CTX, struct vmod_gitcache_cache *c)
{
	struct http *hp = ctx->http_bereq;
	const char *kind;

	CHECK_OBJ_NOTNULL(c, GC_CACHE_MAGIC);
	kind = hget(hp, GC_H_KIND);
	if (kind == NULL)
		return;
	if (hseq(hp, GC_H_STAGE, "preflight")) {
		VRT_u_bereq_body(ctx);
		http_Unset(hp, GC_H_CTYPE);
		http_Unset(hp, GC_H_CENC);
	} else {
		if (kind_flags(kind) & KF_POST)
			http_SetH(hp, HTTP_HDR_METHOD, "POST");
		if ((kind_flags(kind) & KF_PACK) && c->transfer > 0)
			VRT_l_bereq_first_byte_timeout(ctx, c->transfer);
	}
	unset(hp, GC_NFETCH);
}

VCL_BOOL
vmod_cache_backend_response(VRT_CTX, struct vmod_gitcache_cache *c)
{
	const char *kind, *repo;
	unsigned f;

	CHECK_OBJ_NOTNULL(c, GC_CACHE_MAGIC);
	kind = hget(ctx->http_bereq, GC_H_KIND);
	if (kind == NULL)
		return (0);
	repo = hget(ctx->http_bereq, GC_H_REPO);
	if (repo != NULL)
		http_ForceHeader(ctx->http_beresp, GC_H_REPO, repo);
	if (VRT_r_beresp_status(ctx) != 200) {
		VRT_l_beresp_uncacheable(ctx, 1);
		VRT_l_beresp_ttl(ctx, 0);
		return (1);
	}
	f = kind_flags(kind);
	VRT_l_beresp_ttl(ctx, (f & KF_REFS) ? c->refs_ttl : (f & KF_LFSB) ? c->lfs_ttl :
	    c->pack_ttl);
	if (f & (KF_REFS | KF_LFSB))
		VRT_l_beresp_grace(ctx, 0);
	return (1);
}

VCL_BOOL
vmod_cache_deliver(VRT_CTX, struct vmod_gitcache_cache *c)
{
	const char *kind, *hits;

	CHECK_OBJ_NOTNULL(c, GC_CACHE_MAGIC);
	http_Unset(ctx->http_resp, GC_H_REPO);
	kind = hget(ctx->http_req, GC_H_KIND);
	if (kind != NULL && (kind_flags(kind) & KF_PACK) && c->transfer > 0)
		GC_SEND_TIMEOUT(ctx, c->transfer);
	if (kind != NULL && c->debug) {
		hits = WS_Printf(ctx->ws, "%ld", (long)VRT_r_obj_hits(ctx));
		if (hits == NULL) {
			VRT_fail(ctx, "gitcache: out of workspace");
			return (0);
		}
		http_ForceHeader(ctx->http_resp, GC_H_KIND, kind);
		http_ForceHeader(ctx->http_resp, GC_H_HITS, hits);
	}
	if (hseq(ctx->http_req, GC_H_STAGE, "preflight") && VRT_r_resp_status(ctx) == 200) {
		http_ForceHeader(ctx->http_req, GC_H_STAGE, "go");
		return (1);
	}
	return (0);
}

static int
safe_eq(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b), i;
	unsigned char d = (unsigned char)(la != lb);

	for (i = 0; i < lb; i++)
		d |= (unsigned char)(a[i % (la ? la : 1)] ^ b[i]);
	return (d == 0);
}

static const char *
query_param(const char *url, const char *key, size_t *n)
{
	size_t kl = strlen(key);
	const char *q = strchr(url, '?');

	while (q != NULL) {
		q++;
		if (strncmp(q, key, kl) == 0) {
			*n = strcspn(q + kl, "&");
			return (q + kl);
		}
		q = strchr(q, '&');
	}
	return (NULL);
}

/* Decodes a purge target into d: owner/name, lowercased, '/' possibly sent as %2F. */
static int
purge_name(const char *q, size_t n, char *d)
{
	size_t i, o = 0;

	for (i = 0; i < n; i++) {
		if (q[i] == '%' && i + 2 < n && q[i + 1] == '2' &&
		    tolower((unsigned char)q[i + 2]) == 'f') {
			d[o++] = '/';
			i += 2;
		} else {
			d[o++] = (char)tolower((unsigned char)q[i]);
		}
	}
	d[o] = '\0';
	if (o == 0 || d[0] == '/' || d[o - 1] == '/' || strstr(d, "//") != NULL ||
	    strstr(d, "..") != NULL || strchr(d, '/') == NULL)
		return (-1);
	for (i = 0; i < o; i++)
		if (!isalnum((unsigned char)d[i]) && strchr("/._-", d[i]) == NULL)
			return (-1);
	return (0);
}

VCL_INT
vmod_cache_purge(VRT_CTX, struct vmod_gitcache_cache *c)
{
	const struct http *hp = ctx->http_req;
	const char *tok, *q, *expr;
	char *repo;
	size_t n;

	CHECK_OBJ_NOTNULL(c, GC_CACHE_MAGIC);
	if (hp == NULL || strcmp(http_GetMethod(hp), "POST") != 0)
		return (405);
	tok = hget(hp, GC_H_PURGE);
	if (*c->purge_token == '\0' || tok == NULL || !safe_eq(tok, c->purge_token))
		return (403);
	q = query_param(hp->hd[HTTP_HDR_URL].b, "repo=", &n);
	if (q == NULL)
		return (400);
	repo = WS_Alloc(ctx->ws, (unsigned)(n + 1));
	if (repo == NULL)
		return (500);
	if (purge_name(q, n, repo))
		return (400);
	expr = WS_Printf(ctx->ws, "obj.http.X-GC-Repo == %s", repo);
	if (expr == NULL)
		return (500);
	return (gc_ban(ctx, expr) == 0 ? 200 : 500);
}
