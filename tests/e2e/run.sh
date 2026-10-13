#!/bin/bash
set -u
. "$(dirname "$0")/env.sh"
SRC=${SRC:-/src}; W=$(mktemp -d); chmod 755 $W; export HOME=$W; export GIT_TERMINAL_PROMPT=0
export GIT_CONFIG_GLOBAL=$W/gitconfig
git config --global user.email t@t; git config --global user.name t
git config --global protocol.version 2
PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  ok   $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL $1"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1  [$2]"; fi; }
ORIGIN=http://127.0.0.1:9000
stat() { python3 -c "import urllib.request,json; print(json.load(urllib.request.urlopen('$ORIGIN/_stats')).get('$1',0))"; }
reset(){ python3 -c "import urllib.request; urllib.request.urlopen(urllib.request.Request('$ORIGIN/_reset',method='POST',data=b''))"; }
PX=http://127.0.0.1:8081

mkrepo() { # name [object-format]
  git init -q --object-format=${2:-sha1} $W/work-$1 && cd $W/work-$1
  for i in $(seq 1 60); do head -c 3000 /dev/urandom | base64 > f$((i%12)).txt; git add -A; git commit -qm c$i; done
  mkdir -p $W/origin/$(dirname $1); git clone -q --bare . $W/origin/$1.git
  git -C $W/origin/$1.git config uploadpack.allowFilter true
  git -C $W/origin/$1.git config uploadpack.allowAnySHA1InWant true
  cd $W
}
mkrepo pub/open; mkrepo corp/secret; mkrepo pub/par; mkrepo pub/sha sha256; mkrepo dumb/legacy
git -C $W/origin/dumb/legacy.git update-server-info
post(){ python3 -c "import urllib.request; urllib.request.urlopen(urllib.request.Request('$ORIGIN$1',method='POST',data=b''))"; }

python3 $SRC/tests/e2e/ghes_sim.py $W/origin 9000 & SIM=$!
echo e2e-secret > $W/secret
$VD -n $W/vn -a 127.0.0.1:8081 -f $SRC/tests/e2e/git.vcl -s malloc,256m \
  -p vmod_path=${GC_BUILD:-$SRC/build}:$VMODDIR -T 127.0.0.1:6082 -S $W/secret >$W/varnishd.log 2>&1 || { cat $W/varnishd.log; echo "varnishd failed"; exit 2; }
sleep 1
trap 'kill $(cat $W/vn/_.pid 2>/dev/null) $SIM 2>/dev/null' EXIT

echo "== A: public repo, protocol v2, two independent clients"
reset
git clone -q $PX/pub/open.git $W/a1 2>&1; check "clone 1 ok" "[ -f $W/a1/f1.txt ]"
U1=$(stat upload-pack); I1=$(stat info-refs)
check "origin saw requests on first clone" "[ $U1 -ge 1 ] && [ $I1 -ge 1 ]"
git clone -q $PX/pub/open.git $W/a2 2>&1; check "clone 2 ok" "[ -f $W/a2/f1.txt ]"
check "clone 2 identical" "[ \"\$(git -C $W/a1 rev-parse HEAD)\" = \"\$(git -C $W/a2 rev-parse HEAD)\" ]"
check "clone 2 served from cache (upload-pack unchanged: $U1)" "[ $(stat upload-pack) -eq $U1 ]"
check "clone 2 info/refs from cache" "[ $(stat info-refs) -eq $I1 ]"
git -C $W/a2 fsck --strict >/dev/null 2>&1; check "cached clone passes git fsck" "[ $? -eq 0 ]"

echo "== B: public repo, protocol v0"
reset
git -c protocol.version=0 clone -q $PX/pub/open.git $W/b1 2>&1; check "v0 clone 1 ok" "[ -f $W/b1/f1.txt ]"
U1=$(stat upload-pack)
git -c protocol.version=0 clone -q $PX/pub/open.git $W/b2 2>&1; check "v0 clone 2 ok" "[ -f $W/b2/f1.txt ]"
check "v0 clone 2 from cache" "[ $(stat upload-pack) -eq $U1 ]"
git -C $W/b2 fsck --strict >/dev/null 2>&1; check "v0 cached clone passes fsck" "[ $? -eq 0 ]"

