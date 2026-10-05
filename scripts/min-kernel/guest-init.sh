#!/bin/bash
set -euo pipefail

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

fail() {
  echo "MIN_KERNEL_FAIL: $*" >&2
  sync || true
  /bin/busybox poweroff -f || true
  exit 1
}

mountpoint -q /proc || mount -t proc proc /proc
mountpoint -q /sys || mount -t sysfs sysfs /sys
mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /mnt/xfs /tmp/min-kernel-ext4
mountpoint -q /dev/pts || mount -t devpts devpts /dev/pts

kernel="$(uname -r)"
glibc="$(getconf GNU_LIBC_VERSION)"
echo "MIN_KERNEL_EVIDENCE kernel=$kernel userland=$glibc"
case "$kernel" in
  5.10.*) ;;
  *) fail "expected Linux 5.10.x, got $kernel" ;;
esac
[ "$glibc" = "glibc 2.31" ] || fail "expected glibc 2.31, got $glibc"

python3 - <<'PY' || fail "kernel CRNG is not ready"
import os
try:
    os.getrandom(1, os.GRND_NONBLOCK)
except BlockingIOError:
    raise SystemExit(1)
print("MIN_KERNEL_EVIDENCE random_ready=1")
PY

mount -t xfs /dev/vdb /mnt/xfs
echo "MIN_KERNEL_EVIDENCE xfs=$(findmnt -n -o FSTYPE /mnt/xfs) ext4=$(findmnt -n -o FSTYPE /)"

export LD_LIBRARY_PATH=/opt/c-logger-prefix/lib
test -f /opt/c-logger-prefix/lib/liblogger.so.0 || fail "installed shared library missing"
/opt/min-kernel-runtime   /tmp/min-kernel-ext4/runtime.log   /mnt/xfs/runtime.log   /tmp/min-kernel-ext4   /mnt/xfs

echo "MIN_KERNEL_OK kernel=$kernel userland=$glibc ext4=1 xfs=1"
sync
umount /mnt/xfs
/bin/busybox poweroff -f
