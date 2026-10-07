#!/usr/bin/env bash
# Test the production archive from this checkout, not a regression-support DSO.
set -euo pipefail
build="${1:?usage: run_audit_format.sh BUILD_DIR [address,undefined]}"
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
binary="$build/audit-format-regression"
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DLOGGER_STATIC_DEFINE=1 -Iinclude -I"$build/generated" \
    "${flags[@]}" tests/regression/test_audit_format.c "${libraries[0]}" \
    -pthread -Wl,--wrap=clock_gettime -Wl,--wrap=localtime_r \
    -Wl,--wrap=strftime -o "$binary"
printf 'SOURCE_COMMIT=%s\n' "$(git rev-parse HEAD)"
"${CC:-cc}" --version | head -n1
sha256sum "${libraries[0]}"
for scenario in normal cache-hit localtime date zone bad-date bad-zone cached-bad truncation; do
    timeout 15s "$binary" "$scenario"
done

# Private formatter boundaries are linked against the same production archive.
unit="$build/format-checked-regression"
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DLOGGER_STATIC_DEFINE=1 -Iinclude -Isrc -I"$build/generated" \
    "${flags[@]}" tests/regression/test_format_checked.c "${libraries[0]}" \
    -pthread -Wl,--wrap=vsnprintf -o "$unit"
timeout 15s "$unit"
