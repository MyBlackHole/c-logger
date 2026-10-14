#!/usr/bin/env bash
# 当前源码静态目标的格式化属性、校验器自检与原生模糊测试。
set -euo pipefail
build="${1:?usage: run_format_properties.sh BUILD_DIR [plain|address-undefined|thread|fuzzer]}"
kind="${2:-plain}"
compiler="${CC:-cc}"
mapfile -t libraries < <(find "$build" -type f -name liblogger.a -print)
test "${#libraries[@]}" -eq 1
library="$(realpath "${libraries[0]}")"
evidence="$build/format-evidence"
mkdir -p "$evidence"
binary="$(realpath "$build")/format-properties"
flags=()
case "$kind" in
    plain) ;;
    address-undefined) flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer) ;;
    thread) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
    fuzzer) flags+=(-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer) ;;
    *) echo "unknown validation kind: $kind" >&2; exit 2 ;;
esac
if [ "$kind" != fuzzer ]; then
    flags+=(-DLOGGER_FUZZ_STANDALONE=1)
else
    nm -u "$library" > "$evidence/coverage-symbols.txt"
    grep -Fq '__sanitizer_cov_8bit_counters_init' "$evidence/coverage-symbols.txt"
    grep -Fq '__sanitizer_cov_pcs_init' "$evidence/coverage-symbols.txt"
fi
"$compiler" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DLOGGER_STATIC_DEFINE=1 -Iinclude -Isrc -I"$build/generated" \
    "${flags[@]}" tests/fuzz/format_fuzz.c "$library" -pthread -o "$binary"
git rev-parse HEAD > "$evidence/SOURCE_COMMIT"
"$compiler" --version > "$evidence/compiler.txt"
sha256sum "$library" "$binary" > "$evidence/sha256.txt"
export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export TSAN_OPTIONS=halt_on_error=1
if [ "$kind" = fuzzer ]; then
    mkdir -p "$evidence/corpus"
    cp tests/fuzz/format-corpus/* "$evidence/corpus/"
    timeout 120s "$binary" -seed=209 -runs=3000 -max_len=128 -timeout=10 \
        -artifact_prefix="$(realpath "$evidence")/" "$evidence/corpus" \
        2>&1 | tee "$evidence/libfuzzer.log"
else
    for seed in 1 107 20261015 4294967295 selftest; do
        timeout 30s "$binary" "$seed" | tee -a "$evidence/properties.log"
    done
fi
