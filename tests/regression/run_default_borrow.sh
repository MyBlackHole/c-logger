#!/usr/bin/env bash
# 默认槽仅借用：真实生产归档验证代次、借用归还和宿主处置。
set -euo pipefail

build="${1:?usage: run_default_borrow.sh BUILD_DIR [address,undefined|thread]}"
san="${2:-}"
root="$(pwd)"
build="$(realpath "$build")"
mapfile -t archives < <(find "$build" -type f -name liblogger.a -print)
if [ "${#archives[@]}" -ne 1 ]; then
    echo "expected exactly one production static archive in $build" >&2
    exit 2
fi
library="${archives[0]}"
flags=()
if [ -n "$san" ]; then
    flags+=("-fsanitize=$san" -fno-omit-frame-pointer)
fi
common=(-std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror
        -DLOGGER_STATIC_DEFINE=1 -Iinclude -Isrc -I"$build/generated")
wrap=()
for symbol in pthread_mutex_lock pthread_mutex_unlock pthread_rwlock_rdlock \
              pthread_rwlock_wrlock pthread_rwlock_unlock pthread_setcancelstate \
              logger_runtime_free logger_vlog_internal; do
    wrap+=("-Wl,--wrap=$symbol")
done
"${CC:-cc}" "${common[@]}" "${flags[@]}" tests/regression/test_default_binding.c \
    "$library" -pthread "${wrap[@]}" -o "$build/default-borrow-production"
"${CC:-cc}" "${common[@]}" "${flags[@]}" tests/regression/test_global_lifecycle.c \
    "$library" -pthread -Wl,--wrap=pthread_rwlock_rdlock \
    -Wl,--wrap=pthread_rwlock_wrlock -Wl,--wrap=logger_file_write \
    -o "$build/default-lifecycle-production"
printf 'SOURCE_COMMIT=%s\n' "$(git rev-parse HEAD)"
printf 'SOURCE_STATUS=%s\n' "$(git status --porcelain --untracked-files=no)"
printf 'SANITIZER=%s\n' "${san:-none}"
"${CC:-cc}" --version | head -n 1
sha256sum "$library" "$build/default-borrow-production" "$build/default-lifecycle-production"

# 每项故障由独立进程执行；未知解锁不在原进程强行重置后继续测。
temp="$(mktemp -d "${TMPDIR:-/tmp}/c-logger-default-XXXXXXXX")"
trap 'rm -rf -- "$temp"' EXIT
run_case() {
    local name="$1" binary="$2"
    shift 2
    mkdir "$temp/$name"
    (cd "$temp/$name"; timeout 25s "$binary" "$@")
    printf 'DEFAULT_BORROW_PASS=%s\n' "$name"
}
for scenario in held-reader stale-reader stale-control aba cancel control-error \
                writer-error read-error lifetime-unlock control-unlock \
                restore-error reentry stopped overflow high-generation \
                last-generation; do
    run_case "$scenario" "$build/default-borrow-production" "$scenario"
done
for scenario in contract closed-gate; do
    run_case "$scenario" "$build/default-lifecycle-production" "$scenario"
done
for op in write flush flush-void reopen level dropped metrics io diagnostics file-metrics; do
    run_case "generation-$op" "$build/default-lifecycle-production" generation "$op"
done
