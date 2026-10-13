#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "../src/pktline.h"

void unit_route(void);

static size_t
pk(char *dst, ...)
{
	va_list ap;
	const char *s;
	size_t n = 0;

	va_start(ap, dst);
	while ((s = va_arg(ap, const char *)) != NULL) {
		if (!strcmp(s, "0000") || !strcmp(s, "0001")) {
			memcpy(dst + n, s, 4);
			n += 4;
		} else {
			n += (size_t)sprintf(dst + n, "%04zx%s\n", strlen(s) + 5, s);
		}
	}
	va_end(ap);
	return (n);
}

static int
canon(const char *b, size_t n, int proto, struct gc_buf *o)
{
	gc_buf_init(o);
	return (gc_canonicalize((const uint8_t *)b, n, proto, o));
}

static int
same(const char *a, size_t an, const char *b, size_t bn, int proto)
{
	struct gc_buf x, y;
	int r;

	assert(canon(a, an, proto, &x) == 0);
	assert(canon(b, bn, proto, &y) == 0);
	r = x.len == y.len && memcmp(x.p, y.p, x.len) == 0;
	gc_buf_fini(&x);
	gc_buf_fini(&y);
	return (r);
}

int
main(void)
{
	char a[4096], b[4096];
	size_t an, bn;
	struct gc_buf o;
	char cmd[32];

	an = pk(a, "command=fetch", "agent=git/2.43.0", "session-id=aaa", "object-format=sha1",
	    "0001", "thin-pack", "ofs-delta", "want 1111", "want 2222", "have 3333", "done", "0000", NULL);
	bn = pk(b, "command=fetch", "session-id=bbb", "agent=git/2.50.1-Apple", "object-format=sha1",
	    "0001", "thin-pack", "ofs-delta", "want 2222", "want 1111", "have 3333", "done", "0000", NULL);
	assert(same(a, an, b, bn, 2));
	printf("ok v2 telemetry+want order\n");

	an = pk(a, "command=fetch", "0001", "want 1", "have 2", "have 3", "0000", NULL);
	bn = pk(b, "command=fetch", "0001", "want 1", "have 3", "have 2", "0000", NULL);
	assert(!same(a, an, b, bn, 2));
	printf("ok v2 have order kept\n");

	an = pk(a, "command=fetch", "0001", "want 1", "0000", NULL);
	bn = pk(b, "command=fetch", "0001", "want 9", "0000", NULL);
	assert(!same(a, an, b, bn, 2));

	an = pk(a, "command=ls-refs", "0001", "peel", "0000", NULL);
	bn = pk(b, "command=fetch", "0001", "peel", "0000", NULL);
	assert(!same(a, an, b, bn, 2));
	printf("ok v2 distinct\n");

	an = pk(a, "command=ls-refs", "0001", "peel", "ref-prefix refs/heads/", "ref-prefix HEAD", "0000", NULL);
	bn = pk(b, "command=ls-refs", "0001", "peel", "ref-prefix HEAD", "ref-prefix refs/heads/", "ref-prefix HEAD", "0000", NULL);
	assert(same(a, an, b, bn, 2));
	printf("ok v2 ref-prefix\n");

	assert(gc_v2_command((uint8_t *)a, an, cmd, sizeof cmd) == 0 && !strcmp(cmd, "ls-refs"));
	printf("ok v2 command\n");

	an = pk(a, "want 1111 multi_ack_detailed side-band-64k thin-pack agent=git/2.43.0 ofs-delta",
	    "want 2222", "0000", "have 3333", "done", NULL);
	bn = pk(b, "want 2222 ofs-delta thin-pack side-band-64k multi_ack_detailed agent=git/2.50",
	    "want 1111", "0000", "have 3333", "done", NULL);

	assert(same(a, an, b, bn, 1));
	printf("ok v0 caps+want order\n");

	bn = pk(b, "want 1111 multi_ack_detailed side-band-64k thin-pack agent=x ofs-delta",
	    "want 2222", "0000", "have 4444", "done", NULL);
	assert(!same(a, an, b, bn, 1));
	printf("ok v0 distinct\n");

	an = pk(a, "command=fetch", "0001", "thin-pack", "want 2", "ofs-delta", "no-progress", "have 7", "want 1", "done", "0000", NULL);
	bn = pk(b, "command=fetch", "0001", "done", "no-progress", "want 1", "want 2", "want 2", "ofs-delta", "thin-pack", "have 7", "0000", NULL);
	assert(same(a, an, b, bn, 2));

	bn = pk(b, "command=fetch", "0001", "done", "want 1", "want 2", "ofs-delta", "thin-pack", "have 7", "0000", NULL);
	assert(!same(a, an, b, bn, 2));

	an = pk(a, "command=fetch", "0001", "want 1", "deepen 1", "done", "0000", NULL);
	bn = pk(b, "command=fetch", "0001", "want 1", "deepen 2", "done", "0000", NULL);
	assert(!same(a, an, b, bn, 2));
	an = pk(a, "command=fetch", "0001", "want 1", "filter blob:none", "done", "0000", NULL);
	bn = pk(b, "command=fetch", "0001", "want 1", "done", "0000", NULL);
	assert(!same(a, an, b, bn, 2));

	an = pk(a, "command=fetch", "object-format=sha256", "0001", "want 1", "0000", NULL);
	bn = pk(b, "command=fetch", "object-format=sha1", "0001", "want 1", "0000", NULL);
	assert(!same(a, an, b, bn, 2));

	an = pk(a, "command=bundle-uri", "0001", "0000", NULL);
	bn = pk(b, "command=bundle-uri", "0000", NULL);
	assert(same(a, an, b, bn, 2));

	an = pk(a, "command=object-info", "0001", "size", "oid 1", "oid 2", "0000", NULL);
	bn = pk(b, "command=object-info", "0001", "oid 2", "size", "oid 1", "0000", NULL);
	assert(same(a, an, b, bn, 2));
	printf("ok v2 flags/sets/deepen/filter/object-format/bundle-uri/object-info\n");

	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)"0000", 4, 2, &o) == -1); gc_buf_fini(&o);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)"0000", 4, 1, &o) == -1); gc_buf_fini(&o);
	an = pk(a, "0001", "want 1", "0000", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 2, &o) == -1); gc_buf_fini(&o);
	an = pk(a, "agent=x", "command=fetch", "0001", "want 1", "0000", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 2, &o) == -1); gc_buf_fini(&o);
	an = pk(a, "command=fetch", "0001", "want 1", "0000", "command=fetch", "0001", "want 2", "0000", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 2, &o) == -1); gc_buf_fini(&o);
	an = pk(a, "command=fetch", "0001", "want 1", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 2, &o) == -1); gc_buf_fini(&o);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)"fff1x", 5, 2, &o) == -1); gc_buf_fini(&o);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)"0002", 4, 2, &o) == -1); gc_buf_fini(&o);

	an = pk(a, "shallow 1", "0000", "done", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 0, &o) == -1); gc_buf_fini(&o);
	printf("ok rejects probe/no-command/trailing/oversized/no-want\n");

	{
		an = pk(a, "want 2222 side-band-64k agent=x thin-pack", "want 1111", "shallow 3333", "0000", "have 4444", "0000", "have 5555", "done", NULL);
		gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 1, &o) == 0);
		{
			struct gc_buf o2; gc_buf_init(&o2);
			assert(gc_canonicalize(o.p, o.len, 1, &o2) == 0);
			assert(o.len == o2.len && memcmp(o.p, o2.p, o.len) == 0);
			gc_buf_fini(&o2);
		}
		gc_buf_fini(&o);
		an = pk(a, "command=fetch", "agent=x", "object-format=sha1", "0001", "want 2", "want 1", "have 9", "have 8", "thin-pack", "done", "0000", NULL);
		gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 2, &o) == 0);
		{
			struct gc_buf o2; gc_buf_init(&o2);
			assert(gc_canonicalize(o.p, o.len, 2, &o2) == 0);
			assert(o.len == o2.len && memcmp(o.p, o2.p, o.len) == 0);
			gc_buf_fini(&o2);
		}
		gc_buf_fini(&o);
		printf("ok idempotent (v0 and v2)\n");
	}

	an = pk(a, "want 1111 side-band-64k", "want 2222 thin-pack", "0000", "done", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 0, &o) == -1); gc_buf_fini(&o);
	an = pk(a, "want zzzz", "0000", "done", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 0, &o) == -1); gc_buf_fini(&o);
	an = pk(a, "command=fetch", "0001", "want 1111 extra", "0000", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 2, &o) == -1); gc_buf_fini(&o);
	an = pk(a, "command=fetch", "0001", "want 1111", "have ", "0000", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 2, &o) == -1); gc_buf_fini(&o);
	an = pk(a, "command=fetch", "0001", "want ABCD", "0000", NULL);
	gc_buf_init(&o); assert(gc_canonicalize((const uint8_t *)a, an, 2, &o) == -1); gc_buf_fini(&o);
	printf("ok rejects malformed object ids / misplaced capabilities\n");

	{
		static char big[4 * 600 + 64];
		size_t n = 0, i;

		for (i = 0; i < 600; i++) { memcpy(big + n, "0004", 4); n += 4; }
		memcpy(big + n, "0000", 4); n += 4;
		gc_buf_init(&o); (void)gc_canonicalize((const uint8_t *)big, n, 0, &o); gc_buf_fini(&o);
		memcpy(big, "0012command=fetch\n0001", 22);
		memmove(big + 22, big + 22, n - 22);
		gc_buf_init(&o); (void)gc_canonicalize((const uint8_t *)big, n, 2, &o); gc_buf_fini(&o);
		printf("ok many empty pkt-lines do not overflow\n");
	}

	gc_buf_init(&o);
	assert(gc_canonicalize((const uint8_t *)"zzzz", 4, 2, &o) == -1);
	gc_buf_fini(&o);
	gc_buf_init(&o);
	assert(gc_canonicalize((const uint8_t *)"0010abc", 7, 2, &o) == -1);
	gc_buf_fini(&o);
	gc_buf_init(&o);
	assert(gc_canonicalize((const uint8_t *)"", 0, 2, &o) == -1);
	gc_buf_fini(&o);
	gc_buf_init(&o);
	assert(gc_canonicalize((const uint8_t *)"0003", 4, 1, &o) == -1);
	gc_buf_fini(&o);
	printf("ok framing errors\n");
	unit_route();
	printf("ALL PASS\n");
	return (0);
}
