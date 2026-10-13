#!/bin/sh
HERE=$(cd "$(dirname "$0")" && pwd)
ENVS="-e DEBUG -e DEBUG_LINES -e UPSTREAM -e REPO -e LFS_REPO -e LFS_BATCH -e RUNS -e CLONE_ARGS -e KEEP -e GC_MALLOC"
case "${GC_FLAVOUR:-enterprise}" in
  enterprise) IMG=${ENTERPRISE_IMAGE:-enterprise-build:6.0.18r5} ;;
  varnish)    IMG=${VARNISH_IMAGE:-varnish9-build:9.0.4} ;;
  vinyl)      IMG=${VINYL_IMAGE:-vinyl-build:9.0.2} ;;
  *) echo "GC_FLAVOUR must be enterprise, varnish or vinyl" >&2; exit 2 ;;
esac
exec podman run --rm $ENVS --network "${PODMAN_NET:-bridge}" --entrypoint make \
  -v "$HERE":/src:Z -w /src "$IMG" "$@"
