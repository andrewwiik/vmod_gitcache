import sys, collections
sha, inf, dig, err = collections.defaultdict(list), collections.defaultdict(list), {}, []
for l in open(sys.argv[1]):
    p = l.rstrip("\n").split("\t")
    if p[0] == "RESULT" and len(p) == 6:
        name, size, a, ns, mbs = p[1], int(p[2]), int(p[3]), float(p[4]), float(p[5])
        if name.split()[0] in ("zlib", "zlib-ng", "libdeflate", "ISA-L"):
            inf[size].append((name, a, ns, mbs))
        else:
            sha[(size, a)].append((name, ns, mbs))
    elif p[0] == "DIGEST": dig[p[1]] = p[2]
    elif p[0] == "ERROR": err.append(l.strip())
ref = collections.Counter(v for k, v in dig.items() if "BLAKE3" not in k).most_common(1)
if ref:
    bad = [k for k, v in dig.items() if v != ref[0][0] and "BLAKE3" not in k]
    print("SHA-256 digests agree across %d backends%s" % (len([k for k in dig if "BLAKE3" not in k]), "" if not bad else "; MISMATCH: " + ", ".join(bad)))
for e in err: print(e)
print()
print("=== SHA-256 / hashing (higher MB/s is better) ===")
for (size, chunk) in sorted(sha):
    rows = sorted(sha[(size, chunk)], key=lambda r: -r[2])
    base = next((r[2] for r in rows if r[0].startswith("Varnish built-in")), None)
    print("\n-- %d byte input, %s" % (size, "single call" if chunk == 0 else "fed in %d byte chunks" % chunk))
    print("   %-34s %10s %10s %8s" % ("backend", "ns/op", "MB/s", "vs Varnish"))
    for n, ns, mbs in rows:
        print("   %-34s %10.0f %10.0f %8s" % (n, ns, mbs, ("%.1fx" % (mbs / base)) if base else ""))
print()
print("=== gzip inflate (higher MB/s of output is better) ===")
for size in sorted(inf):
    rows = sorted(inf[size], key=lambda r: -r[3])
    base = next((r[3] for r in rows if r[0].startswith("zlib 1")), None)
    print("\n-- %d bytes decompressed (%d compressed)" % (size, rows[0][1]))
    print("   %-34s %10s %10s %8s" % ("backend", "ns/op", "MB/s", "vs zlib"))
    for n, a, ns, mbs in rows:
        print("   %-34s %10.0f %10.0f %8s" % (n, ns, mbs, ("%.1fx" % (mbs / base)) if base else ""))