echo "== C: private repo credential isolation"
reset
git clone -q http://alice:t-alice@127.0.0.1:8081/corp/secret.git $W/c1 2>&1; check "alice clone ok" "[ -f $W/c1/f1.txt ]"
U1=$(stat upload-pack)
git clone -q http://alice:t-alice@127.0.0.1:8081/corp/secret.git $W/c2 2>&1; check "alice clone 2 ok" "[ -f $W/c2/f1.txt ]"
check "alice clone 2 from cache" "[ $(stat upload-pack) -eq $U1 ]"
git clone -q http://bob:t-bob@127.0.0.1:8081/corp/secret.git $W/c3 >/dev/null 2>&1
check "bob (valid token, no access) is denied" "[ ! -d $W/c3/.git ]"
git clone -q $PX/corp/secret.git $W/c4 >/dev/null 2>&1
check "anonymous is denied (no leak from alice's cache)" "[ ! -d $W/c4/.git ]"
check "origin recorded the denials" "[ $(stat denied) -ge 2 ]"

echo "== D: incremental fetch sees new commits"
cd $W/work-pub/open 2>/dev/null || cd $W/work-pub/open
cd $W/work-pub/open && echo new > new.txt && git add new.txt && git commit -qm newcommit && git push -q $W/origin/pub/open.git HEAD 2>&1; cd $W
sleep 6
git -C $W/a1 fetch -q 2>&1
check "client fetched the new commit through the cache" "git -C $W/a1 log origin/HEAD --oneline | grep -q newcommit"
git -C $W/a1 fsck >/dev/null 2>&1; check "fetched repo passes fsck" "[ $? -eq 0 ]"

echo "== E: large negotiation (git gzips the request body)"
reset
git clone -q $PX/pub/open.git $W/e1 2>&1
cd $W/e1; for i in $(seq 1 120); do git commit -q --allow-empty -m local$i; done; cd $W
cp -a $W/e1 $W/e2
cd $W/work-pub/open && echo n2 > n2.txt && git add n2.txt && git commit -qm second && git push -q $W/origin/pub/open.git HEAD 2>&1; cd $W
sleep 6
reset
GIT_TRACE_PACKET=$W/tp1 git -C $W/e1 fetch -q 2>&1; G1=$(stat gzip-request); U1=$(stat upload-pack)
check "git did send a gzip-compressed request" "[ $G1 -ge 1 ]"
GIT_TRACE_PACKET=$W/tp2 git -C $W/e2 fetch -q 2>&1
if [ -n "${DEBUG:-}" ]; then diff <(sed -E "s/^[0-9:.]+ //" $W/tp1) <(sed -E "s/^[0-9:.]+ //" $W/tp2) | head -${DEBUG_LINES:-30}; fi
check "identical gzipped fetch served from cache (upload-pack stays $U1)" "[ $(stat upload-pack) -eq $U1 ]"

echo "== F: access control around shared packs"
reset
post "/_grant?repo=corp/secret&user=bob"
git clone -q http://bob:t-bob@127.0.0.1:8081/corp/secret.git $W/f1 >/dev/null 2>&1
check "newly granted bob can clone" "[ -f $W/f1/f1.txt ]"
check "bob reused alice's cached pack (only his own ls-refs reached origin: upload-pack=$(stat upload-pack))" "[ $(stat upload-pack) -eq 1 ]"
git -C $W/f1 fsck --strict >/dev/null 2>&1; check "bob's clone passes fsck" "[ $? -eq 0 ]"
reset
git clone -q http://carol:t-carol@127.0.0.1:8081/corp/secret.git $W/f2 >/dev/null 2>&1
check "carol (valid token, no access) cannot clone although the pack is cached" "[ ! -d $W/f2/.git ]"
check "carol never reached a pack: origin upload-pack=0" "[ $(stat upload-pack) -eq 0 ]"
check "origin denied carol" "[ $(stat denied) -ge 1 ]"
git clone -q $PX/pub/open.git $W/f3 >/dev/null 2>&1
check "anonymous clone of public repo works before revocation" "[ -f $W/f3/f1.txt ]"
post "/_private?repo=pub/open"
sleep 6
git clone -q $PX/pub/open.git $W/f4 >/dev/null 2>&1
check "anonymous clone is refused within refs-TTL (5s) of revocation" "[ ! -d $W/f4/.git ]"
git clone -q http://bob:t-bob@127.0.0.1:8081/pub/open.git $W/f5 >/dev/null 2>&1
check "bob (not on the new ACL) refused" "[ ! -d $W/f5/.git ]"
git clone -q http://alice:t-alice@127.0.0.1:8081/pub/open.git $W/f6 >/dev/null 2>&1
check "alice still allowed" "[ -f $W/f6/f1.txt ]"
post "/_public?repo=pub/open"

