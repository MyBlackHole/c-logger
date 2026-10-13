#!/usr/bin/env bash
# R7：同一检出源码的真实生产静态库；注入仅在链接时发生。
set -euo pipefail

build="\${1:?usage: run_global_r7.sh BUILD_DIR [address,undefined|thread]}"
san="\${2:-}"
root="$(pwd)"
mapfile -t archives < <(find "$build" -type f -name liblogger.a -print)
if [ "\${#archives[@]}" -ne 1 ]; then
    echo "expected exactly one production static archive in $build" >&2
    exit 2
fi
library="$(realpath "\${archives[0]}")"
binary="$root/$build/global-r7-production"
flags=()
if [ -n "$san" ]; then
    flags+=("-fsanitize=$san" -fno-omit-frame-pointer)
fi

"\${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DLOGGER_STATIC_DEFINE=1 -Iinclude -Isrc -I"$build/generated" \
    "\${flags[@]}" tests/regression/test_global_lifecycle.c "$library" \
    -pthread -Wl,--wrap=pthread_mutex_lock \
    -Wl,--wrap=pthread_mutex_unlock \
    -Wl,--wrap=pthread_rwlock_rdlock \
    -Wl,--wrap=pthread_rwlock_wrlock \
    -Wl,--wrap=pthread_rwlock_unlock \
    -Wl,--wrap=pthread_join -Wl,--wrap=pthread_cond_destroy \
    -Wl,--wrap=logger_create -Wl,--wrap=logger_destroy_status \
    -Wl,--wrap=logger_file_write -Wl,--wrap=logger_file_reopen \
    -Wl,--wrap=logger_stderr_writev_all -Wl,--wrap=fsync \
    -Wl,--wrap=free -o "$binary"

printf 'SOURCE_COMMIT=%s\n' "$(git rev-parse HEAD)"
printf 'SOURCE_STATUS=%s\n' "$(git status --porcelain --untracked-files=no)"
printf 'SANITIZER=%s\n' "\${san:-none}"
"\${CC:-cc}" --version | head -n 1
sha256sum "$library" "$binary"

# 一个故障用例就是一个全新进程，严禁在 poison/锁所有权不确定时
# 在同一进程重新复位 Global 或销毁仍被持有的 pthread 对象。
temp="$(mktemp -d "\${TMPDIR:-/tmp}/c-logger-r7-XXXXXXXX")"
trap 'rm -rf -- "$temp"' EXIT
for scenario in normal candidate-writer-failure candidate-retain \
                shutdown-writer-failure shutdown-unlock-failure \
                shutdown-retain shutdown-io-receipt; do
    mkdir "$temp/$scenario"
    (
        cd "$temp/$scenario"
        timeout 25s "$binary" r7-stage "$scenario"
    )
    printf 'R7_PASS=%s\n' "$scenario"
done

# 真实读者排空、发布入口及并发解锁/写锁交接复用既有场景；
# 不另造“锁有序等于对象阶段正确”的伪检查。
for scenario in start-gate stop-gate; do
    mkdir "$temp/$scenario"
    (
        cd "$temp/$scenario"
        timeout 25s "$binary" "$scenario"
    )
    printf 'R7_PASS=%s\n' "$scenario"
done
for scenario in writer acquire-fail; do
    mkdir "$temp/handoff-$scenario"
    (
        cd "$temp/handoff-$scenario"
        timeout 25s "$binary" proof-handoff "$scenario"
    )
    printf 'R7_PASS=proof-handoff-%s\n' "$scenario"
done
