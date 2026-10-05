#!/bin/bash
set -euo pipefail

fail() {
  echo "MIN_KERNEL_FAIL: $*" >&2
  sync || true
  /bin/busybox poweroff -f || true
  exit 1
}

mountpoint -q /proc || mount -t proc proc /proc
mountpoint -q /sys || mount -t sysfs sysfs /sys
mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /mnt/xfs
mountpoint -q /dev/pts || mount -t devpts devpts /dev/pts

kernel="$(uname -r)"
glibc="$(getconf GNU_LIBC_VERSION)"
echo "MIN_KERNEL_EVIDENCE kernel=$kernel userland=$glibc"
case "$kernel" in
  3.17.*) ;;
  *) fail "expected Linux 3.17.x, got $kernel" ;;
esac
[ "$glibc" = "glibc 2.31" ] || fail "expected glibc 2.31, got $glibc"

mount -t xfs /dev/vdb /mnt/xfs
echo "MIN_KERNEL_EVIDENCE xfs=$(findmnt -n -o FSTYPE /mnt/xfs) ext4=$(findmnt -n -o FSTYPE /)"

for kind in shared static; do
  src="/opt/src-$kind"
  prefix="$src/min-kernel-prefix"
  cd "$src"
  export LOGGER_PROJECT_VERSION="$(cat VERSION)"
  echo "MIN_KERNEL_EVIDENCE core-tests kind=$kind"
  xmake test --root -g production-core -j1

  version="$(cat VERSION)"
  if [ "$kind" = shared ]; then
    artifact="$prefix/lib/liblogger.so.$version"
  else
    artifact="$prefix/lib/liblogger.a"
  fi
  test -f "$artifact" || fail "missing $kind artifact"
  python3 tests/packaging/check_install.py \
    --source "$src" \
    --build "$src/min-kernel-build" \
    --artifact "$artifact" \
    --kind "$kind" \
    --cc cc \
    --cxx c++ \
    --cmake cmake \
    --installed-prefix "$prefix"

  echo "MIN_KERNEL_EVIDENCE process-crash kind=$kind"
  xmake test --root -g process-crash -j1 -vD
done

shared="/opt/src-shared"
prefix="$shared/min-kernel-prefix"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
flags="$(pkg-config --cflags --libs logger)"
cc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  "$shared/tests/integration/test_min_kernel_runtime.c" \
  $flags -o /tmp/min-kernel-runtime
export LD_LIBRARY_PATH="$prefix/lib"
mkdir -p /tmp/min-kernel-ext4
/tmp/min-kernel-runtime \
  /tmp/min-kernel-ext4/runtime.log \
  /mnt/xfs/runtime.log \
  /tmp/min-kernel-ext4 \
  /mnt/xfs

echo "MIN_KERNEL_OK kernel=$kernel userland=$glibc ext4_internal_rotation=1 xfs_internal_rotation=0"
sync
umount /mnt/xfs
/bin/busybox poweroff -f