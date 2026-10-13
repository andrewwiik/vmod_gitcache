#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pktline.h"

#define PKT_MAX		65520

enum { PKT_DATA, PKT_FLUSH, PKT_DELIM, PKT_END };

struct line {
	const uint8_t	*p;
	size_t		n;
};

struct tls_obj {
	void		*p;
	void		(*fr)(void *);
};

struct out {
	gc_sink_f	*f;
	void		*priv;
	int		err;
};

void
gc_buf_init(struct gc_buf *b)
{
	memset(b, 0, sizeof *b);
}

void
gc_buf_fini(struct gc_buf *b)
{
	free(b->p);
	memset(b, 0, sizeof *b);
}

int
gc_buf_reserve(struct gc_buf *b, size_t n)
{
	uint8_t *np;

	if (b->oom)
		return (-1);
	if (n <= b->cap)
		return (0);
	np = realloc(b->p, n);
	if (np == NULL) {
		b->oom = 1;
		return (-1);
	}
	b->p = np;
	b->cap = n;
	return (0);
}

int
gc_buf_add(struct gc_buf *b, const void *d, size_t n)
{
	size_t nc;

	if (b->oom)
		return (-1);
	if (b->len + n > b->cap) {
		nc = b->cap ? b->cap : 256;
		while (nc < b->len + n)
			nc *= 2;
		if (gc_buf_reserve(b, nc))
			return (-1);
	}
	if (n > 0)
		memcpy(b->p + b->len, d, n);
	b->len += n;
	return (0);
}

int
gc_is_hex(const void *v, size_t n)
{
	const uint8_t *p = v;
	size_t i;

	for (i = 0; i < n; i++)
		if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
			return (0);
	return (1);
}

int
gc_lencmp(const void *a, size_t an, const void *b, size_t bn)
{
	int c = memcmp(a, b, an < bn ? an : bn);

	if (c != 0)
		return (c);
	return (an < bn ? -1 : an > bn);
}

static __thread struct gc_buf tls_bufs[GC_NSLOTS];
static __thread struct tls_obj tls_objs[GC_NOBJS];
static __thread struct line *tls_lines;
static __thread size_t tls_nlines;
static __thread int tls_armed;

static pthread_key_t tls_key;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;
static int tls_keyed;

static void
tls_exit(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < GC_NSLOTS; i++)
		gc_buf_fini(&tls_bufs[i]);
	for (i = 0; i < GC_NOBJS; i++)
		if (tls_objs[i].p != NULL)
			tls_objs[i].fr(tls_objs[i].p);
	memset(tls_objs, 0, sizeof tls_objs);
	free(tls_lines);
	tls_lines = NULL;
	tls_nlines = 0;
	tls_armed = 0;
}

static void
tls_mkkey(void)
{
	tls_keyed = pthread_key_create(&tls_key, tls_exit) == 0;
}

/* The key's destructor is in this object, so the key must not outlive a dlclose(). */
static void __attribute__((destructor))
tls_unload(void)
{
	if (tls_keyed)
		(void)pthread_key_delete(tls_key);
}

static void
tls_arm(void)
{
	if (tls_armed)
		return;
	(void)pthread_once(&tls_once, tls_mkkey);
	if (tls_keyed && pthread_setspecific(tls_key, &tls_armed) == 0)
		tls_armed = 1;
}

struct gc_buf *
gc_tls_buf(int slot, size_t hint)
{
	struct gc_buf *b = &tls_bufs[slot];

	tls_arm();
	b->len = 0;
	b->oom = 0;
	if (hint > b->cap)
		(void)gc_buf_reserve(b, hint);
	return (b);
}

void
gc_tls_release(int slot)
{
	struct gc_buf *b = &tls_bufs[slot];

	if (b->cap > GC_KEEP)
		gc_buf_fini(b);
	b->len = 0;
}

void *
gc_tls_obj(int slot, void *(*mk)(void), void (*fr)(void *))
{
	struct tls_obj *o = &tls_objs[slot];

	if (o->p == NULL && (o->p = mk()) != NULL) {
		o->fr = fr;
		tls_arm();
	}
	return (o->p);
}

