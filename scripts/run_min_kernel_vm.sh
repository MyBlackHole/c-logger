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
  if mountpoint -q "$out/root-mnt"; then
    sudo umount "$out/root-mnt"
  fi
  if [ -n "${cid:-}" ]; then
    docker rm -f "$cid" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

echo "building Ubuntu 20.04 / Linux 3.17 guest image"
docker build \
  --build-arg XMAKE_VERSION=3.1.1 \
  -t "$image" \
  -f "$root/scripts/min-kernel/Dockerfile.focal" \
  "$root"

for kind in shared static; do
  src="$out/src-$kind"
  mkdir -p "$src"
  git -C "$root" archive HEAD | tar -x -C "$src"
  if [ "$kind" = shared ]; then
    shared=y
  else
    shared=n
  fi
  echo "building $kind production profile on glibc 2.31"
  docker run --rm \
    -e LOGGER_PROJECT_VERSION="$version" \
    -v "$src:/opt/src-$kind" \
    -w "/opt/src-$kind" \
    "$image" \
    bash -lc "
      set -euo pipefail
      test \"\$(getconf GNU_LIBC_VERSION)\" = \"glibc 2.31\"
      xmake f --root -m release -o min-kernel-build \
        --build_shared=$shared \
        --build_tests=y \
        --build_private_tests=y
      xmake --root -j2
      xmake install --root -o min-kernel-prefix logger
    "
done

cid="$(docker create "$image")"
docker cp "$out/src-shared/." "$cid:/opt/src-shared"
docker cp "$out/src-static/." "$cid:/opt/src-static"
docker cp "$root/scripts/min-kernel/guest-init.sh" \
  "$cid:/usr/local/sbin/c-logger-min-kernel-init"

docker cp "$cid:/opt/min-kernel/bzImage" "$out/guest-vmlinuz"
docker cp "$cid:/opt/min-kernel/compiler.txt" "$out/kernel-compiler.txt"
docker cp "$cid:/opt/min-kernel/kernel-release.txt" "$out/kernel-release.txt"
kernel_release="$(tr -d '\r\n' < "$out/kernel-release.txt")"
case "$kernel_release" in
  3.17.*) ;;
  *) echo "unexpected built kernel release: $kernel_release" >&2; exit 1 ;;
esac
docker export "$cid" -o "$out/focal-rootfs.tar"

root_disk="$out/focal-root.raw"
xfs_disk="$out/xfs.raw"
truncate -s 8G "$root_disk"
sudo mkfs.ext4 -q -F -O ^orphan_file "$root_disk"
mkdir -p "$out/root-mnt"
sudo mount -o loop "$root_disk" "$out/root-mnt"
sudo tar --numeric-owner -xpf "$out/focal-rootfs.tar" -C "$out/root-mnt"
sudo chmod 0755 "$out/root-mnt/usr/local/sbin/c-logger-min-kernel-init"
sudo umount "$out/root-mnt"

truncate -s 1G "$xfs_disk"
sudo mkfs.xfs -q -f -m crc=0 "$xfs_disk"

{
  echo "candidate_kernel=Linux 3.17"
  echo "guest_kernel_release=$kernel_release"
  echo "userland=Ubuntu 20.04"
  echo "glibc=2.31"
  echo "xmake=3.1.1"
  echo "source_commit=$(git -C "$root" rev-parse HEAD)"
  echo "kernel_compiler:"
  sed 's/^/  /' "$out/kernel-compiler.txt"
  sha256sum "$out/guest-vmlinuz"
} | tee "$out/platform-evidence.txt"

set +e
timeout 20m qemu-system-x86_64 \
  -accel tcg \
  -m 2048 \
  -smp 2 \
  -nographic \
  -no-reboot \
  -kernel "$out/guest-vmlinuz" \
  -append "root=/dev/vda rw console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 loglevel=7 init=/usr/local/sbin/c-logger-min-kernel-init" \
  -d guest_errors,cpu_reset \
  -D "$out/qemu-debug.log" \
  -drive "file=$root_disk,format=raw,if=virtio" \
  -drive "file=$xfs_disk,format=raw,if=virtio" \
  2>&1 | tee "$out/serial.log"
qemu_rc=${PIPESTATUS[0]}
set -e

if [ "$qemu_rc" -ne 0 ]; then
  echo "QEMU exited with rc=$qemu_rc" >&2
  exit "$qemu_rc"
fi
grep -q '^MIN_KERNEL_OK kernel=3\.17\.' "$out/serial.log"
grep -q 'userland=glibc 2\.31' "$out/serial.log"

grep -q 'ext4_internal_rotation=1 xfs_internal_rotation=0' "$out/serial.log"
echo "minimum-kernel candidate validation passed"
