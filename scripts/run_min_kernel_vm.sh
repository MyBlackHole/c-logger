#!/bin/bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/min-kernel-evidence}"
image="c-logger-min-kernel-focal"
version="$(cat "$root/VERSION")"

rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

cleanup() {
  set +e
timeout 10m qemu-system-x86_64 \
  -accel tcg \
  -m 1024 \
  -smp 2 \
  -nographic \
  -no-reboot \
  -kernel "$out/guest-vmlinuz" \
  -append "root=/dev/vda rw console=ttyS0,115200 loglevel=5 init=/usr/local/sbin/c-logger-min-kernel-init" \
  -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-pci,rng=rng0 \
  -d guest_errors \
  -D "$out/qemu-debug.log" \
  -drive "file=$root_disk,format=raw,if=virtio,cache=none" \
  -drive "file=$xfs_disk,format=raw,if=virtio,cache=none" \
  2>&1 | tee "$out/serial.log"
qemu_rc=${PIPESTATUS[0]}
set -e

grep -q '^MIN_KERNEL_EVIDENCE random_ready=1$' "$out/serial.log"
grep -q '^MIN_KERNEL_RUNTIME_PROBE_OK$' "$out/serial.log"
grep -q '^MIN_KERNEL_OK kernel=5\.10\.' "$out/serial.log"

# With -no-reboot, QEMU may return a non-zero status after a guest poweroff
# even though the guest completed validation. The serial success markers above
# are the authoritative protocol result; qemu_rc remains diagnostic only.
if [ "$qemu_rc" -ne 0 ]; then
  echo "QEMU exited with rc=$qemu_rc after MIN_KERNEL_OK" >&2
fi

echo "minimum-kernel baseline validation passed"
