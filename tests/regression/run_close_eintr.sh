#!/usr/bin/env bash
# close(EINTR)：使用真实生产静态归档，只在测试可执行文件中包装 close。
set -euo pipefail

build="${1:?usage: run_close_eintr.sh BUILD_DIR [address,undefined|thread]}"
san="${2:-}"
root="$(pwd)"
mapfile -t libraries < <(find "$build" -type f -name liblogger.a)
if [ "${#libraries[@]}" -ne 1 ]; then
    echo "expected exactly one production archive in $build" >&2
    exit 2
fi
library="$(realpath "${libraries[0]}")"
binary="$root/$build/close-eintr-production"
flags=()
if [ -n "$san" ]; then
    flags+=("-fsanitize=$san" -fno-omit-frame-pointer)
fi

"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DLOGGER_STATIC_DEFINE=1 -Iinclude -Isrc -I"$build/generated" \
    "${flags[@]}" tests/regression/test_destroy_status.c "$library" \
    -pthread -Wl,--wrap=logger_runtime_free -Wl,--wrap=close \
    -Wl,--wrap=pthread_join -Wl,--wrap=pthread_cond_destroy \
    -Wl,--wrap=pthread_mutex_destroy -o "$binary"

printf 'SOURCE_COMMIT=%s\n' "$(git rev-parse HEAD)"
printf 'SOURCE_STATUS=%s\n' "$(git status --porcelain --untracked-files=no)"
printf 'SANITIZER=%s\n' "${san:-none}"
"${CC:-cc}" --version | head -n1
sha256sum "$library" "$binary"

temp="$(mktemp -d "${TMPDIR:-/tmp}/c-logger-close-eintr-XXXXXXXX")"
trap 'rm -rf -- "$temp"' EXIT
for scenario in close-error close-eintr; do
    mkdir "$temp/$scenario"
    (
        cd "$temp/$scenario"
        timeout 25s "$binary" "$scenario"
    )
    printf 'CLOSE_PASS=%s\n' "$scenario"
done
