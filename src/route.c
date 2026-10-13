#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "libdeflate.h"

#include "route.h"

#define LFS_DIR		"/info/lfs/"
#define OBJ_DIR		"/objects/"

/* K_INFO_REFS stands for every info/refs flavour; the query decides which one. */
static const struct {
	const char	*sfx;
	const char	*meth;
	enum gc_kind	k;
} endpoints[] = {
	{ "/git-receive-pack",			"POST",	K_RECEIVE_PACK },
	{ "/git-upload-pack",			"POST",	K_UPLOAD_PACK },
	{ "/info/refs",				"GET",	K_INFO_REFS },
	{ "/objects/info/packs",		"GET",	K_DUMB_REFS },
	{ "/objects/info/alternates",		"GET",	K_DUMB_REFS },
	{ "/objects/info/http-alternates",	"GET",	K_DUMB_REFS },
	{ "/HEAD",				"GET",	K_DUMB_REFS },
};

static int
eq(const char *p, const char *e, const char *s)
{
	size_t n = strlen(s);

	return ((size_t)(e - p) == n && memcmp(p, s, n) == 0);
}

static int
ends(const char *p, const char *e, const char *s)
{
	size_t n = strlen(s);

	return ((size_t)(e - p) > n && memcmp(e - n, s, n) == 0);
}

static const char *
find(const char *p, const char *e, const char *s, int last)
{
	size_t n = strlen(s);
	const char *r = NULL;

	for (; p + n <= e; p++) {
		if (memcmp(p, s, n) != 0)
			continue;
		r = p;
		if (!last)
			break;
	}
	return (r);
}

static int
has_param(const char *q, const char *kv)
{
	size_t n = strlen(kv);

	while (q != NULL && *q != '\0') {
		if (strncmp(q, kv, n) == 0 && (q[n] == '\0' || q[n] == '&'))
			return (1);
		q = strchr(q, '&');
		if (q != NULL)
			q++;
	}
	return (0);
}

static void
set_tail(struct gc_req *r, const char *p, const char *e)
{
	r->tail = p;
	r->taillen = (size_t)(e - p);
}

/* The repository is the URL up to e, without leading slashes or a .git suffix. */
static void
found(struct gc_req *r, enum gc_kind k, const char *url, const char *e)
{
	r->k = k;
	r->repo = url;
	r->repolen = (size_t)(e - url);
	while (r->repolen > 0 && *r->repo == '/') {
		r->repo++;
		r->repolen--;
	}
	if (r->repolen > 4 && memcmp(r->repo + r->repolen - 4, ".git", 4) == 0)
		r->repolen -= 4;
	if (r->repolen == 0)
		r->k = K_NONE;
}

static void
route_lfs(struct gc_req *r, const char *url, const char *m, const char *end, int get, int post)
{
	const char *p = m + strlen(LFS_DIR);
	enum gc_kind k = K_LFS_OTHER;

	if (post && eq(p, end, "objects/batch")) {
		k = K_LFS_BATCH;
	} else if (get && end - p == 8 + 64 && memcmp(p, "objects/", 8) == 0 &&
	    gc_is_hex(p + 8, 64)) {
		k = K_LFS_OBJECT;
		set_tail(r, p, end);
	}
	found(r, k, url, m);
}

static enum gc_kind
info_refs(const char *q)
{
	if (q == NULL)
		return (K_DUMB_REFS);
	if (has_param(q + 1, "service=git-receive-pack"))
		return (K_INFO_REFS_PUSH);
	if (has_param(q + 1, "service=git-upload-pack"))
		return (K_INFO_REFS);
	return (K_NONE);
}

/* A loose object (objects/xx/<hex>) or a pack file (objects/pack/pack-<sha>.{pack,idx,rev}). */
static int
is_dumb_object(const char *p, const char *end)
{
	size_t n = (size_t)(end - p);
	const char *h, *x;

	if (n >= 3 + 38 && n <= 3 + 62 && p[2] == '/' && gc_is_hex(p, 2) &&
	    gc_is_hex(p + 3, n - 3))
		return (1);
	if (n <= 10 || memcmp(p, "pack/pack-", 10) != 0)
		return (0);
	h = p + 10;
	for (x = h; x < end && gc_is_hex(x, 1); x++)
		continue;
	return ((x - h == 40 || x - h == 64) && *end == '\0' &&
	    (eq(x, end, ".pack") || eq(x, end, ".idx") || eq(x, end, ".rev")));
}