echo "== G: chunked request bodies and git's probe POST"
mkrepo pub/many
cd $W/work-pub/many; for i in $(seq 1 1500); do git commit -q --allow-empty -m m$i; git tag t$i; done
git push -q --tags $W/origin/pub/many.git 2>&1; cd $W
reset
GIT_TRACE_CURL=$W/curl.trace git -c http.postBuffer=65536 clone -q --no-checkout $PX/pub/many.git $W/g1 2>$W/g1.err
sed "s/^/    g1.err: /" $W/g1.err | head -5
check "clone with a >64KB request body (chunked) succeeds through the proxy" "[ \"\$(git -C $W/g1 tag | wc -l)\" -ge 1500 ]"
check "git really sent Transfer-Encoding: chunked" "grep -qi 'Transfer-Encoding: chunked' $W/curl.trace"
check "git sent the bare 0000 probe; passed through uncached (origin probe=$(stat probe))" "[ $(stat probe) -ge 1 ]"
U=$(( $(stat upload-pack) - $(stat probe) ))
git -c http.postBuffer=65536 clone -q --no-checkout $PX/pub/many.git $W/g2 2>/dev/null
check "second large-request clone is identical" "[ \"\$(git -C $W/g1 rev-parse HEAD)\" = \"\$(git -C $W/g2 rev-parse HEAD)\" ]"
check "second large-request clone passes through uncached (chunked bodies are never keyed)" "[ \$(( $(stat upload-pack) - $(stat probe) )) -gt $U ]"
git -C $W/g2 fsck >/dev/null 2>&1; check "large-request clone passes fsck" "[ $? -eq 0 ]"

echo "== H: shallow, partial, sha256, concurrency"
reset
git clone -q --depth 1 $PX/pub/open.git $W/h1 2>&1; U=$(stat upload-pack)
git clone -q --depth 1 $PX/pub/open.git $W/h2 2>&1
check "shallow clones both ok and really shallow" "[ \"\$(git -C $W/h1 rev-list --count HEAD)\" = 1 ] && [ \"\$(git -C $W/h2 rev-list --count HEAD)\" = 1 ]"
check "second shallow clone from cache" "[ $(stat upload-pack) -eq $U ]"
reset
git clone -q --filter=blob:none --no-checkout $PX/pub/open.git $W/h3 2>&1; U=$(stat upload-pack)
git clone -q --filter=blob:none --no-checkout $PX/pub/open.git $W/h4 2>&1
check "partial clones ok" "[ -d $W/h3/.git ] && [ -d $W/h4/.git ]"
check "second partial clone from cache" "[ $(stat upload-pack) -eq $U ]"
git -C $W/h3 checkout -q HEAD 2>&1; U=$(stat upload-pack)
git -C $W/h4 checkout -q HEAD 2>&1
check "lazy blob fetch works through the cache" "[ -f $W/h3/f1.txt ] && [ -f $W/h4/f1.txt ]"
check "second lazy blob fetch from cache" "[ $(stat upload-pack) -eq $U ]"
git -C $W/h4 fsck >/dev/null 2>&1; check "partial clone passes fsck" "[ $? -eq 0 ]"
reset
git clone -q $PX/pub/sha.git $W/h5 2>$W/h5.err; U=$(stat upload-pack)
sed "s/^/    h5.err: /" $W/h5.err | head -8
if [ ! -d $W/h5/.git ]; then
  echo "    -- diagnosing sha256: direct to origin --"
  git clone -q http://127.0.0.1:9000/pub/sha.git $W/h5direct 2>&1 | sed "s/^/    direct: /" | head -5
  GIT_TRACE_PACKET=1 git ls-remote $PX/pub/sha.git 2>&1 | sed "s/^[0-9:.]* //" | head -12 | sed "s/^/    via-proxy: /"
