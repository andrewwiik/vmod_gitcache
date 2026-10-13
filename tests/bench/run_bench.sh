#!/bin/sh
# Builds every available backend of the two harnesses, runs them, and prints ranked tables.
# Runs inside the gc-bench image: podman run --rm -v $PWD/tests/bench:/bench gc-bench sh run_bench.sh
cd /bench
OUT=/tmp/bench.out; rm -rf $OUT; mkdir -p $OUT
CC="cc -O2 -Wno-deprecated-declarations"
st() { ls "$1" 2>/dev/null | head -1; }
AWS=$(st /opt/awslc/lib/libcrypto.a); [ -z "$AWS" ] && AWS=$(st /opt/awslc/lib64/libcrypto.a)
LSSL=$(st /opt/libressl/lib/libcrypto.a)
BSSL=/opt/boringssl/lib/libcrypto.a
ok=0
build() { # kind name flags... ; builds $OUT/<kind>_<n>
	kind=$1; name=$2; shift 2
	f=$OUT/${kind}_$(echo "$name" | tr -c 'A-Za-z0-9\n' '_')
	if $CC bench_$kind.c -DNAME="\"$name\"" "$@" -o $f 2>$f.err; then rm -f $f.err; else echo "SKIPPED $name: $(grep -m1 -i error $f.err | cut -c1-150)"; fi
}
for api in "EVP:B_EVP" "low-level:B_LOW" "one-shot:B_ONESHOT"; do
	lab=${api%%:*}; def=-D${api##*:}
	build sha "OpenSSL 3.5 $lab" $def -lcrypto
	[ -n "$AWS" ] && build sha "aws-lc $lab" $def -I/opt/awslc/include $AWS -lpthread -ldl
	[ -f "$BSSL" ] && build sha "BoringSSL $lab" $def -I/opt/boringssl/include $BSSL -lpthread -lstdc++
	[ -n "$LSSL" ] && build sha "LibreSSL $lab" $def -I/opt/libressl/include $LSSL -lpthread
done
build sha "OpenSSL 3.5 EVP prefetched" -DB_EVPFETCH -lcrypto
build sha "libsodium" -DB_SODIUM -lsodium
build sha "nettle" -DB_NETTLE -lnettle
build sha "libgcrypt" -DB_GCRYPT -lgcrypt -lgpg-error
build sha "mbedTLS" -DB_MBED -lmbedcrypto
build sha "wolfSSL" -DB_WOLF -lwolfssl
build sha "Varnish built-in VSHA256" -DB_VSHA -I/opt/vsha/include -I/opt/vsha /opt/vsha/vsha256.c
build sha "BLAKE3 (not SHA-256)" -DB_B3 -I/opt/blake3/include -L/opt/blake3/lib -lblake3
build inflate "zlib 1.3" -DB_ZLIB -lz
build inflate "zlib-ng 2.3" -DB_ZNG -I/opt/zlibng/include /opt/zlibng/lib/libz-ng.a
build inflate "libdeflate 1.23" -DB_LIBDEFLATE -ldeflate -lz
build inflate "ISA-L 2.31" -DB_ISAL -lisal -lz
for b in $OUT/sha_* $OUT/inflate_*; do case "$b" in *.err) continue;; esac; [ -x "$b" ] && $b; done > $OUT/all.txt 2>&1
python3 /bench/summarize.py /tmp/bench.out/all.txt
