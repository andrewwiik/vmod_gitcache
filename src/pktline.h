#ifndef GITCACHE_PKTLINE_H
#define GITCACHE_PKTLINE_H

#include <stddef.h>
#include <stdint.h>

struct gc_buf {
	uint8_t	*p;
	size_t	len;
	size_t	cap;
	int	oom;
};

void gc_buf_init(struct gc_buf *);
void gc_buf_fini(struct gc_buf *);
int gc_buf_add(struct gc_buf *, const void *, size_t);
int gc_buf_reserve(struct gc_buf *, size_t);

int gc_is_hex(const void *, size_t);
int gc_lencmp(const void *, size_t, const void *, size_t);

/*
 * Per-thread scratch buffers. A worker handles one request at a time, so these are reused
 * across requests and never allocate in the steady state. A buffer that grew past GC_KEEP is
 * freed on release so a rare large request does not pin memory in every thread. Everything
 * a thread holds is freed when it exits.
 */
#define GC_KEEP		65536
#define GC_SLOT_BODY	0
#define GC_SLOT_INFLATE	1
#define GC_NSLOTS	2
struct gc_buf *gc_tls_buf(int slot, size_t hint);
void gc_tls_release(int slot);

/* Per-thread objects: made by mk on first use in a thread, freed by fr when it exits. */
#define GC_OBJ_INFLATER	0
#define GC_OBJ_DIGEST	1
#define GC_NOBJS	2
void *gc_tls_obj(int slot, void *(*mk)(void), void (*fr)(void *));

typedef int gc_sink_f(void *priv, const void *p, size_t n);

/*
 * Canonicalise an upload-pack request body (see PROTOCOL.md), streaming the canonical bytes to
 * sink in order. Returns 0, or -1 if the body is not acceptable; the sink may then have
 * received partial output, which the caller must discard.
 */
int gc_canonicalize_sink(const uint8_t *in, size_t len, int proto, gc_sink_f *sink, void *priv);

int gc_canonicalize(const uint8_t *in, size_t len, int proto, struct gc_buf *out);

int gc_v2_command(const uint8_t *in, size_t len, char *dst, size_t dstlen);

#endif
