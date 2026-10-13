#!/bin/bash
# What the cache buys: the same git operations straight from the origin and through Varnish.
set -u
SRC=${SRC:-/src}; UPSTREAM=${UPSTREAM:-github.com}; REPO=${REPO:-libgit2/libgit2}; RUNS=${RUNS:-5}; CLONE_ARGS=${CLONE_ARGS:-}; KEEP=${KEEP:-1}
. "$SRC/tests/e2e/env.sh"
W=$(mktemp -d); chmod 755 $W; export HOME=$W GIT_TERMINAL_PROMPT=0 GIT_CONFIG_GLOBAL=/dev/null
GC_MALLOC=${GC_MALLOC:-1g}
. "$SRC/tests/e2e/up_start.sh"
now(){ date +%s.%N; }
run(){ local t=$(now); "$@" >/dev/null 2>$W/err || { echo "FAILED: $*" >&2; tail -3 $W/err >&2; }; awk -v a="$(now)" -v b="$t" 'BEGIN{printf "%.3f\n", a-b}'; }
med(){ sort -n | awk '{a[NR]=$1} END{printf "%.2f", a[int((NR+1)/2)]}'; }
bytes(){ $VSTAT -n $W/vn -1 -f 'VBE.*.beresp_bodybytes' | awk '{s+=$2} END{print s+0}'; }
hdr(){ printf '%-44s %10s %10s\n' "$1" "direct" "via cache"; }

echo "== $UPSTREAM/$REPO, $RUNS runs each, median wall time (s)"
D=(); C=(); M=""
clone(){ local t; t=$(run git clone --bare $CLONE_ARGS "$1" "$2"); [ "${3:-}" = keep ] || rm -rf "$2"; echo $t; }
keep(){ [ "$KEEP" = 1 ] && [ "$1" = 1 ] && echo keep; }
for i in $(seq $RUNS); do D+=($(clone https://$UPSTREAM/$REPO.git $W/d$i $(keep $i))); done
B0=$(bytes); R0=$(beq)
M=$(clone $PX/$REPO.git $W/m0 keep); SIZE=$(du -sm $W/m0 | cut -f1); rm -rf $W/m0
B1=$(bytes); R1=$(beq)
for i in $(seq $RUNS); do C+=($(clone $PX/$REPO.git $W/c$i $(keep $i))); done
B2=$(bytes); R2=$(beq)
NUKED=$($VSTAT -n $W/vn -1 -f MAIN.n_lru_nuked | awk '{print $2}')
hdr "clone $CLONE_ARGS (${SIZE} MB)"
printf '%-44s %10s %10s\n' "  every clone" "$(printf '%s\n' "${D[@]}" | med)" "$(printf '%s\n' "${C[@]}" | med)"
printf '%-44s %10s %10s\n' "  first clone through an empty cache" "-" "$M"

if [ "$KEEP" = 1 ]; then
cd $W/d1
DF=(); CF=()
for i in $(seq $RUNS); do DF+=($(run git fetch -q https://$UPSTREAM/$REPO.git HEAD)); done
cd $W/c1 && git remote set-url origin $PX/$REPO.git
for i in $(seq $RUNS); do CF+=($(run git fetch -q origin HEAD)); done
hdr "up-to-date fetch (git fetch, nothing new)"
printf '%-44s %10s %10s\n' "  every fetch" "$(printf '%s\n' "${DF[@]}" | med)" "$(printf '%s\n' "${CF[@]}" | med)"
fi

DL=(); CL=()
for i in $(seq 10); do DL+=($(run git ls-remote https://$UPSTREAM/$REPO.git HEAD)); done
for i in $(seq 10); do CL+=($(run git ls-remote $PX/$REPO.git HEAD)); done
hdr "ls-remote (10 runs)"
printf '%-44s %10s %10s\n' "  every run" "$(printf '%s\n' "${DL[@]}" | med)" "$(printf '%s\n' "${CL[@]}" | med)"

N=$((RUNS + 1))
echo
echo "== load on $UPSTREAM for $N clones"
printf '%-44s %10s %10s\n' "" "direct" "via cache"
printf '%-44s %10s %10s\n' "  requests to the origin" "$((N * 2))" "$R2"
printf '%-44s %10s %10s\n' "  MB downloaded from the origin" "~$((N * SIZE))" "$((B2 / 1048576))"
echo "  cache evictions during the run: $NUKED"
