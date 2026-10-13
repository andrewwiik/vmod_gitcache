#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef B_ZNG
#include <zlib.h>
#endif

#ifndef NAME
#define NAME "?"
#endif

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec + t.tv_nsec * 1e-9);
}

#if defined(B_ZLIB)
static void h_setup(void) { }
static size_t
h_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	z_stream zs;
	size_t got;

	memset(&zs, 0, sizeof zs);
	if (inflateInit2(&zs, 15 + 16) != Z_OK)
		return (0);
	zs.next_in = (Bytef *)(uintptr_t)in;
	zs.avail_in = (uInt)n;
	zs.next_out = out;
	zs.avail_out = (uInt)cap;
	if (inflate(&zs, Z_FINISH) != Z_STREAM_END) {
		inflateEnd(&zs);
		return (0);
	}
	got = zs.total_out;
	inflateEnd(&zs);
	return (got);
}
#elif defined(B_ZNG)
#include <zlib-ng.h>
static void h_setup(void) { }
static size_t
h_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	zng_stream zs;
	size_t got;

	memset(&zs, 0, sizeof zs);
	if (zng_inflateInit2(&zs, 15 + 16) != Z_OK)
		return (0);
	zs.next_in = (uint8_t *)(uintptr_t)in;
	zs.avail_in = (uint32_t)n;
	zs.next_out = out;
	zs.avail_out = (uint32_t)cap;
	if (zng_inflate(&zs, Z_FINISH) != Z_STREAM_END) {
		zng_inflateEnd(&zs);
		return (0);
	}
	got = zs.total_out;
	zng_inflateEnd(&zs);
	return (got);
}
#elif defined(B_LIBDEFLATE)
#include <libdeflate.h>
static struct libdeflate_decompressor *dd;
static void h_setup(void) { dd = libdeflate_alloc_decompressor(); }
static size_t
h_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	size_t got = 0;

	if (libdeflate_gzip_decompress(dd, in, n, out, cap, &got) != LIBDEFLATE_SUCCESS)
		return (0);
	return (got);
}
#elif defined(B_ISAL)
#include <isa-l/igzip_lib.h>
static void h_setup(void) { }
static size_t
h_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	struct inflate_state st;

	isal_inflate_init(&st);
	st.next_in = (uint8_t *)(uintptr_t)in;
	st.avail_in = (uint32_t)n;
	st.next_out = out;
	st.avail_out = (uint32_t)cap;
	st.crc_flag = ISAL_GZIP;
	if (isal_inflate_stateless(&st) != ISAL_DECOMP_OK)
		return (0);
	return (st.total_out);
}
#else
#error no backend selected
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

#ifdef B_ZNG
static size_t
gzip9(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	zng_stream zs;

	memset(&zs, 0, sizeof zs);
	zng_deflateInit2(&zs, 9, Z_DEFLATED, 15 + 16, 9, Z_DEFAULT_STRATEGY);
	zs.next_in = (uint8_t *)(uintptr_t)in;
	zs.avail_in = (uint32_t)n;
	zs.next_out = out;
	zs.avail_out = (uint32_t)cap;
	zng_deflate(&zs, Z_FINISH);
	n = zs.total_out;
	zng_deflateEnd(&zs);
	return (n);
}
#else
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
#endif

int
main(void)
{
	static const struct { int w, h; } shapes[] = { { 2, 0 }, { 4, 64 }, { 50, 2000 }, { 3000, 20000 } };
	size_t i;

	h_setup();
	for (i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
		size_t cap = (size_t)shapes[i].w * 60 + (size_t)shapes[i].h * 60 + 4096;
		uint8_t *raw = malloc(cap), *gz = malloc(cap), *out = malloc(cap + 1);
		size_t rn = mk_body(raw, shapes[i].w, shapes[i].h);
		size_t gn = gzip9(raw, rn, gz, cap), got, rep;
		double best = 1e30;
		size_t batch = rn < 4096 ? 64 : (rn < 200000 ? 8 : 1);

		got = h_inflate(gz, gn, out, rn + 1);
		if (got != rn || memcmp(out, raw, rn) != 0) {
			printf("ERROR\t%s\t%zu\tdecompressed output is wrong (%zu != %zu)\n", NAME, rn, got, rn);
			continue;
		}
		for (rep = 0; rep < 5; rep++) {
			size_t iters = 0, k;
			double t0 = now(), t1;

			do {
				for (k = 0; k < batch; k++)
					(void)h_inflate(gz, gn, out, rn + 1);
				iters += batch;
				t1 = now();
			} while (t1 - t0 < 0.08);
			if ((t1 - t0) / iters * 1e9 < best)
				best = (t1 - t0) / iters * 1e9;
		}
		printf("RESULT\t%s\t%zu\t%zu\t%.1f\t%.1f\n", NAME, rn, gn, best, (double)rn / best * 1e3);
		free(raw);
		free(gz);
		free(out);
	}
	return (0);
}
