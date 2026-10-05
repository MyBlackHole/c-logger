#!/bin/bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/min-kernel-evidence}"
image="c-logger-min-kernel-focal"
version="$(cat "$root/VERSION")"

rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

cid=""
cleanup() {
  set +e
  if mountpoint -q "$out/root-mnt"; then
    sudo umount "$out/root-mnt"
  fi
  if [ -n "${cid:-}" ]; then
    docker rm -f "$cid" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

echo "building Ubuntu 20.04 / Linux 5.10 guest image"
docker build \
  --build-arg KERNEL_VERSION=5.10.271 \
  --build-arg XMAKE_VERSION=3.1.1 \
  -t "$image" \
  -f "$root/scripts/min-kernel/Dockerfile.focal" \
  "$root"

src="$out/src"
mkdir -p "$src"
git -C "$root" archive HEAD | tar -x -C "$src"

echo "building one authoritative shared production candidate on glibc 2.31"
docker run --rm \
  -e LOGGER_PROJECT_VERSION="$version" \
  -v "$src:/opt/src" \
  -w /opt/src \
  "$image" \
  bash -lc '
    set -euo pipefail
    test "$(getconf GNU_LIBC_VERSION)" = "glibc 2.31"
    xmake f --root -m release -o min-kernel-build --build_shared=y
    xmake --root -j2 logger
    xmake install --root -o min-kernel-prefix logger
    export PKG_CONFIG_LIBDIR="$PWD/min-kernel-prefix/lib/pkgconfig"
    cc -std=c11 -Wall -Wextra -Wpedantic -Werror \
      tests/integration/test_min_kernel_runtime.c \
      $(pkg-config --cflags --libs logger) \
      -o min-kernel-runtime
  '

cid="$(docker create "$image")"
docker cp "$src/min-kernel-prefix/." "$cid:/opt/c-logger-prefix"
docker cp "$src/min-kernel-runtime" "$cid:/opt/min-kernel-runtime"
docker cp "$root/scripts/min-kernel/guest-init.sh" \
  "$cid:/usr/local/sbin/c-logger-min-kernel-init"

docker cp "$cid:/opt/min-kernel/bzImage" "$out/guest-vmlinuz"
docker cp "$cid:/opt/min-kernel/compiler.txt" "$out/kernel-compiler.txt"
docker cp "$cid:/opt/min-kernel/kernel-release.txt" "$out/kernel-release.txt"
kernel_release="$(tr -d '\r\n' < "$out/kernel-release.txt")"
case "$kernel_release" in
  5.10.*) ;;
  *) echo "unexpected built kernel release: $kernel_release" >&2; exit 1 ;;
esac
docker export "$cid" -o "$out/focal-rootfs.tar"

root_disk="$out/focal-root.raw"
xfs_disk="$out/xfs.raw"
truncate -s 6G "$root_disk"
sudo mkfs.ext4 -q -F "$root_disk"
mkdir -p "$out/root-mnt"
sudo mount -o loop "$root_disk" "$out/root-mnt"
sudo tar --numeric-owner -xpf "$out/focal-rootfs.tar" -C "$out/root-mnt"
sudo chmod 0755 "$out/root-mnt/usr/local/sbin/c-logger-min-kernel-init"
sudo umount "$out/root-mnt"

truncate -s 1G "$xfs_disk"
sudo mkfs.xfs -q -f "$xfs_disk"

{
  echo "candidate_kernel=Linux 5.10"
  echo "guest_kernel_release=$kernel_release"
  echo "userland=Ubuntu 20.04"
  echo "glibc=2.31"
  echo "source_commit=$(git -C "$root" rev-parse HEAD)"
  echo "kernel_compiler:"
  sed 's/^/  /' "$out/kernel-compiler.txt"
  sha256sum "$out/guest-vmlinuz"
} | tee "$out/platform-evidence.txt"

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

grep -Fq 'MIN_KERNEL_EVIDENCE random_ready=1' "$out/serial.log"
grep -Fq 'MIN_KERNEL_RUNTIME_PROBE_OK' "$out/serial.log"
grep -Fq 'MIN_KERNEL_OK kernel=5.10.' "$out/serial.log"

# With -no-reboot, QEMU may return a non-zero status after a guest poweroff
# even though the guest completed validation. The serial success markers above
# are the authoritative protocol result; qemu_rc remains diagnostic only.
if [ "$qemu_rc" -ne 0 ]; then
  echo "QEMU exited with rc=$qemu_rc after MIN_KERNEL_OK" >&2
fi

echo "minimum-kernel baseline validation passed"