/* LFS transfer URLs are server-chosen; they end in the object's sha256, which clients verify. */
static void
route_href(struct gc_req *r, const char *url, const char *end, int get, int post)
{
	const char *s = NULL, *p;

	for (p = url; p < end; p++)
		if (*p == '/')
			s = p;
	if (s == NULL)
		return;
	if (get && end - s - 1 == 64 && gc_is_hex(s + 1, 64)) {
		r->href = 1;
		set_tail(r, s + 1, end);
		found(r, K_LFS_OBJECT, url, s);
	} else if (post && eq(s, end, "/verify")) {
		for (p = s; p > url && p[-1] != '/'; p--)
			continue;
		if (s - p == 64 && gc_is_hex(p, 64))
			found(r, K_LFS_OTHER, url, p);
	}
}

void
gc_route(const char *url, const char *meth, struct gc_req *r)
{
	const char *q, *end, *m;
	enum gc_kind k;
	size_t i;
	int get, post;

	memset(r, 0, sizeof *r);
	if (url == NULL || meth == NULL)
		return;
	get = strcmp(meth, "GET") == 0;
	post = strcmp(meth, "POST") == 0;
	q = strchr(url, '?');
	end = q != NULL ? q : url + strlen(url);
	if (q != NULL && q[1] != '\0')
		r->query = q + 1;

	m = find(url, end, LFS_DIR, 0);
	if (m != NULL) {
		route_lfs(r, url, m, end, get, post);
		return;
	}
	for (i = 0; i < sizeof endpoints / sizeof endpoints[0]; i++) {
		if (!ends(url, end, endpoints[i].sfx))
			continue;
		if (strcmp(meth, endpoints[i].meth) != 0)
			return;
		m = end - strlen(endpoints[i].sfx);
		k = endpoints[i].k == K_INFO_REFS ? info_refs(q) : endpoints[i].k;
		if (k == K_DUMB_REFS)
			set_tail(r, m + 1, end);
		if (k != K_NONE)
			found(r, k, url, m);
		return;
	}
	m = find(url, end, OBJ_DIR, 1);
	if (get && m != NULL && is_dumb_object(m + strlen(OBJ_DIR), end)) {
		set_tail(r, m + 1, end);
		found(r, K_DUMB_OBJECT, url, m);
		return;
	}
	route_href(r, url, end, get, post);
}

static void *
inflater_new(void)
{
	return (libdeflate_alloc_decompressor());
}

static void
inflater_free(void *p)
{
	libdeflate_free_decompressor(p);
}

int
gc_gunzip_to(const uint8_t *in, size_t n, struct gc_buf *out, size_t max)
{
	struct libdeflate_decompressor *d;
	size_t isize, got = 0;

	out->len = 0;
	/* 10 byte header + 8 byte trailer is the smallest gzip stream */
	if (n < 18)
		return (-1);
	/* the trailer holds the uncompressed size: reject a bomb before allocating anything */
	isize = (size_t)in[n - 4] | (size_t)in[n - 3] << 8 | (size_t)in[n - 2] << 16 |
	    (size_t)in[n - 1] << 24;
	if (isize > max || gc_buf_reserve(out, isize + 1))
		return (-1);
	d = gc_tls_obj(GC_OBJ_INFLATER, inflater_new, inflater_free);
	if (d == NULL || libdeflate_gzip_decompress(d, in, n, out->p, isize + 1, &got) !=
	    LIBDEFLATE_SUCCESS || got != isize)
		return (-1);
	out->len = got;
	return (0);
}

int
gc_gunzip(struct gc_buf *b, size_t max)
{
	struct gc_buf out;

	gc_buf_init(&out);
	if (gc_gunzip_to(b->p, b->len, &out, max)) {
		gc_buf_fini(&out);
		return (-1);
	}
	free(b->p);
	*b = out;
	return (0);
}

struct elem {
	const char	*p;
	size_t		n;
};

struct jp {
	const char	*s;
	size_t		n;
	size_t		i;
};

static int
elem_cmp(const void *a, const void *b)
{
	const struct elem *x = a, *y = b;

	return (gc_lencmp(x->p, x->n, y->p, y->n));
}

static int
elem_is(const struct elem *e, const char *s)
{
	return (gc_lencmp(e->p, e->n, s, strlen(s)) == 0);
}

static int
jp_at(const struct jp *p, char c)
{
	return (p->i < p->n && p->s[p->i] == c);
}

static void
jp_ws(struct jp *p)
{
	while (jp_at(p, ' ') || jp_at(p, '\t') || jp_at(p, '\n') || jp_at(p, '\r'))
		p->i++;
}

static int
jp_string(struct jp *p)
{
	char c;

	if (!jp_at(p, '"'))
		return (-1);
	p->i++;
	while (p->i < p->n) {
		c = p->s[p->i++];
		if (c == '"')
			return (0);
		if (c == '\\' && p->i++ >= p->n)
			return (-1);
	}
	return (-1);
}

