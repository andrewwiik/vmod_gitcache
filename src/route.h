#ifndef GITCACHE_ROUTE_H
#define GITCACHE_ROUTE_H

#include <stddef.h>
#include <stdint.h>

#include "pktline.h"

enum gc_kind {
	K_NONE,
	K_INFO_REFS,
	K_INFO_REFS_PUSH,
	K_UPLOAD_PACK,
	K_RECEIVE_PACK,
	K_LFS_BATCH,
	K_LFS_OBJECT,
	K_LFS_OTHER,
	K_DUMB_REFS,
	K_DUMB_OBJECT,
};

struct gc_req {
	enum gc_kind	k;
	const char	*repo;
	size_t		repolen;
	const char	*tail;
	size_t		taillen;
	int		href;
	const char	*query;
};

void gc_route(const char *url, const char *meth, struct gc_req *r);

int gc_lfs_is_download(const uint8_t *in, size_t n);

int gc_gunzip(struct gc_buf *b, size_t max);

/*
 * Inflate a gzip stream into out (which is reused, not freed), in one pass. The size recorded
 * in the gzip trailer must not exceed max. Returns 0 or -1; out is left empty on failure.
 */
int gc_gunzip_to(const uint8_t *in, size_t n, struct gc_buf *out, size_t max);

/*
 * Canonical form of an LFS batch request body for hashing: the elements of the "transfers"
 * and "objects" arrays are sorted, since clients list them in random order and the server
 * treats them as sets. Everything else is verbatim. Returns 0, or -1 on allocation failure.
 */
int gc_lfs_canon(const uint8_t *in, size_t n, struct gc_buf *out);

#endif
