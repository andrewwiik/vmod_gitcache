#ifndef GITCACHE_COMPAT_H
#define GITCACHE_COMPAT_H

/*
 * Include after cache/cache.h and pktline.h. Varnish Enterprise keeps its VRT major version far
 * above open source (6012 vs 23), which is how the two API generations are told apart.
 */
#if VRT_MAJOR_VERSION < 1000

typedef hdr_t gc_hdr_t;
#define GC_HDR(name)		HDR(name)
#define GC_IS_HDR(t, h)		http_IsHdr((t), (h))
#define GC_BODY_CACHED(req)	((req)->req_body_status == BS_CACHED)
#define GC_BODY_NONE(req)	((req)->req_body_status == BS_NONE)
#define GC_SEND_TIMEOUT(ctx, t)	VRT_l_sess_send_timeout(ctx, t)

static int
gc_body_cb(void *priv, unsigned flush, const void *p, ssize_t n)
{
	(void)flush;
	return (n > 0 && gc_buf_add(priv, p, (size_t)n) ? -1 : 0);
}

static int
gc_read_body(struct req *req, struct gc_buf *b)
{
	return (VRB_Iterate(req->wrk, req->vsl, req, gc_body_cb, b) < 0 ? -1 : 0);
}

static void
gc_hashdata(VRT_CTX, const char *s)
{
	VRT_hashdata(ctx, TOSTRAND(s));
}

static int
gc_ban(VRT_CTX, const char *expr)
{
	return (VRT_ban_string(ctx, expr) == NULL ? 0 : -1);
}

#else

/* A name prefixed by its length, the layout HDR() produces in the newer API. */
typedef const char *gc_hdr_t;
#define GC_HDR(name)							\
	((gc_hdr_t)&(const struct {					\
		unsigned char	l;					\
		char		s[sizeof(name ":")];			\
	}){ sizeof(name), name ":" })
#define GC_IS_HDR(t, h)							\
	(Tlen(*(t)) >= (unsigned char)(h)[0] &&				\
	    strncasecmp((t)->b, (h) + 1, (unsigned char)(h)[0]) == 0)
#define GC_BODY_CACHED(req)	((req)->req_body_status == REQ_BODY_CACHED)
#define GC_BODY_NONE(req)	((req)->req_body_status == REQ_BODY_NONE)
#define GC_SEND_TIMEOUT(ctx, t)	VRT_l_resp_send_timeout(ctx, t)

static int
gc_body_cb(void *priv, int flush, int last, const void *p, ssize_t n)
{
	(void)flush;
	(void)last;
	return (n > 0 && gc_buf_add(priv, p, (size_t)n) ? -1 : 0);
}

static int
gc_read_body(struct req *req, struct gc_buf *b)
{
	return (ObjIterate(req->wrk, req->body_oc, b, gc_body_cb, 0, 0, -1) ? -1 : 0);
}

static void
gc_hashdata(VRT_CTX, const char *s)
{
	VRT_hashdata(ctx, s, vrt_magic_string_end);
}

static int
gc_ban(VRT_CTX, const char *expr)
{
	VRT_ban_string(ctx, expr);
	return (0);
}

#endif

#define GC_H_STAGE	GC_HDR("X-GC-Stage")
#define GC_H_ORIGURL	GC_HDR("X-GC-Orig-URL")
#define GC_H_ORIGMETH	GC_HDR("X-GC-Orig-Method")
#define GC_H_OKIND	GC_HDR("X-GC-Okind")
#define GC_H_FINAL	GC_HDR("X-GC-Final")
#define GC_H_KEY	GC_HDR("X-GC-Key")
#define GC_H_KIND	GC_HDR("X-GC-Kind")
#define GC_H_REPO	GC_HDR("X-GC-Repo")
#define GC_H_HITS	GC_HDR("X-GC-Hits")
#define GC_H_PURGE	GC_HDR("X-Purge-Token")
#define GC_H_PROTO	GC_HDR("Git-Protocol")
#define GC_H_AUTH	GC_HDR("Authorization")
#define GC_H_CTYPE	GC_HDR("Content-Type")
#define GC_H_CENC	GC_HDR("Content-Encoding")

#endif
