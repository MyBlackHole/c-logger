#!/bin/sh
set -eu
profile="${1:-production}"
build="${BUILD_DIR:-build}"
case "$profile" in
  fast)       exec ctest --test-dir "$build" --output-on-failure -L fast ;;
  unit)       exec ctest --test-dir "$build" --output-on-failure -L unit ;;
  integration)exec ctest --test-dir "$build" --output-on-failure -L integration ;;
  concurrency)exec ctest --test-dir "$build" --output-on-failure -L concurrency ;;
  reliability)exec ctest --test-dir "$build" --output-on-failure -L reliability ;;
  security)   exec ctest --test-dir "$build" --output-on-failure -L security ;;
  host-owned) exec ctest --test-dir "$build" --output-on-failure -L host-owned ;;
  crypto)     exec ctest --test-dir "$build" --output-on-failure -L crypto ;;
  production) exec ctest --test-dir "$build" --output-on-failure ;;
  *) echo "unknown profile: $profile" >&2; exit 2 ;;
esac
