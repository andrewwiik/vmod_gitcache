#!/bin/bash
set -u
. "$(dirname "$0")/env.sh"
SRC=${SRC:-/src}; UPSTREAM=${UPSTREAM:-github.com}; REPO=${REPO:-octocat/Hello-World}
W=$(mktemp -d); chmod 755 $W; export HOME=$W GIT_TERMINAL_PROMPT=0 GIT_CONFIG_GLOBAL=$W/gitconfig
git config --global user.email t@t; git config --global user.name t
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ok   $1"; }; bad(){ FAIL=$((FAIL+1)); echo "  FAIL $1"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1  [$2]"; fi; }
. "$SRC/tests/e2e/up_start.sh"

for PROTO in 2 0; do
  echo "== $UPSTREAM/$REPO via proxy, protocol v$PROTO"
  git -c protocol.version=$PROTO clone -q $PX/$REPO.git $W/p$PROTO-1 2>$W/err1; rc=$?
  check "clone 1 succeeds" "[ $rc -eq 0 ] && [ -d $W/p$PROTO-1/.git ]"
  [ $rc -ne 0 ] && { sed 's/^/       /' $W/err1; $VLOG -n $W/vn -d -g request -i BerespStatus,FetchError,Error,Backend_health 2>&1 | head -10; }
  B1=$(beq)
  git -c protocol.version=$PROTO clone -q $PX/$REPO.git $W/p$PROTO-2 2>/dev/null
  check "clone 2 succeeds" "[ -d $W/p$PROTO-2/.git ]"
  check "clone 2 caused no new upstream requests (backend_req stays $B1)" "[ $(beq) -eq $B1 ]"
  check "clones identical" "[ \"\$(git -C $W/p$PROTO-1 rev-parse HEAD)\" = \"\$(git -C $W/p$PROTO-2 rev-parse HEAD)\" ]"
  check "cached clone passes fsck" "git -C $W/p$PROTO-2 fsck --strict >/dev/null 2>&1"
done
echo "== matches the real upstream"
REAL=$(git ls-remote https://$UPSTREAM/$REPO.git HEAD 2>/dev/null | cut -f1)
check "proxied HEAD == direct HEAD ($REAL)" "[ -n \"$REAL\" ] && [ \"$REAL\" = \"\$(git -C $W/p2-1 rev-parse origin/HEAD 2>/dev/null || git -C $W/p2-1 rev-parse HEAD)\" ]"

echo "== client configuration: pointing git at the cache"
creq(){ $VSTAT -n $W/vn -1 -f MAIN.client_req | awk '{print $2}'; }
git config --global url."$PX/".insteadOf "https://$UPSTREAM/"
git config --global --add url."$PX/".insteadOf "git@$UPSTREAM:"
git config --global url."https://$UPSTREAM/".pushInsteadOf "https://$UPSTREAM/"
C0=$(creq)
git clone -q https://$UPSTREAM/$REPO.git $W/cfg1 2>/dev/null
C1=$(creq)
check "clone of the original https URL is served through the cache" "[ -d $W/cfg1/.git ] && [ $C1 -gt $C0 ]"
check "git rewrites fetches to the cache" "[ \"\$(git -C $W/cfg1 remote get-url origin)\" = $PX/$REPO.git ]"
check "pushes still go straight to the origin" "[ \"\$(git -C $W/cfg1 remote get-url --push origin)\" = https://$UPSTREAM/$REPO.git ]"
git clone -q git@$UPSTREAM:$REPO.git $W/cfg2 2>/dev/null
C2=$(creq)
check "an ssh-style remote (git@host:owner/repo) is routed through the cache too" "[ -d $W/cfg2/.git ] && [ $C2 -gt $C1 ]"
GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0="url.$PX/.insteadOf" GIT_CONFIG_VALUE_0="https://$UPSTREAM/" \
  git clone -q https://$UPSTREAM/$REPO.git $W/cfg3 2>/dev/null
C3=$(creq)
check "the same works from environment variables, with no config file (for CI)" "[ -d $W/cfg3/.git ] && [ $C3 -gt $C2 ]"
git config --global --unset-all url."$PX/".insteadOf
git config --global --unset-all url."https://$UPSTREAM/".pushInsteadOf

if [ -n "${LFS_REPO:-}" ]; then
  echo "== $UPSTREAM LFS flow: $LFS_REPO via proxy${LFS_BATCH:+ (batch caching on)}"
  git lfs install --skip-repo >/dev/null 2>&1
  lfs_clone() {
    GIT_LFS_SKIP_SMUDGE=1 git clone -q $PX/$LFS_REPO.git $1 2>/dev/null || return 1
    (cd $1 && timeout 120 git lfs pull 2>$W/lfs.err)
  }
  B0=$(beq)
  lfs_clone $W/l1; rc=$?
  [ $rc -ne 0 ] && sed 's/^/       /' $W/lfs.err | head -5
  B1=$(beq)
  check "git lfs pull through the proxy succeeds" "[ $rc -eq 0 ]"
  N=$(git -C $W/l1 lfs ls-files | wc -l)
  check "repository has LFS files ($N)" "[ $N -ge 1 ]"
  check "no LFS pointer files left; every object is real content" "! grep -rl '^version https://git-lfs' $W/l1 --include='*' --exclude-dir=.git 2>/dev/null | grep -q ."
  check "git lfs fsck passes" "git -C $W/l1 lfs fsck >/dev/null 2>&1"
  echo "    first clone+pull: $((B1-B0)) origin requests"
  $VLOG -n $W/vn -d -g request -i ReqMethod,ReqURL 2>/dev/null | grep -E 'ReqMethod|ReqURL' | paste - - | grep -i lfs | sed -E 's/ +/ /g' | sort | uniq -c | sed 's/^/    saw: /'
  check "the LFS batch request went through the proxy" "$VLOG -n $W/vn -d -g request -i ReqMethod,ReqURL 2>/dev/null | grep -q '/info/lfs/objects/batch'"
  lfs_clone $W/l2; rc=$?
  B2=$(beq)
  if [ -n "${DEBUG:-}" ]; then
    $VLOG -n $W/vn -d -g request -i ReqMethod,ReqURL,VCL_call,Hit,HitMiss 2>/dev/null | python3 -c "
import sys, re
cur = {}
def flush():
    if cur.get('url'): print('    %-4s %-70s %s' % (cur.get('m'), cur['url'][:70], ','.join(cur.get('calls', []))))
for l in sys.stdin:
    if l.startswith('*   << Request'):
        flush(); cur = {'calls': []}
    elif ' ReqMethod ' in l: cur['m'] = l.split()[-1]
    elif ' ReqURL ' in l: cur['url'] = l.split()[-1]
    elif ' VCL_call ' in l and l.split()[-1] in ('HIT', 'MISS', 'PASS', 'RESTART'): cur['calls'].append(l.split()[-1])
flush()"
  fi
  check "second client's pull succeeds" "[ $rc -eq 0 ] && git -C $W/l2 lfs fsck >/dev/null 2>&1"
  echo "    second clone+pull: $((B2-B1)) origin requests"
  if [ -n "${LFS_BATCH:-}" ]; then
    check "with batch caching the second clone+pull needs no origin request at all" "[ $((B2-B1)) -eq 0 ]"
  else
    check "without batch caching only the batch itself reaches the origin" "[ $((B2-B1)) -le 1 ]"
  fi
fi
echo; echo "passed=$PASS failed=$FAIL"; [ $FAIL -eq 0 ]
