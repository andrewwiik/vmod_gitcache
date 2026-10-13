# Sourced: starts Varnish with the gitcache VCL in front of $UPSTREAM (needs $W, $SRC, env.sh).
if [ "$GC_KIND" = vinyl ]; then
  cat > $W/stunnel.conf <<STUN
pid = $W/stunnel.pid
foreground = no
[origin]
client = yes
accept = 127.0.0.1:8443
connect = $UPSTREAM:443
sni = $UPSTREAM
verifyChain = yes
checkHost = $UPSTREAM
CApath = /etc/ssl/certs
STUN
  stunnel4 $W/stunnel.conf || { echo "stunnel failed"; exit 2; }
  IMPORTS=""; BACKEND_DEF='backend default { .host = "127.0.0.1"; .port = "8443"; }'; SELECT=""
else
  IMPORTS=""; SELECT=""
  BACKEND_DEF="backend default { .host = \"$UPSTREAM\"; .port = \"443\"; .ssl = 1; .ssl_sni = 1; .ssl_verify_peer = 1; .ssl_verify_host = 1; }"
fi
CACHE_ARGS=""; [ -n "${LFS_BATCH:-}" ] && CACHE_ARGS=", lfs_batch = true, lfs_batch_ttl = 60s, refs_ttl = 60s"
python3 - "$UPSTREAM" "$IMPORTS" "$BACKEND_DEF" "$SELECT" "$SRC/tests/e2e/upstream.vcl.in" "$W/up.vcl" "$CACHE_ARGS" <<'PYR'
import sys
up, imports, bdef, sel, src, dst, cargs = sys.argv[1:8]
t = open(src).read()
for k, v in (("@IMPORTS@", imports), ("@BACKEND_DEF@", bdef), ("@BACKEND_SELECT@", sel), ("@CACHE_ARGS@", cargs), ("@UPSTREAM@", up)):
    t = t.replace(k, v)
open(dst, "w").write(t)
PYR
echo e2e > $W/secret
$VD -n $W/vn -a 127.0.0.1:8082 -f $W/up.vcl -s malloc,${GC_MALLOC:-256m} -p vmod_path=${GC_BUILD:-$SRC/build}:$VMODDIR \
  -T 127.0.0.1:6083 -S $W/secret >$W/vd.log 2>&1 || { cat $W/vd.log; exit 2; }
trap 'kill $(cat $W/vn/_.pid 2>/dev/null) $(cat $W/stunnel.pid 2>/dev/null) 2>/dev/null' EXIT
sleep 1
beq(){ $VSTAT -n $W/vn -1 -f MAIN.backend_req | awk '{print $2}'; }
PX=http://127.0.0.1:8082
