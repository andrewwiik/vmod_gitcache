/*
 * One pack request, end to end in the module's own code: load the request body, inflate it,
 * canonicalise it and hash it. Built twice from the same source: -DOLD against the sources as
 * first committed (generic SHA-256, zlib in place, a malloc'd buffer per step, the work
 * repeated for kind detection and again after the access-check restart) and without it against
 * the current sources.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "pktline.h"
#include "route.h"

#ifdef OLD
#include <sys/types.h>
#include "vdef.h"
#include "vas.h"
#include "vsha256.h"
void
VAS_Fail(const char *func, const char *file, int line, const char *cond, enum vas_e kind)
{
	(void)func; (void)file; (void)line; (void)cond; (void)kind;
	abort();
}
#else
#include <openssl/evp.h>
#endif

static unsigned long n_malloc;
void *__real_malloc(size_t);
void *__real_realloc(void *, size_t);
void *__real_calloc(size_t, size_t);
void *__wrap_malloc(size_t n) { n_malloc++; return (__real_malloc(n)); }
void *__wrap_realloc(void *p, size_t n) { n_malloc++; return (__real_realloc(p, n)); }
void *__wrap_calloc(size_t a, size_t b) { n_malloc++; return (__real_calloc(a, b)); }

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec + t.tv_nsec * 1e-9);
}

#ifdef OLD
static int
sink_sha(void *p, const void *d, size_t n)
{
	VSHA256_Update(p, d, n);
	return (0);
}
#else
struct hs { EVP_MD_CTX *c; size_t n; uint8_t buf[4096]; };
static int
sink_sha(void *priv, const void *p, size_t len)
{
	struct hs *h = priv;

	if (len >= sizeof h->buf) {
		if (h->n) { EVP_DigestUpdate(h->c, h->buf, h->n); h->n = 0; }
		EVP_DigestUpdate(h->c, p, len);
		return (0);
	}
	if (h->n + len > sizeof h->buf) { EVP_DigestUpdate(h->c, h->buf, h->n); h->n = 0; }
	memcpy(h->buf + h->n, p, len);
	h->n += len;
	return (0);
}
#endif

static size_t
mk_body(uint8_t *out, int nwant, int nhave)
{
	size_t n = 0;
	char oid[41];
	int i;

	n += (size_t)sprintf((char *)out + n, "0012command=fetch\n0015agent=git/2.43.0\n0017object-format=sha1\n0001");
	n += (size_t)sprintf((char *)out + n, "000ethin-pack\n000eofs-delta\n");
	for (i = 0; i < nwant; i++) {
		snprintf(oid, sizeof oid, "%040x", (unsigned)(i * 2654435761u));
		n += (size_t)sprintf((char *)out + n, "0032want %s\n", oid);
	}
	for (i = 0; i < nhave; i++) {
		snprintf(oid, sizeof oid, "%040x", (unsigned)(i * 40503u + 7) * 2246822519u);
		n += (size_t)sprintf((char *)out + n, "0032have %s\n", oid);
	}
	n += (size_t)sprintf((char *)out + n, "0009done\n0000");
	return (n);
}

static size_t
gzip9(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	z_stream zs;

	memset(&zs, 0, sizeof zs);
	deflateInit2(&zs, 9, Z_DEFLATED, 15 + 16, 9, Z_DEFAULT_STRATEGY);
	zs.next_in = (Bytef *)(uintptr_t)in;
	zs.avail_in = (uInt)n;
	zs.next_out = out;
	zs.avail_out = (uInt)cap;
	deflate(&zs, Z_FINISH);
	n = zs.total_out;
	deflateEnd(&zs);
	return (n);
}

/* the "stored" body as the module finds it: gzip if git compressed it, else plain */
static const uint8_t *stored;
static size_t stored_n;
static int is_gz;

