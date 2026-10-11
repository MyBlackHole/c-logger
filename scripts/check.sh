#!/bin/sh
set -eu

profile="${1:-production}"
build="${BUILD_DIR:-build}"
jobs="${JOBS:-1}"
mode="${MODE:-release}"
shared="${BUILD_SHARED:-n}"

case "$shared" in
  y|n) ;;
  *) echo "BUILD_SHARED must be y or n" >&2; exit 2 ;;
esac

if [ -z "${LOGGER_PROJECT_VERSION:-}" ]; then
  LOGGER_PROJECT_VERSION="$(cat VERSION)"
  export LOGGER_PROJECT_VERSION
fi

configure_full() {
  xmake f -m "$mode" -o "$build" \
    --build_shared="$shared" \
    --legacy_fork=n \
    --build_tests=y \
    --build_private_tests=y \
    --build_regression_tests=y
}

case "$profile" in
  fast)
    xmake f -m "$mode" -o "$build" \
      --build_shared="$shared" \
      --legacy_fork=n \
      --build_tests=y \
      --build_private_tests=n \
      --build_regression_tests=n
    exec xmake test -j"$jobs"
    ;;

  crash)
    xmake f -m release -o "$build" \
      --build_shared="$shared" \
      --legacy_fork=n \
      --build_tests=n \
      --build_private_tests=y \
      --build_regression_tests=n
    exec xmake test -g process-crash -j1
    ;;

  legacy-fork)
    xmake f -m release -o "$build" \
      --build_shared=n \
      --legacy_fork=y \
      --build_tests=n \
      --build_private_tests=n \
      --build_regression_tests=y
    exec xmake test -j1 'fork_reinit_regression/*'
    ;;

  production)
    configure_full
    exec xmake test -j1
    ;;

  unit|integration|concurrency|reliability|security|host-owned)
    echo "profile '$profile' is a compatibility alias; running the complete Xmake suite" >&2
    configure_full
    exec xmake test -j1
    ;;

  *)
    echo "usage: scripts/check.sh {fast|production|crash|legacy-fork|unit|integration|concurrency|reliability|security|host-owned}" >&2
    exit 2
    ;;
esac
