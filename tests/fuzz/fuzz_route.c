#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../../src/route.h"

static const char *methods[] = { "GET", "POST", "PUT", "HEAD", "DELETE" };

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	int mode;

	if (size < 2)
		return (0);
	mode = data[0] % 5;
	data++;
	size--;

	if (mode == 0) {
		struct gc_req r;
		char *url = malloc(size);
		const char *meth = methods[data[0] % 5];
		size_t ul;

		if (url == NULL)
			return (0);
		memcpy(url, data + 1, size - 1);
		url[size - 1] = '\0';
		ul = strlen(url);
		gc_route(url, meth, &r);
		if (r.k != K_NONE) {
			assert(r.repo >= url && r.repo + r.repolen <= url + ul);
			assert(r.repolen > 0);
			if (r.tail != NULL && r.href)
				assert(r.tail >= url && r.tail + r.taillen <= url + ul);
			if (r.query != NULL)
				assert(r.query >= url && r.query <= url + ul && *r.query != '\0');
		}
		free(url);
	} else if (mode == 1) {
		struct gc_buf b;

		gc_buf_init(&b);
		gc_buf_add(&b, data, size);
		(void)gc_lfs_is_download(b.p, b.len);
		gc_buf_fini(&b);
	} else if (mode == 2) {
		struct gc_buf b;
		size_t before;
		int rc;

		gc_buf_init(&b);
		gc_buf_add(&b, data, size);
		before = b.len;
		rc = gc_gunzip(&b, 1 << 16);
		if (rc == 0)
			assert(b.len <= (1 << 16));
		else
			assert(b.len == before);
		gc_buf_fini(&b);
	} else if (mode == 4) {
		struct gc_buf a, b;

		gc_buf_init(&a);
		gc_buf_init(&b);
		if (gc_lfs_canon(data, size, &a) == 0) {
			assert(gc_lfs_canon(a.p, a.len, &b) == 0);
			assert(a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, a.len) == 0));
		}
		gc_buf_fini(&a);
		gc_buf_fini(&b);
	} else {
		struct gc_buf z;
		z_stream zs;
		uint8_t chunk[4096];

		gc_buf_init(&z);
		memset(&zs, 0, sizeof zs);
		if (deflateInit2(&zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
			return (0);
		zs.next_in = (Bytef *)data;
		zs.avail_in = (uInt)size;
		do {
			zs.next_out = chunk;
			zs.avail_out = sizeof chunk;
			deflate(&zs, Z_FINISH);
			gc_buf_add(&z, chunk, sizeof chunk - zs.avail_out);
		} while (zs.avail_out == 0);
		deflateEnd(&zs);
		assert(gc_gunzip(&z, size + 1) == 0);
		assert(z.len == size && (size == 0 || memcmp(z.p, data, size) == 0));
		gc_buf_fini(&z);
	}
	return (0);
}
