#!/usr/bin/env bash
# 生产归档逐点验证文件目录/协调锁/保留目录/fdopendir 获取故障。
set -euo pipefail

build="${1:?usage: run_file_acquisition.sh BUILD_DIR [address,undefined|thread]}"
san="${2:-}"
root="$(pwd)"
mapfile -t libraries < <(find "$build" -type f -name liblogger.a -print)
if [ "${#libraries[@]}" -ne 1 ]; then
    echo "expected exactly one production archive in $build" >&2
    exit 2
fi
library="$(realpath "${libraries[0]}")"
binary="$root/$build/file-acquisition-production"
flags=()
if [ -n "$san" ]; then
    flags+=("-fsanitize=$san" -fno-omit-frame-pointer)
fi

"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DLOGGER_STATIC_DEFINE=1 -Iinclude -Isrc -I"$build/generated" \
    "${flags[@]}" tests/regression/test_file_acquisition.c "$library" \
    -pthread -Wl,--wrap=open -Wl,--wrap=openat -Wl,--wrap=fdopendir \
    -Wl,--wrap=close -Wl,--wrap=closedir -o "$binary"

printf 'SOURCE_COMMIT=%s\n' "$(git rev-parse HEAD)"
printf 'SOURCE_STATUS=%s\n' "$(git status --porcelain --untracked-files=no)"
printf 'SANITIZER=%s\n' "${san:-none}"
"${CC:-cc}" --version | head -n1
sha256sum "$library" "$binary"

temp="$(mktemp -d "${TMPDIR:-/tmp}/c-logger-file-acq-XXXXXXXX")"
trap 'rm -rf -- "$temp"' EXIT

for scenario in healthy bind-directory lock-file retention-directory fdopendir; do
    mkdir "$temp/$scenario"
    (
        cd "$temp/$scenario"
        timeout 25s "$binary" "$scenario"
    )
    printf 'FILE_RESOURCE_PASS=%s\n' "$scenario"
done
