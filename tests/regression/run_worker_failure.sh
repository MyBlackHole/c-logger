#!/usr/bin/env bash
# Current-checkout production archive, not the regression-support library.
set -euo pipefail
build="${1:?usage: run_worker_failure.sh BUILD_DIR [address,undefined|thread]}"
san="${2:-}"
mapfile -t libraries < <(find "$build" -type f -name liblogger.a)
if [ "${#libraries[@]}" -ne 1 ]; then
    echo "expected exactly one production archive in $build" >&2
    exit 2
fi
flags=()
if [ -n "$san" ]; then
    flags+=("-fsanitize=$san" -fno-omit-frame-pointer)
fi
binary="$build/worker-failure-regression"
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DLOGGER_STATIC_DEFINE=1 -Iinclude -Isrc -I"$build/generated" \
    "${flags[@]}" tests/regression/test_worker_failure.c "${libraries[0]}" \
    -pthread -Wl,--wrap=pthread_create -Wl,--wrap=pthread_join \
    -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=pthread_mutex_unlock \
    -Wl,--wrap=pthread_cond_wait -Wl,--wrap=pthread_cond_broadcast \
    -Wl,--wrap=strrchr -Wl,--wrap=logger_file_writev -Wl,--wrap=free \
    -o "$binary"
printf 'SOURCE_COMMIT=%s\n' "$(git rev-parse HEAD)"
printf 'SOURCE_STATUS=%s\n' "$(git status --porcelain --untracked-files=no)"
"${CC:-cc}" --version | head -n1
sha256sum "${libraries[0]}"
for scenario in wait-first error-first wait-gap multiple spurious later-slot \
                spill normal-gap normal-drain backend-error empty-target \
                global cancel; do
    timeout 20s "$binary" "$scenario"
done
