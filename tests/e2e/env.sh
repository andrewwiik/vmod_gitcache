if command -v vinyld >/dev/null 2>&1; then
  GC_KIND=vinyl; VD=vinyld; VADM=vinyladm; VLOG=vinyllog; VSTAT=vinylstat
  VMODDIR=$(pkg-config --variable=vmoddir vinylapi)
else
  VD=varnishd; VADM=varnishadm; VLOG=varnishlog; VSTAT=varnishstat
  if varnishd -V 2>&1 | grep -qi plus; then
    GC_KIND=enterprise; VMODDIR=/usr/lib/varnish-plus/vmods
  else
    GC_KIND=varnish; VMODDIR=$(pkg-config --variable=vmoddir varnishapi)
  fi
fi
export GC_KIND
