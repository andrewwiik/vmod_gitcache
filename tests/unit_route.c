#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "../src/route.h"

static enum gc_kind
k(const char *url, const char *meth)
{
	struct gc_req r;

	gc_route(url, meth, &r);
	return (r.k);
}

static void
add_str(struct gc_buf *b, const char *str)
{
	gc_buf_add(b, str, strlen(str));
}

static void
repo_is(const char *url, const char *meth, const char *want)
{
	struct gc_req r;

	gc_route(url, meth, &r);
	assert(r.k != K_NONE);
	assert(r.repolen == strlen(want) && memcmp(r.repo, want, r.repolen) == 0);
}

static void
gz(const void *in, size_t n, struct gc_buf *out)
{
	z_stream zs;
	uint8_t chunk[4096];

	memset(&zs, 0, sizeof zs);
	assert(deflateInit2(&zs, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
	zs.next_in = (Bytef *)in;
	zs.avail_in = (uInt)n;
	do {
		zs.next_out = chunk;
		zs.avail_out = sizeof chunk;
		deflate(&zs, Z_FINISH);
		gc_buf_add(out, chunk, sizeof chunk - zs.avail_out);
	} while (zs.avail_out == 0);
	deflateEnd(&zs);
}

static void *
tls_user(void *arg)
{
	const struct gc_buf *z = arg;
	struct gc_buf *b, o;

	b = gc_tls_buf(GC_SLOT_BODY, 1000);
	assert(!b->oom);
	b = gc_tls_buf(GC_SLOT_INFLATE, 0);
	assert(gc_gunzip_to(z->p, z->len, b, 1 << 20) == 0);
	gc_buf_init(&o);
	assert(gc_canonicalize((const uint8_t *)"0012command=fetch\n0000", 22, 2, &o) == 0);
	gc_buf_fini(&o);
	return (NULL);
}

void
unit_route(void)
{
	const char *oid = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	char u[256];
	struct gc_buf b;
	struct gc_req r;

	assert(k("/o/r.git/info/refs?service=git-upload-pack", "GET") == K_INFO_REFS);
	assert(k("/o/r.git/info/refs?service=git-receive-pack", "GET") == K_INFO_REFS_PUSH);
	assert(k("/o/r.git/info/refs", "GET") == K_DUMB_REFS);
	assert(k("/o/r.git/info/refs?service=bogus", "GET") == K_NONE);
	assert(k("/o/r.git/info/refs?service=git-upload-pack", "POST") == K_NONE);
	assert(k("/o/r.git/git-upload-pack", "POST") == K_UPLOAD_PACK);
	assert(k("/o/r.git/git-upload-pack", "GET") == K_NONE);
	assert(k("/o/r.git/git-receive-pack", "POST") == K_RECEIVE_PACK);
	repo_is("/Owner/Repo.git/info/refs?service=git-upload-pack", "GET", "Owner/Repo");
	repo_is("//o/r/git-upload-pack", "POST", "o/r");
	repo_is("/a/b/c.git/git-upload-pack", "POST", "a/b/c");

	assert(k("/git-upload-pack", "POST") == K_NONE);
	assert(k("//git-upload-pack", "POST") == K_NONE);
	assert(k("", "GET") == K_NONE);
	assert(k("/", "GET") == K_NONE);
	assert(k("?", "GET") == K_NONE);
	assert(k(NULL, "GET") == K_NONE);
	assert(k("/o/r.git/info/refs", NULL) == K_NONE);

	assert(k("/o/r.git/objects/ab/cdef0123456789abcdef0123456789abcdef01", "GET") == K_DUMB_OBJECT);
	assert(k("/o/r.git/objects/AB/cdef0123456789abcdef0123456789abcdef01", "GET") == K_NONE);
	assert(k("/o/r.git/objects/ab/cdef", "GET") == K_NONE);
	assert(k("/o/r.git/objects/pack/pack-0123456789abcdef0123456789abcdef01234567.pack", "GET") == K_DUMB_OBJECT);
	assert(k("/o/r.git/objects/pack/pack-0123456789abcdef0123456789abcdef01234567.idx", "GET") == K_DUMB_OBJECT);
	assert(k("/o/r.git/objects/pack/pack-0123456789abcdef0123456789abcdef01234567.txt", "GET") == K_NONE);
	assert(k("/o/r.git/objects/pack/pack-0123456789abcdef.pack", "GET") == K_NONE);

	assert(k("/o/r.git/info/lfs/objects/batch", "POST") == K_LFS_BATCH);
	assert(k("/o/r.git/info/lfs/objects/batch", "GET") == K_LFS_OTHER);
	snprintf(u, sizeof u, "/o/r.git/info/lfs/objects/%s", oid);
	assert(k(u, "GET") == K_LFS_OBJECT);
	assert(k(u, "PUT") == K_LFS_OTHER);
	snprintf(u, sizeof u, "/repository/lfs/%s", oid);
	assert(k(u, "GET") == K_LFS_OBJECT);
	repo_is(u, "GET", "repository/lfs");
	snprintf(u, sizeof u, "/repository/lfs/%s/verify", oid);
	assert(k(u, "POST") == K_LFS_OTHER);
	snprintf(u, sizeof u, "/repository/lfs/%.63s", oid);
	assert(k(u, "GET") == K_NONE);
	snprintf(u, sizeof u, "/x/%s?X-Amz-Signature=abc", oid);
	gc_route(u, "GET", &r);
	assert(r.k == K_LFS_OBJECT && r.query != NULL && strcmp(r.query, "X-Amz-Signature=abc") == 0);
	snprintf(u, sizeof u, "/x/%s?", oid);
	gc_route(u, "GET", &r);
	assert(r.query == NULL);

	gc_buf_init(&b); gc_buf_add(&b, "{\"operation\": \"download\", \"objects\": []}", 40);
	assert(gc_lfs_is_download(b.p, b.len)); gc_buf_fini(&b);
	gc_buf_init(&b); gc_buf_add(&b, "{\"operation\":\"upload\"}", 21);
	assert(!gc_lfs_is_download(b.p, b.len)); gc_buf_fini(&b);
	gc_buf_init(&b); gc_buf_add(&b, "{\"operation\"", 12);
	assert(!gc_lfs_is_download(b.p, b.len)); gc_buf_fini(&b);
	gc_buf_init(&b); gc_buf_add(&b, "{\"operation\" : \n \"download\"}", 28);
	assert(gc_lfs_is_download(b.p, b.len)); gc_buf_fini(&b);
	gc_buf_init(&b);
	assert(!gc_lfs_is_download(b.p, b.len)); gc_buf_fini(&b);

	{
		struct gc_buf z, back;
		char text[2000];

		memset(text, 'a', sizeof text);
		gc_buf_init(&z); gz(text, sizeof text, &z);
		assert(z.len < sizeof text / 4);
		back = z;
		assert(gc_gunzip(&back, sizeof text) == 0);
		assert(back.len == sizeof text && memcmp(back.p, text, sizeof text) == 0);
		gc_buf_fini(&back);

		gc_buf_init(&z); gz(text, sizeof text, &z);
		assert(gc_gunzip(&z, sizeof text - 1) == -1);
		assert(z.len > 0);
		gc_buf_fini(&z);

		gc_buf_init(&z); gc_buf_add(&z, "not gzip at all", 15);
		assert(gc_gunzip(&z, 1000) == -1); gc_buf_fini(&z);
		gc_buf_init(&z); gz(text, sizeof text, &z); z.len /= 2;
		assert(gc_gunzip(&z, 100000) == -1); gc_buf_fini(&z);
	}
	{
		static const char *same[] = {
		    "{\"operation\":\"download\",\"objects\":[{\"oid\":\"aa\",\"size\":1},{\"oid\":\"bb\",\"size\":2}],\"transfers\":[\"lfs-standalone-file\",\"basic\",\"ssh\"],\"ref\":{\"name\":\"refs/heads/main\"}}",
		    "{\"operation\":\"download\",\"objects\":[{\"oid\":\"bb\",\"size\":2},{\"oid\":\"aa\",\"size\":1}],\"transfers\":[\"ssh\",\"lfs-standalone-file\",\"basic\"],\"ref\":{\"name\":\"refs/heads/main\"}}",
		    "{\"operation\":\"download\",\"objects\":[ {\"oid\":\"aa\",\"size\":1} , {\"oid\":\"bb\",\"size\":2} ],\"transfers\":[ \"basic\" , \"ssh\", \"lfs-standalone-file\" ],\"ref\":{\"name\":\"refs/heads/main\"}}",
		};
		static const char *differ[] = {
		    "{\"operation\":\"download\",\"objects\":[{\"oid\":\"aa\",\"size\":1},{\"oid\":\"cc\",\"size\":2}],\"transfers\":[\"lfs-standalone-file\",\"basic\",\"ssh\"],\"ref\":{\"name\":\"refs/heads/main\"}}",
		    "{\"operation\":\"download\",\"objects\":[{\"oid\":\"aa\",\"size\":1},{\"oid\":\"bb\",\"size\":2}],\"transfers\":[\"basic\"],\"ref\":{\"name\":\"refs/heads/main\"}}",
		    "{\"operation\":\"download\",\"objects\":[{\"oid\":\"aa\",\"size\":1},{\"oid\":\"bb\",\"size\":2}],\"transfers\":[\"lfs-standalone-file\",\"basic\",\"ssh\"],\"ref\":{\"name\":\"refs/heads/other\"}}",
		};
		struct gc_buf c0, c1;
		size_t k;

		gc_buf_init(&c0);
		assert(gc_lfs_canon((const uint8_t *)same[0], strlen(same[0]), &c0) == 0);
		for (k = 1; k < 3; k++) {
			gc_buf_init(&c1);
			assert(gc_lfs_canon((const uint8_t *)same[k], strlen(same[k]), &c1) == 0);
			assert(c0.len == c1.len && memcmp(c0.p, c1.p, c0.len) == 0);
			gc_buf_fini(&c1);
		}
		for (k = 0; k < 3; k++) {
			gc_buf_init(&c1);
			assert(gc_lfs_canon((const uint8_t *)differ[k], strlen(differ[k]), &c1) == 0);
			assert(!(c0.len == c1.len && memcmp(c0.p, c1.p, c0.len) == 0));
			gc_buf_fini(&c1);
		}
		gc_buf_init(&c1);
		assert(gc_lfs_canon(c0.p, c0.len, &c1) == 0);
		assert(c0.len == c1.len && memcmp(c0.p, c1.p, c0.len) == 0);
		gc_buf_fini(&c1);
		gc_buf_fini(&c0);
		gc_buf_init(&c0);
		assert(gc_lfs_canon((const uint8_t *)"{\"transfers\":[\"a\",", 18, &c0) == 0);
		assert(c0.len == 18);
		gc_buf_fini(&c0);
		gc_buf_init(&c0);
		assert(gc_lfs_canon((const uint8_t *)"", 0, &c0) == 0 && c0.len == 0);
		gc_buf_fini(&c0);
	}
	{
		struct gc_buf c;
		const char *decoy = "{\"ref\":{\"name\":\"\\\"transfers\\\":[\\\"b\\\",\\\"a\\\"]\"},\"transfers\":[\"b\",\"a\"]}";
		const char *want = "{\"ref\":{\"name\":\"\\\"transfers\\\":[\\\"b\\\",\\\"a\\\"]\"},\"transfers\":[\"a\",\"b\"]}";
		const char *bad[] = { "{\"transfers\":[\"b\",\"a\"", "[\"b\",\"a\"]", "{\"transfers\":[\"b\",\"a\"],}x",
		    "{\"objects\":[{\"oid\":\"b\"},{\"oid\":\"a\"]}", "not json at all" };
		size_t k;

		/* only the top-level array is sorted: the same text inside a string value is left alone */
		gc_buf_init(&c);
		assert(gc_lfs_canon((const uint8_t *)decoy, strlen(decoy), &c) == 0);
		assert(c.len == strlen(want) && memcmp(c.p, want, c.len) == 0);
		gc_buf_fini(&c);
		/* malformed input is passed through untouched, never "fixed" */
		for (k = 0; k < sizeof bad / sizeof bad[0]; k++) {
			gc_buf_init(&c);
			assert(gc_lfs_canon((const uint8_t *)bad[k], strlen(bad[k]), &c) == 0);
			assert(c.len == strlen(bad[k]) && memcmp(c.p, bad[k], c.len) == 0);
			gc_buf_fini(&c);
		}
		/* the operation is read from the top level only */
		gc_buf_init(&c);
		add_str(&c, "{\"ref\":{\"name\":\"\\\"operation\\\":\\\"download\\\"\"},\"operation\":\"upload\"}");
		assert(!gc_lfs_is_download(c.p, c.len));
		gc_buf_fini(&c);
		gc_buf_init(&c);
		add_str(&c, "{\"note\":\"x\",\"operation\" : \"download\", \"objects\":[]}");
		assert(gc_lfs_is_download(c.p, c.len));
		gc_buf_fini(&c);
		gc_buf_init(&c);
		add_str(&c, "[{\"operation\":\"download\"}]");
		assert(!gc_lfs_is_download(c.p, c.len));
		gc_buf_fini(&c);
	}
	{
		/* regression (found by fuzzing): a NUL byte inside a nested value used to stall the scan forever */
		static const char nul1[] = "{\"transfers\":[\0]}";
		static const char nul2[] = "{\"objects\":[{\"oid\":\0\0\0}], \"x\":[[\0";
		static const char nul3[] = "{\"a\":{\"b\":[1,\0,2]},\"operation\":\"download\"}";
		struct gc_buf c;

		gc_buf_init(&c);
		assert(gc_lfs_canon((const uint8_t *)nul1, sizeof nul1 - 1, &c) == 0);
		gc_buf_fini(&c);
		gc_buf_init(&c);
		assert(gc_lfs_canon((const uint8_t *)nul2, sizeof nul2 - 1, &c) == 0);
		gc_buf_fini(&c);
		gc_buf_init(&c);
		gc_buf_add(&c, nul3, sizeof nul3 - 1);
		(void)gc_lfs_is_download(c.p, c.len);
		gc_buf_fini(&c);
	}
	{
		/* a thread's scratch buffers and objects are freed when it exits (LeakSanitizer checks) */
		struct gc_buf z;
		pthread_t t;

		gc_buf_init(&z);
		gz("0123456789", 10, &z);
		assert(pthread_create(&t, NULL, tls_user, &z) == 0);
		assert(pthread_join(t, NULL) == 0);
		gc_buf_fini(&z);
	}
	printf("ok route / lfs-op / gunzip / lfs batch canonicalisation / thread exit\n");
}