static int
one_request(void)
{
	uint8_t dig[64];

#ifdef OLD
	/* two passes (the second after the access-check restart), each loading and inflating the
	 * body for kind detection and again for the key, then canonicalising into a buffer */
	int pass, k;

	for (pass = 0; pass < 2; pass++) {
		struct gc_buf b, canon;
		VSHA256_CTX sc;

		for (k = 0; k < 2; k++) {
			gc_buf_init(&b);
			gc_buf_add(&b, stored, stored_n);
			if (is_gz && gc_gunzip(&b, 10 << 20))
				return (-1);
			if (k == 0)
				gc_buf_fini(&b);
		}
		gc_buf_init(&canon);
		if (gc_canonicalize(b.p, b.len, 2, &canon))
			return (-1);
		VSHA256_Init(&sc);
		VSHA256_Update(&sc, canon.p, canon.len);
		VSHA256_Final(dig, &sc);
		gc_buf_fini(&b);
		gc_buf_fini(&canon);
	}
#else
	/* one analysis: load once into the reused buffer, inflate once, canonicalise into the hash;
	 * the restart reuses the stored key and does no body work */
	static EVP_MD *md;
	static __thread EVP_MD_CTX *ctx;
	struct hs h;
	struct gc_buf *raw = gc_tls_buf(GC_SLOT_BODY, stored_n + 1), *inf;
	const uint8_t *body;
	size_t blen;
	unsigned l;

	if (md == NULL)
		md = EVP_MD_fetch(NULL, "SHA256", NULL);
	if (ctx == NULL)
		ctx = EVP_MD_CTX_new();
	memcpy(raw->p, stored, stored_n);
	raw->len = stored_n;
	body = raw->p;
	blen = raw->len;
	if (is_gz) {
		inf = gc_tls_buf(GC_SLOT_INFLATE, 0);
		if (gc_gunzip_to(raw->p, raw->len, inf, 10 << 20))
			return (-1);
		body = inf->p;
		blen = inf->len;
	}
	h.c = ctx;
	h.n = 0;
	EVP_DigestInit_ex(ctx, md, NULL);
	if (gc_canonicalize_sink(body, blen, 2, sink_sha, &h))
		return (-1);
	if (h.n)
		EVP_DigestUpdate(ctx, h.buf, h.n);
	EVP_DigestFinal_ex(ctx, dig, &l);
	gc_tls_release(GC_SLOT_BODY);
	gc_tls_release(GC_SLOT_INFLATE);
#endif
	return (dig[0]);
}

int
main(void)
{
	static const struct { const char *name; int w, h, gz; } shapes[] = {
		{ "clone, 2 wants (plain)", 2, 0, 0 },
		{ "incremental fetch, 4 wants + 64 haves (gzip)", 4, 64, 1 },
		{ "big negotiation, 50 wants + 2000 haves (gzip)", 50, 2000, 1 },
		{ "CI mega-fetch, 3000 wants + 20000 haves (gzip)", 3000, 20000, 1 },
	};
	size_t i;

	for (i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
		size_t cap = (size_t)(shapes[i].w + shapes[i].h) * 60 + 4096;
		uint8_t *raw = malloc(cap), *gz = malloc(cap);
		size_t rn = mk_body(raw, shapes[i].w, shapes[i].h), gn = 0;
		double best = 1e30;
		unsigned long m0, runs = 0;
		int rep;
		size_t batch = rn < 4096 ? 200 : (rn < 200000 ? 10 : 1);

		if (shapes[i].gz) {
			gn = gzip9(raw, rn, gz, cap);
			stored = gz; stored_n = gn; is_gz = 1;
		} else {
			stored = raw; stored_n = rn; is_gz = 0;
		}
		if (one_request() < 0) { printf("ERROR\t%s\n", shapes[i].name); continue; }
		for (rep = 0; rep < 5; rep++) {
			size_t it = 0, k;
			double t0 = now(), t1;

			do {
				for (k = 0; k < batch; k++)
					one_request();
				it += batch;
				t1 = now();
			} while (t1 - t0 < 0.1);
			if ((t1 - t0) / it < best)
				best = (t1 - t0) / it;
		}
		m0 = n_malloc;
		for (runs = 0; runs < 10; runs++)
			one_request();
		printf("RESULT\t%s\t%zu\t%zu\t%.1f\t%.1f\n", shapes[i].name, rn, shapes[i].gz ? gn : rn, best * 1e6,
		    (double)(n_malloc - m0) / 10);
		free(raw);
		free(gz);
	}
	return (0);
}
