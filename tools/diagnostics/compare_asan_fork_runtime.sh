#!/usr/bin/env bash
# 独立运行库诊断；不构建 Logger，不替代 Xmake 验收。
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out=${1:-$(mktemp -d)}
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)
cc=${CC:-gcc}
llvm_linker=${LLVM_LINKER:-clang}
"$cc" --version > "$out/compiler.txt"
"$llvm_linker" --version > "$out/llvm-linker.txt"
uname -a > "$out/kernel.txt"
getconf GNU_LIBC_VERSION > "$out/glibc.txt"
cp -- "$root/asan_fork_probe.c" "$root/asan_atfork_control.c" "$out/"
flags=(-std=gnu11 -g -O1 -fno-omit-frame-pointer -Wall -Wextra -Werror)
{
    set -x
    "$cc" "${flags[@]}" -pthread -fsanitize=address,undefined \
        -c "$root/asan_fork_probe.c" -o "$out/probe-gcc.o"
    "$cc" "${flags[@]}" -c "$root/asan_atfork_control.c" \
        -o "$out/atfork-keep.o"
    "$cc" "${flags[@]}" -DDROP_SANITIZER_ATFORK=1 \
        -c "$root/asan_atfork_control.c" -o "$out/atfork-drop.o"
    "$cc" -pthread -fsanitize=address,undefined "$out/probe-gcc.o" \
        "$out/atfork-keep.o" -Wl,--wrap=pthread_atfork \
        -o "$out/gcc-runtime"
    "$llvm_linker" -pthread -fsanitize=address,undefined "$out/probe-gcc.o" \
        "$out/atfork-keep.o" -Wl,--wrap=pthread_atfork \
        -o "$out/llvm-runtime"
    "$llvm_linker" -pthread -fsanitize=address,undefined "$out/probe-gcc.o" \
        "$out/atfork-drop.o" -Wl,--wrap=pthread_atfork \
        -o "$out/llvm-runtime-no-atfork"
    set +x
} > "$out/build.log" 2>&1
sha256sum "$out"/*.o "$out/gcc-runtime" "$out/llvm-runtime" \
    "$out/llvm-runtime-no-atfork" > "$out/binaries.sha256"
# 所有结果独立记录；负向对照不算通过，也不修改正常运行库或 Logger。
result=0
: > "$out/exit-codes.tsv"
for name in gcc-runtime llvm-runtime llvm-runtime-no-atfork; do
    status=0
    timeout --kill-after=5s 220s "$out/$name" > "$out/$name.log" 2>&1 || status=$?
    printf '%s\t%s\n' "$name" "$status" >> "$out/exit-codes.tsv"
    printf 'SANITIZER_FORK_RUN=%s EXIT=%s\n' "$name" "$status"
    if ((status != 0)); then
        result=1
    fi
done
printf 'SANITIZER_FORK_EVIDENCE=%s\n' "$out"
exit "$result"
