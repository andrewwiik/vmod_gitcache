#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/pktline.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct gc_buf a, b, c;
	char cmd[8];
	int proto, ra, rb, rc;

	if (size < 1)
		return (0);
	proto = data[0] % 3;
	data++;
	size--;

	gc_buf_init(&a); gc_buf_init(&b); gc_buf_init(&c);
	ra = gc_canonicalize(data, size, proto, &a);
	rb = gc_canonicalize(data, size, proto, &b);
	assert(ra == rb);
	if (ra == 0) {
		assert(a.len == b.len && memcmp(a.p, b.p, a.len) == 0);
		rc = gc_canonicalize(a.p, a.len, proto, &c);
		if (rc == 0 && !(c.len == a.len && memcmp(c.p, a.p, a.len) == 0)) {
			size_t d = 0, m = a.len < c.len ? a.len : c.len;
			while (d < m && a.p[d] == c.p[d])
				d++;
			fprintf(stderr, "IDEMPOTENCE proto=%d first=%zu second=%zu differ-at=%zu\n", proto, a.len, c.len, d);
			for (int w = 0; w < 2; w++) {
				struct gc_buf *x = w ? &c : &a;
				fprintf(stderr, "%s:", w ? "second" : "first ");
				for (size_t i = d > 24 ? d - 24 : 0; i < x->len && i < d + 40; i++)
					fprintf(stderr, (x->p[i] >= 32 && x->p[i] < 127) ? "%c" : "<%02x>", x->p[i]);
				fprintf(stderr, "\n");
			}
			abort();
		}

		for (size_t i = 0; i + 4 <= a.len; ) {
			size_t l = 0;
			for (int k = 0; k < 4; k++) {
				uint8_t ch = a.p[i + k];
				l = l * 16 + (size_t)(ch <= '9' ? ch - '0' : ch - 'a' + 10);
			}
			if (l < 4) { i += 4; continue; }
			assert(l <= 65520 && i + l <= a.len);
			i += l;
		}
	}
	(void)gc_v2_command(data, size, cmd, sizeof cmd);
	gc_buf_fini(&a); gc_buf_fini(&b); gc_buf_fini(&c);
	return (0);
}
