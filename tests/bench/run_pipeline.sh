#!/bin/sh
# Old (as first committed) against new pipeline, from the same harness. Run in the gc-bench image
# with the repository mounted at /repo: podman run --rm -v $PWD:/repo:Z gc-bench sh /repo/tests/bench/run_pipeline.sh
set -e
W=/tmp/pipe; rm -rf $W; mkdir -p $W/old $W/new
cd /repo
git show 76aff5b:src/pktline.c > $W/old/pktline.c; git show 76aff5b:src/pktline.h > $W/old/pktline.h
git show 76aff5b:src/route.c > $W/old/route.c; git show 76aff5b:src/route.h > $W/old/route.h
WRAP="-Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=calloc"
cc -O2 -DOLD -I$W/old -I/opt/vsha/include -I/opt/vsha tests/bench/bench_pipeline.c $W/old/pktline.c $W/old/route.c /opt/vsha/vsha256.c -lz $WRAP -o $W/old/bench 2>$W/old.err || { cat $W/old.err | head; exit 1; }
LD=third_party/libdeflate
cc -O2 -DGC_HAVE_OPENSSL -Isrc -I$LD tests/bench/bench_pipeline.c src/pktline.c src/route.c $LD/lib/utils.c $LD/lib/crc32.c $LD/lib/deflate_decompress.c $LD/lib/gzip_decompress.c $LD/lib/arm/cpu_features.c $LD/lib/x86/cpu_features.c -I$LD/lib -lcrypto -lz $WRAP -o $W/new/bench 2>$W/new.err || { cat $W/new.err | head; exit 1; }
$W/old/bench > $W/old.txt; $W/new/bench > $W/new.txt
python3 - <<'PY'
o = {l.split("\t")[1]: l.rstrip("\n").split("\t") for l in open("/tmp/pipe/old.txt") if l.startswith("RESULT")}
n = {l.split("\t")[1]: l.rstrip("\n").split("\t") for l in open("/tmp/pipe/new.txt") if l.startswith("RESULT")}
print("%-50s %9s %11s %11s %8s   %s" % ("one pack request", "body", "old (us)", "new (us)", "speedup", "mallocs old -> new"))
for k in o:
    a, b = o[k], n[k]
    print("%-50s %9s %11.1f %11.1f %7.1fx   %.0f -> %.0f" % (k, a[2], float(a[4]), float(b[4]), float(a[4]) / float(b[4]), float(a[5]), float(b[5])))
PY
