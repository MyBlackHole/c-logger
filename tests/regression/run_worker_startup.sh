#!/usr/bin/env bash
# Link to the current checkout's production static archive, including sanitizers.
set -euo pipefail
build="${1:?usage: run_worker_startup.sh BUILD_DIR [address,undefined|thread]}"
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
binary="$build/worker-startup-regression"
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DLOGGER_STATIC_DEFINE=1 -Iinclude -Isrc -I"$build/generated" \
    "${flags[@]}" tests/regression/test_worker_startup.c "${libraries[0]}" \
    -pthread -Wl,--wrap=pthread_create -Wl,--wrap=pthread_join \
    -Wl,--wrap=pthread_cond_wait -Wl,--wrap=pthread_cond_destroy \
    -Wl,--wrap=free -o "$binary"
printf 'SOURCE_COMMIT=%s\n' "$(git rev-parse HEAD)"
printf 'SOURCE_STATUS=%s\n' "$(git status --porcelain --untracked-files=no)"
"${CC:-cc}" --version | head -n1
sha256sum "${libraries[0]}"
for scenario in normal sync create-error worker-first worker-live \
                join-error destroy-error creator-first global-worker-first; do
    timeout 20s "$binary" "$scenario"
done