static struct line *
lines_get(size_t n)
{
	struct line *nl;

	tls_arm();
	if (n > tls_nlines) {
		nl = realloc(tls_lines, n * sizeof *nl);
		if (nl == NULL)
			return (NULL);
		tls_lines = nl;
		tls_nlines = n;
	}
	return (tls_lines);
}

static void
lines_release(void)
{
	if (tls_nlines * sizeof *tls_lines > GC_KEEP) {
		free(tls_lines);
		tls_lines = NULL;
		tls_nlines = 0;
	}
}

static int
hexval(uint8_t c)
{
	if (c >= '0' && c <= '9')
		return (c - '0');
	if (c >= 'a' && c <= 'f')
		return (c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (c - 'A' + 10);
	return (-1);
}

/* Reads a pkt-line into l (newline stripped) and its type; returns bytes used, 0 if invalid. */
static size_t
pkt_next(const uint8_t *in, size_t len, struct line *l, int *type)
{
	size_t n = 0;
	int i, v;

	if (len < 4)
		return (0);
	for (i = 0; i < 4; i++) {
		v = hexval(in[i]);
		if (v < 0)
			return (0);
		n = n * 16 + (size_t)v;
	}
	l->p = NULL;
	l->n = 0;
	if (n < PKT_END) {
		*type = (int)n + 1;
		return (4);
	}
	if (n < 4 || n > PKT_MAX || n > len)
		return (0);
	*type = PKT_DATA;
	l->p = in + 4;
	l->n = n - 4;
	if (l->n > 0 && l->p[l->n - 1] == '\n')
		l->n--;
	return (n);
}

/* Validates framing and returns the packet count (and the largest payload), or SIZE_MAX. */
static size_t
count_pkts(const uint8_t *in, size_t len, size_t *maxpay)
{
	struct line l;
	size_t n = 0, used;
	int t;

	*maxpay = 0;
	while (len > 0) {
		used = pkt_next(in, len, &l, &t);
		if (used == 0)
			return (SIZE_MAX);
		if (l.n > *maxpay)
			*maxpay = l.n;
		in += used;
		len -= used;
		n++;
	}
	return (n);
}

static int
line_cmp(const void *a, const void *b)
{
	const struct line *x = a, *y = b;

	return (gc_lencmp(x->p, x->n, y->p, y->n));
}

/* Sorts v and drops duplicates; returns the new count. */
static size_t
uniq(struct line *v, size_t n)
{
	size_t i, o;

	if (n == 0)
		return (0);
	qsort(v, n, sizeof *v, line_cmp);
	for (i = o = 1; i < n; i++)
		if (line_cmp(&v[o - 1], &v[i]) != 0)
			v[o++] = v[i];
	return (o);
}

static int
starts(const struct line *l, const char *s)
{
	size_t n = strlen(s);

	return (l->n >= n && memcmp(l->p, s, n) == 0);
}

static int
exact(const struct line *l, const char *s)
{
	return (l->n == strlen(s) && memcmp(l->p, s, l->n) == 0);
}

static int
any_prefix(const struct line *l, const char * const *v)
{
	for (; *v != NULL; v++)
		if (starts(l, *v))
			return (1);
	return (0);
}

static int
any_exact(const struct line *l, const char * const *v)
{
	for (; *v != NULL; v++)
		if (exact(l, *v))
			return (1);
	return (0);
}

static void
ow(struct out *o, const void *p, size_t n)
{
	if (!o->err && n > 0 && o->f(o->priv, p, n) != 0)
		o->err = 1;
}

static void
emit_head(struct out *o, size_t payload)
{
	static const char x[] = "0123456789abcdef";
	size_t v = payload + 5;
	char h[4];

	if (v > PKT_MAX) {
		o->err = 1;
		return;
	}
	h[0] = x[(v >> 12) & 15];
	h[1] = x[(v >> 8) & 15];
	h[2] = x[(v >> 4) & 15];
	h[3] = x[v & 15];
	ow(o, h, 4);
}

static void
emit_line(struct out *o, const struct line *l)
{
	emit_head(o, l->n);
	ow(o, l->p, l->n);
	ow(o, "\n", 1);
}

static void
emit_lines(struct out *o, const struct line *v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		emit_line(o, &v[i]);
}

static void
emit_special(struct out *o, int type)
{
	static const char * const s[] = { "", "0000", "0001", "0002" };

	ow(o, s[type], 4);
}

static int
is_telemetry(const struct line *l)
{
	static const char * const pre[] = { "agent=", "session-id=", NULL };

	return (any_prefix(l, pre));
}

static int
is_oid(const uint8_t *p, size_t n)
{
	return (n > 0 && n <= 64 && gc_is_hex(p, n));
}

static int
oid_line_ok(const struct line *l)
{
	static const char * const pre[] = { "want ", "have ", "oid ", "shallow ", NULL };
	const char * const *s;
	size_t n;

	for (s = pre; *s != NULL; s++) {
		n = strlen(*s);
		if (starts(l, *s))
			return (is_oid(l->p + n, l->n - n));
	}
	return (1);
}

static int
is_set_arg(const struct line *l)
{
	static const char * const pre[] = { "want ", "want-ref ", "shallow ",
	    "ref-prefix ", "oid ", "deepen", "filter ", "packfile-uris ", NULL };
	static const char * const flag[] = { "thin-pack", "no-progress",
	    "include-tag", "ofs-delta", "sideband-all", "wait-for-done", "done",
	    "symrefs", "peel", "unborn", "size", NULL };

	return (any_prefix(l, pre) || any_exact(l, flag));
}

/*
 * Capabilities go first, sorted; then the delimiter; then set-like arguments, sorted, and the
 * rest in their original order. All three groups share one array of np lines: capabilities and
 * set arguments grow up from the start, the ordered rest grows down from the end.
 */
static int
canon_v2(const uint8_t *in, size_t len, size_t np, struct out *o)
{
	struct line *v, l;
	size_t ncaps = 0, nset = 0, nord = 0, used, i;
	int t, cmd = 0, args = 0;

	v = lines_get(np);
	if (v == NULL)
		return (-1);
	for (;;) {
		used = pkt_next(in, len, &l, &t);
		if (used == 0)
			return (-1);
		in += used;
		len -= used;
		if (t == PKT_FLUSH)
			break;
		if (t == PKT_END || (t == PKT_DELIM && (args || !cmd)))
			return (-1);
		if (t == PKT_DELIM) {
			args = 1;
		} else if (!cmd) {
			if (!starts(&l, "command=") || l.n == 8)
				return (-1);
			emit_line(o, &l);
			cmd = 1;
		} else if (!args) {
			if (!is_telemetry(&l))
				v[ncaps++] = l;
		} else if (!oid_line_ok(&l)) {
			return (-1);
		} else if (is_set_arg(&l)) {
			v[ncaps + nset++] = l;
		} else {
			v[np - ++nord] = l;
		}
	}
	if (len != 0 || !cmd)
		return (-1);
	emit_lines(o, v, uniq(v, ncaps));
	emit_special(o, PKT_DELIM);
	emit_lines(o, v + ncaps, uniq(v + ncaps, nset));
	for (i = 1; i <= nord; i++)
		emit_line(o, &v[np - i]);
	emit_special(o, PKT_FLUSH);
	return (o->err ? -1 : 0);
}

/* Splits a v0 want into w and its capabilities; caps is NULL when the line may carry none. */
static int
want_line(const struct line *l, struct line *caps, size_t *ncaps, struct line *w)
{
	const uint8_t *p = l->p + 5, *e = l->p + l->n, *sp, *q;
	struct line t;

	sp = memchr(p, ' ', (size_t)(e - p));
	if (sp == NULL) {
		*w = *l;
		return (is_oid(p, (size_t)(e - p)) ? 0 : -1);
	}
	if (caps == NULL || !is_oid(p, (size_t)(sp - p)))
		return (-1);
	w->p = l->p;
	w->n = (size_t)(sp - l->p);
	for (p = sp + 1; p < e; p = q + 1) {
		q = memchr(p, ' ', (size_t)(e - p));
		if (q == NULL)
			q = e;
		t.p = p;
		t.n = (size_t)(q - p);
		if (t.n > 0 && !is_telemetry(&t))
			caps[(*ncaps)++] = t;
	}
	return (0);
}

/* Lines up to the first flush are sorted, capabilities on the first want; the rest is verbatim. */
static int
canon_v0(const uint8_t *in, size_t len, size_t np, size_t maxpay, struct out *o)
{
	struct line *set, *caps, l;
	size_t nset = 0, ncaps = 0, used, k, c, total;
	int t, want = 0;

	set = lines_get(np + maxpay / 2 + 2);
	if (set == NULL)
		return (-1);
	caps = set + np;
	for (;;) {
		used = pkt_next(in, len, &l, &t);
		if (used == 0)
			return (-1);
		in += used;
		len -= used;
		if (t == PKT_FLUSH)
			break;
		if (t != PKT_DATA)
			return (-1);
		if (starts(&l, "want ")) {
			if (want_line(&l, want ? NULL : caps, &ncaps, &set[nset++]))
				return (-1);
			want = 1;
		} else if (!oid_line_ok(&l)) {
			return (-1);
		} else {
			set[nset++] = l;
		}
	}
	if (!want)
		return (-1);
	nset = uniq(set, nset);
	ncaps = uniq(caps, ncaps);
	for (k = 0; k < nset; k++) {
		if (ncaps == 0 || !starts(&set[k], "want ")) {
			emit_line(o, &set[k]);
			continue;
		}
		total = set[k].n;
		for (c = 0; c < ncaps; c++)
			total += 1 + caps[c].n;
		emit_head(o, total);
		ow(o, set[k].p, set[k].n);
		for (c = 0; c < ncaps; c++) {
			ow(o, " ", 1);
			ow(o, caps[c].p, caps[c].n);
		}
		ow(o, "\n", 1);
		ncaps = 0;
	}
	emit_special(o, PKT_FLUSH);
	while (len > 0) {
		used = pkt_next(in, len, &l, &t);
		if (used == 0)
			return (-1);
		in += used;
		len -= used;
		if (t == PKT_DATA)
			emit_line(o, &l);
		else
			emit_special(o, t);
	}
	return (o->err ? -1 : 0);
}

int
gc_canonicalize_sink(const uint8_t *in, size_t len, int proto, gc_sink_f *sink, void *priv)
{
	struct out o = { sink, priv, 0 };
	size_t np, maxpay;
	int r;

	if (len == 0)
		return (-1);
	np = count_pkts(in, len, &maxpay);
	if (np == SIZE_MAX)
		return (-1);
	r = proto == 2 ? canon_v2(in, len, np, &o) : canon_v0(in, len, np, maxpay, &o);
	lines_release();
	return (r == 0 && !o.err ? 0 : -1);
}

static int
buf_sink(void *priv, const void *p, size_t n)
{
	return (gc_buf_add(priv, p, n));
}

int
gc_canonicalize(const uint8_t *in, size_t len, int proto, struct gc_buf *out)
{
	int r = gc_canonicalize_sink(in, len, proto, buf_sink, out);

	return (r == 0 && !out->oom ? 0 : -1);
}

int
gc_v2_command(const uint8_t *in, size_t len, char *dst, size_t dstlen)
{
	struct line l;
	int t;

	if (pkt_next(in, len, &l, &t) == 0 || t != PKT_DATA || l.n < 9 ||
	    memcmp(l.p, "command=", 8) != 0 || l.n - 8 + 1 > dstlen)
		return (-1);
	memcpy(dst, l.p + 8, l.n - 8);
	dst[l.n - 8] = '\0';
	return (0);
}