/* Skip one JSON value, string- and nesting-aware. */
static int
jp_value(struct jp *p)
{
	size_t depth = 0;

	for (;;) {
		jp_ws(p);
		if (p->i >= p->n)
			return (-1);
		switch (p->s[p->i]) {
		case '"':
			if (jp_string(p))
				return (-1);
			break;
		case '{':
		case '[':
			depth++;
			p->i++;
			break;
		case '}':
		case ']':
			if (depth == 0)
				return (-1);
			depth--;
			p->i++;
			break;
		case ',':
		case ':':
			if (depth == 0)
				return (-1);
			p->i++;
			break;
		default:
			/* always consume one byte, so no input can stall the scan; strchr() also matches
			 * the terminating NUL, so a NUL byte must be excluded explicitly */
			p->i++;
			while (p->i < p->n && p->s[p->i] != '\0' &&
			    strchr(",:]} \t\r\n\"{[", p->s[p->i]) == NULL)
				p->i++;
			break;
		}
		if (depth == 0)
			return (0);
	}
}

static int
jp_open(struct jp *p)
{
	jp_ws(p);
	if (!jp_at(p, '{'))
		return (-1);
	p->i++;
	return (0);
}

/* Reads the next "key": value of an object; 1, 0 past its closing brace, -1 if malformed. */
static int
jp_member(struct jp *p, struct elem *k, struct elem *v)
{
	size_t st;

	jp_ws(p);
	if (jp_at(p, '}')) {
		p->i++;
		return (0);
	}
	st = p->i + 1;
	if (jp_string(p))
		return (-1);
	k->p = p->s + st;
	k->n = p->i - 1 - st;
	jp_ws(p);
	if (!jp_at(p, ':'))
		return (-1);
	p->i++;
	jp_ws(p);
	st = p->i;
	if (jp_value(p))
		return (-1);
	v->p = p->s + st;
	v->n = p->i - st;
	jp_ws(p);
	if (jp_at(p, ','))
		p->i++;
	else if (!jp_at(p, '}'))
		return (-1);
	return (1);
}

/* Emit the array a with its elements sorted. Returns 0, or -1 if it is not a clean array. */
static int
sort_array(const struct elem *a, struct gc_buf *out)
{
	struct jp p;
	struct elem *el;
	size_t ne = 0, k, st;
	int rc = -1;

	if (a->n < 2 || a->p[a->n - 1] != ']')
		return (-1);
	p.s = a->p;
	p.n = a->n - 1;
	p.i = 1;
	el = malloc(sizeof *el * a->n);
	if (el == NULL)
		return (-1);
	jp_ws(&p);
	while (p.i < p.n) {
		st = p.i;
		if (jp_value(&p))
			goto done;
		el[ne].p = p.s + st;
		el[ne++].n = p.i - st;
		jp_ws(&p);
		if (jp_at(&p, ',')) {
			p.i++;
			jp_ws(&p);
			if (p.i >= p.n)
				goto done;	/* trailing comma */
		} else if (p.i < p.n) {
			goto done;
		}
	}
	qsort(el, ne, sizeof *el, elem_cmp);
	if (gc_buf_add(out, "[", 1))
		goto done;
	for (k = 0; k < ne; k++)
		if ((k > 0 && gc_buf_add(out, ",", 1)) || gc_buf_add(out, el[k].p, el[k].n))
			goto done;
	rc = gc_buf_add(out, "]", 1) ? -1 : 0;
done:
	free(el);
	return (rc);
}

int
gc_lfs_is_download(const uint8_t *in, size_t n)
{
	struct jp p = { (const char *)in, n, 0 };
	struct elem k, v;
	int r, dl = 0;

	if (jp_open(&p))
		return (0);
	while ((r = jp_member(&p, &k, &v)) > 0)
		if (elem_is(&k, "operation"))
			dl = elem_is(&v, "\"download\"");
	return (r == 0 && dl);
}

int
gc_lfs_canon(const uint8_t *in, size_t n, struct gc_buf *out)
{
	struct jp p = { (const char *)in, n, 0 };
	struct elem k, v;
	size_t len0 = out->len, done = 0, vs, mark;
	int r = -1;

	if (jp_open(&p) == 0) {
		while ((r = jp_member(&p, &k, &v)) > 0) {
			if (*v.p != '[' || !(elem_is(&k, "transfers") || elem_is(&k, "objects")))
				continue;
			vs = (size_t)(v.p - p.s);
			mark = out->len;
			if (gc_buf_add(out, p.s + done, vs - done))
				return (-1);
			if (sort_array(&v, out) == 0)
				done = vs + v.n;
			else
				out->len = mark;	/* not a clean array: leave it as sent */
		}
		jp_ws(&p);
	}
	if (r == 0 && p.i == n)
		return (gc_buf_add(out, p.s + done, n - done) ? -1 : 0);
	out->len = len0;
	return (gc_buf_add(out, in, n) ? -1 : 0);
}
