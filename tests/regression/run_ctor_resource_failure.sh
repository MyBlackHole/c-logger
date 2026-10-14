#!/usr/bin/env bash
# #106 真实生产静态库：构造候选、队列与工作区资源逐获取点失败回滚。
set -euo pipefail

build="${1:?usage: run_ctor_resource_failure.sh BUILD_DIR [address,undefined|thread]}"
san="${2:-}"
root="$(pwd)"
mapfile -t libraries < <(find "$build" -type f -name liblogger.a -print)
if [ "${#libraries[@]}" -ne 1 ]; then
    echo "expected exactly one production archive in $build" >&2
    exit 2
fi
library="$(realpath "${libraries[0]}")"
binary="$root/$build/ctor-resource-production"
flags=()
if [ -n "$san" ]; then
    flags+=("-fsanitize=$san" -fno-omit-frame-pointer)
fi

"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DLOGGER_STATIC_DEFINE=1 -Iinclude -Isrc -I"$build/generated" \
    "${flags[@]}" tests/regression/test_ctor_resource_failure.c "$library" \
    -pthread -Wl,--wrap=calloc -Wl,--wrap=malloc -Wl,--wrap=free \
    -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_cond_init \
    -Wl,--wrap=pthread_create -o "$binary"

printf 'SOURCE_COMMIT=%s\n' "$(git rev-parse HEAD)"
printf 'SOURCE_STATUS=%s\n' "$(git status --porcelain --untracked-files=no)"
printf 'SANITIZER=%s\n' "${san:-none}"
"${CC:-cc}" --version | head -n1
sha256sum "$library" "$binary"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/c-logger-ctor-res-XXXXXXXX")"
trap 'rm -rf -- "$tmp"' EXIT
mkdir "$tmp/healthy"
(
    cd "$tmp/healthy"
    timeout 25s "$binary" healthy
)
printf 'RESOURCE_PASS=healthy\n'
for index in {1..9}; do
    mkdir "$tmp/calloc-$index"
    (
        cd "$tmp/calloc-$index"
        timeout 25s "$binary" calloc "$index"
    )
    printf 'RESOURCE_PASS=calloc-%s\n' "$index"
done
for kind in malloc mutex cond; do
    case "$kind" in
        malloc) count=1 ;;
        mutex) count=3 ;;
        cond) count=2 ;;
    esac
    for ((index=1; index<=count; index++)); do
        mkdir "$tmp/$kind-$index"
        (
            cd "$tmp/$kind-$index"
            timeout 25s "$binary" "$kind" "$index"
        )
        printf 'RESOURCE_PASS=%s-%s\n' "$kind" "$index"
    done
done
