#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define FEED(U) do {								\
	size_t o_ = 0, ch_ = (chunk == 0 || chunk > n) ? n : chunk;		\
	while (o_ < n) {							\
		size_t k_ = n - o_ < ch_ ? n - o_ : ch_;			\
		U(d + o_, k_);							\
		o_ += k_;							\
	}									\
} while (0)

#if defined(B_EVP)
#include <openssl/evp.h>
static EVP_MD_CTX *ctx;
static int outlen = 32;
static void h_setup(void) { ctx = EVP_MD_CTX_new(); }
#define U(p, l) EVP_DigestUpdate(ctx, p, l)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	unsigned l;

	EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
	FEED(U);
	EVP_DigestFinal_ex(ctx, out, &l);
}
#elif defined(B_EVPFETCH)
#include <openssl/evp.h>
static EVP_MD_CTX *ctx;
static EVP_MD *md;
static int outlen = 32;
static void h_setup(void) { ctx = EVP_MD_CTX_new(); md = EVP_MD_fetch(NULL, "SHA256", NULL); }
#define U(p, l) EVP_DigestUpdate(ctx, p, l)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	unsigned l;

	EVP_DigestInit_ex(ctx, md, NULL);
	FEED(U);
	EVP_DigestFinal_ex(ctx, out, &l);
}
#elif defined(B_LOW)
#include <openssl/sha.h>
static SHA256_CTX sc;
static int outlen = 32;
static void h_setup(void) { }
#define U(p, l) SHA256_Update(&sc, p, l)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	SHA256_Init(&sc);
	FEED(U);
	SHA256_Final(out, &sc);
}
#elif defined(B_ONESHOT)
#include <openssl/sha.h>
static int outlen = 32;
static void h_setup(void) { }
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	(void)chunk;
	SHA256(d, n, out);
}
#elif defined(B_SODIUM)
#include <sodium.h>
static crypto_hash_sha256_state st;
static int outlen = 32;
static void h_setup(void) { sodium_init(); }
#define U(p, l) crypto_hash_sha256_update(&st, p, l)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	crypto_hash_sha256_init(&st);
	FEED(U);
	crypto_hash_sha256_final(&st, out);
}
#elif defined(B_NETTLE)
#include <nettle/sha2.h>
static struct sha256_ctx st;
static int outlen = 32;
static void h_setup(void) { }
#define U(p, l) sha256_update(&st, l, p)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	sha256_init(&st);
	FEED(U);
	sha256_digest(&st, 32, out);
}
#elif defined(B_GCRYPT)
#include <gcrypt.h>
static gcry_md_hd_t h;
static int outlen = 32;
static void
h_setup(void)
{
	gcry_check_version(NULL);
	gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
	gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
	gcry_md_open(&h, GCRY_MD_SHA256, 0);
}
#define U(p, l) gcry_md_write(h, p, l)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	gcry_md_reset(h);
	FEED(U);
	memcpy(out, gcry_md_read(h, GCRY_MD_SHA256), 32);
}
#elif defined(B_MBED)
#include <mbedtls/sha256.h>
static mbedtls_sha256_context st;
static int outlen = 32;
static void h_setup(void) { mbedtls_sha256_init(&st); }
#define U(p, l) mbedtls_sha256_update(&st, p, l)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	mbedtls_sha256_starts(&st, 0);
	FEED(U);
	mbedtls_sha256_finish(&st, out);
}
#elif defined(B_WOLF)
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/sha256.h>
static wc_Sha256 st;
static int outlen = 32;
static void h_setup(void) { }
#define U(p, l) wc_Sha256Update(&st, p, l)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	wc_InitSha256(&st);
	FEED(U);
	wc_Sha256Final(&st, out);
}
#elif defined(B_B3)
#include <blake3.h>
static blake3_hasher st;
static int outlen = 32;
static void h_setup(void) { }
#define U(p, l) blake3_hasher_update(&st, p, l)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	blake3_hasher_init(&st);
	FEED(U);
	blake3_hasher_finalize(&st, out, 32);
}
#elif defined(B_VSHA)
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
static VSHA256_CTX st;
static int outlen = 32;
static void h_setup(void) { }
#define U(p, l) VSHA256_Update(&st, p, l)
static void
h_hash(const uint8_t *d, size_t n, size_t chunk, uint8_t *out)
{
	VSHA256_Init(&st);
	FEED(U);
	VSHA256_Final(out, &st);
}
#else
#error no backend selected
#endif

static void
fill(uint8_t *b, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	uint32_t s = 12345;
	size_t i;

	for (i = 0; i < n; i++) {
		s = s * 1103515245u + 12345u;
		b[i] = (i % 50 == 49) ? '\n' : (uint8_t)hx[(s >> 16) & 15];
	}
}

static void
run(size_t size, size_t chunk)
{
	uint8_t *buf = malloc(size), out[64];
	double best = 1e30;
	int rep;
	size_t batch = size < 4096 ? 256 : (size < 200000 ? 16 : 1);

	fill(buf, size);
	for (rep = 0; rep < 3; rep++)
		h_hash(buf, size, chunk, out);
	for (rep = 0; rep < 5; rep++) {
		size_t iters = 0, k;
		double t0 = now(), t1;

		do {
			for (k = 0; k < batch; k++)
				h_hash(buf, size, chunk, out);
			iters += batch;
			t1 = now();
		} while (t1 - t0 < 0.08);
		if ((t1 - t0) / iters * 1e9 < best)
			best = (t1 - t0) / iters * 1e9;
	}
	printf("RESULT\t%s\t%zu\t%zu\t%.1f\t%.1f\n", NAME, size, chunk, best, (double)size / best * 1e3);
	free(buf);
}

int
main(void)
{
	static const size_t sizes[] = { 207, 3507, 102607, 1150107 };
	uint8_t buf[1024], out[64];
	size_t i, j;

	h_setup();
	fill(buf, sizeof buf);
	h_hash(buf, sizeof buf, 0, out);
	printf("DIGEST\t%s\t", NAME);
	for (j = 0; j < (size_t)outlen; j++)
		printf("%02x", out[j]);
	printf("\n");
	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		run(sizes[i], 0);
		if (sizes[i] > 4096)
			run(sizes[i], 4096);
	}
	return (0);
}
