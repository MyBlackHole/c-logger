#!/usr/bin/env bash
# 用实际 libFuzzer 固定文件回放，核对健康与 UBSan 故障的退出语义。
set -euo pipefail

evidence="${1:?usage: run_fuzz_sanitizer_exit.sh EVIDENCE_DIR}"
compiler="${CC:-clang}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$evidence"
evidence="$(realpath "$evidence")"
source="$evidence/fuzz_sanitizer_exit.c"
binary="$evidence/fuzz-sanitizer-exit"
cp "$root/tests/probes/fuzz_sanitizer_exit.c" "$source"
cp "$root/tests/regression/run_fuzz_sanitizer_exit.sh" "$evidence/runner.sh"
git -C "$root" rev-parse HEAD > "$evidence/SOURCE_COMMIT"
git -C "$root" rev-parse 'HEAD^{tree}' > "$evidence/SOURCE_TREE"
printf 'ASAN_OPTIONS=%s\nUBSAN_OPTIONS=%s\n' \
    "${ASAN_OPTIONS-<unset>}" "${UBSAN_OPTIONS-<unset>}" \
    > "$evidence/sanitizer-env.txt"
"$compiler" --version > "$evidence/compiler.txt" 2>&1
printf '\x00' > "$evidence/healthy.input"
printf '\x01' > "$evidence/signed-overflow.input"

# 与 public harness 的编译模式一致；不得在此单独关闭 UBSan 恢复，
# 也不得覆盖 ASAN_OPTIONS 或 UBSAN_OPTIONS 来掩盖 job 环境的缺失。
command=("$compiler" -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Werror
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    "$source" -o "$binary")
printf '%q ' "${command[@]}" > "$evidence/compile-command.txt"
printf '\n' >> "$evidence/compile-command.txt"
compile_status=0
"${command[@]}" > "$evidence/compile.stdout.log" \
    2> "$evidence/compile.stderr.log" || compile_status=$?
printf '%s\n' "$compile_status" > "$evidence/compile.exitcode"
if [ "$compile_status" -ne 0 ]; then
    cat "$evidence/compile.stderr.log" >&2
    echo 'FUZZ_SANITIZER_EXIT_FAILED compile' >&2
    exit 1
fi
sha256sum "$source" "$evidence/runner.sh" "$binary" \
    "$evidence/healthy.input" "$evidence/signed-overflow.input" \
    > "$evidence/sha256.txt"

run_input() {
    local name="$1" status=0 timed_out=0
    local command=(timeout --kill-after=1s 30s "$binary" -runs=1 -timeout=10
        "-artifact_prefix=$evidence/" "$evidence/$name.input")
    printf '%q ' "${command[@]}" > "$evidence/$name.command.txt"
    printf '\n' >> "$evidence/$name.command.txt"
    "${command[@]}" > "$evidence/$name.stdout.log" \
        2> "$evidence/$name.stderr.log" || status=$?
    if [ "$status" -eq 124 ] || [ "$status" -eq 137 ] || \
        grep -Fq 'ERROR: libFuzzer: timeout' "$evidence/$name.stderr.log"; then
        timed_out=1
    fi
    printf '%s\n' "$status" > "$evidence/$name.exitcode"
    printf '%s\n' "$timed_out" > "$evidence/$name.timeout"
}

# 文件参数只执行给定输入一次；这不是新增的随机 fuzz runs。
run_input healthy
run_input signed-overflow
if [ "$(cat "$evidence/healthy.exitcode")" -ne 0 ] || \
    [ "$(cat "$evidence/healthy.timeout")" -ne 0 ] || \
    ! grep -Fxq 'FUZZ_SANITIZER_HEALTHY=1' "$evidence/healthy.stderr.log" || \
    grep -Eq 'FUZZ_SANITIZER_MISSED_ERROR|runtime error:|ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer)' \
        "$evidence/healthy.stdout.log" "$evidence/healthy.stderr.log"; then
    echo 'FUZZ_SANITIZER_EXIT_FAILED healthy' >&2
    exit 1
fi
if [ "$(cat "$evidence/signed-overflow.exitcode")" -ne 1 ] || \
    [ "$(cat "$evidence/signed-overflow.timeout")" -ne 0 ] || \
    ! grep -Fq 'runtime error: signed integer overflow' "$evidence/signed-overflow.stderr.log" || \
    grep -Eq 'FUZZ_SANITIZER_MISSED_ERROR|FUZZ_SANITIZER_HEALTHY|ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer)' \
        "$evidence/signed-overflow.stdout.log" "$evidence/signed-overflow.stderr.log"; then
    echo 'FUZZ_SANITIZER_EXIT_FAILED signed_overflow' >&2
    exit 1
fi
echo 'FUZZ_SANITIZER_EXIT_OK healthy=0 signed_overflow=1'