fi
check "sha256 repository clones through the cache" "[ -f $W/h5/f1.txt ] && [ \"\$(git -C $W/h5 rev-parse --show-object-format)\" = sha256 ]"
git clone -q $PX/pub/sha.git $W/h6 2>/dev/null
check "second sha256 clone from cache" "[ $(stat upload-pack) -eq $U ]"
git -C $W/h6 fsck --strict >/dev/null 2>&1; check "sha256 clone passes fsck" "[ $? -eq 0 ]"
reset
PIDS=""; for i in 1 2 3 4 5 6 7 8; do git clone -q $PX/pub/par.git $W/par$i >/dev/null 2>&1 & PIDS="$PIDS $!"; done; wait $PIDS
check "8 parallel clones all succeed" "[ $(ls -d $W/par*/.git 2>/dev/null | wc -l) -eq 8 ]"
check "parallel clones coalesced into <=3 origin pack requests (got $(stat upload-pack); 16 without coalescing)" "[ $(stat upload-pack) -le 3 ]"
check "parallel clones coalesced: info/refs <=2 (got $(stat info-refs))" "[ $(stat info-refs) -le 2 ]"
check "all parallel clones identical" "[ \$(for i in 1 2 3 4 5 6 7 8; do git -C $W/par\$i rev-parse HEAD; done | sort -u | wc -l) -eq 1 ]"

echo "== I: dumb HTTP protocol"
reset
git clone -q $PX/dumb/legacy.git $W/i1 2>$W/i1.err
check "dumb-protocol clone works through the cache" "[ -f $W/i1/f1.txt ]"
D=$(stat dumb-file)
git clone -q $PX/dumb/legacy.git $W/i2 2>/dev/null
check "second dumb clone served entirely from cache (origin dumb-file stays $D)" "[ -f $W/i2/f1.txt ] && [ $(stat dumb-file) -eq $D ]"
git -C $W/i2 fsck --strict >/dev/null 2>&1; check "dumb clone passes fsck" "[ $? -eq 0 ]"

echo "== J: webhook-driven invalidation (push)"
git clone -q $PX/pub/par.git $W/j1 2>&1
git -C $W/j1 fetch -q 2>&1
cd $W/work-pub/par && echo j > j.txt && git add j.txt && git commit -qm jcommit && git push -q $W/origin/pub/par.git HEAD 2>&1; cd $W
git -C $W/j1 fetch -q 2>&1
STALE=$(git -C $W/j1 log origin/HEAD --oneline | grep -c jcommit)
check "without a purge, the new tip is not yet visible inside the TTL (control)" "[ $STALE -eq 0 ]"
purge_status() {
python3 - "$1" "$2" "$3" <<PY
import sys, urllib.request, urllib.error
m, tok, repo = sys.argv[1:4]
req = urllib.request.Request("$PX/_gitcache/purge?repo=" + repo, method=m, data=b"" if m == "POST" else None)
if tok: req.add_header("X-Purge-Token", tok)
try: print(urllib.request.urlopen(req).status)
except urllib.error.HTTPError as e: print(e.code)
PY
}
check "purge without a token is refused (403)" "[ \"\$(purge_status POST '' Pub/Par)\" = 403 ]"
check "purge with a wrong token is refused (403)" "[ \"\$(purge_status POST wrong Pub/Par)\" = 403 ]"
check "purge requires POST (405)" "[ \"\$(purge_status GET e2e-purge Pub/Par)\" = 405 ]"
check "malformed repository name is rejected (400), no ban expression injection" "[ \"\$(purge_status POST e2e-purge a%20b)\" = 400 ]"
check "purge with the token succeeds (200)" "[ \"\$(purge_status POST e2e-purge Pub/Par)\" = 200 ]"
git -C $W/j1 fetch -q 2>&1
check "after the purge webhook the new tip is visible immediately" "git -C $W/j1 log origin/HEAD --oneline | grep -q jcommit"
git -C $W/j1 fsck >/dev/null 2>&1; check "purged-then-fetched repo passes fsck" "[ $? -eq 0 ]"

if [ -n "${DEBUG:-}" ]; then
  $VLOG -n $W/vn -d -g request -i ReqMethod,ReqHeader,VCL_call,Hit,HitMiss,HitPass,ReqAcct 2>&1 | grep -E 'ReqMethod|Content-(Length|Encoding)|X-GC-(Kind|Key)|VCL_call +(HIT|MISS|PASS)|HitMiss' | tail -${DEBUG_LINES:-60}
fi
echo; echo "passed=$PASS failed=$FAIL"; [ $FAIL -eq 0 ]
