#!/usr/bin/env python3
"""Boot a minimal Linux guest twice, killing QEMU after acknowledged audit writes."""

import argparse
import gzip
import lzma
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


def _copy_module(stage, source):
    relative = source.relative_to("/")
    destination = stage / relative
    destination.parent.mkdir(parents=True, exist_ok=True)

    if source.suffix == ".zst":
        destination = destination.with_suffix("")
        with destination.open("wb") as out:
            subprocess.run(["zstd", "-q", "-d", "-c", str(source)],
                           stdout=out, check=True)
    elif source.suffix == ".xz":
        destination = destination.with_suffix("")
        with lzma.open(source, "rb") as src, destination.open("wb") as out:
            shutil.copyfileobj(src, out)
    elif source.suffix == ".gz":
        destination = destination.with_suffix("")
        with gzip.open(source, "rb") as src, destination.open("wb") as out:
            shutil.copyfileobj(src, out)
    else:
        shutil.copy2(source, destination)


def stage_filesystem_support(stage, kernel_release, filesystem):
    if filesystem != "xfs":
        return False

    result = subprocess.run(
        ["modprobe", "--show-depends", "-S", kernel_release, "xfs"],
        check=True, text=True, stdout=subprocess.PIPE,
    )
    module_paths = []
    builtin = False
    for raw in result.stdout.splitlines():
        line = raw.strip()
        if line.startswith("builtin "):
            builtin = True
        elif line.startswith("insmod "):
            source = Path(line.split(None, 1)[1])
            if not source.is_file():
                raise RuntimeError(f"kernel module not found: {source}")
            module_paths.append(source)

    if not module_paths:
        if builtin:
            return False
        raise RuntimeError(f"xfs module unavailable for kernel {kernel_release}")

    for source in module_paths:
        _copy_module(stage, source)
    subprocess.run(["depmod", "-b", str(stage), kernel_release], check=True)
    return True


def guest_init(phase, point, filesystem, load_xfs_module):
    load_module = "modprobe xfs || exit 90\n" if load_xfs_module else ""
    return f"""#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo VM_KERNEL_$(/bin/busybox uname -r)
{load_module}mount -t {filesystem} /dev/vda /mnt || exec sh
echo VM_FILESYSTEM_{filesystem}
cd /mnt || exit 1
/vm_powercut {phase} {point}
result=$?
echo GUEST_EXIT_$result
sync
poweroff -f
"""


def boot(root, kernel, kernel_release, disk, binary, filesystem, phase, point, timeout):
    initramfs = root / f"initramfs-{phase}.cpio.gz"
    stage = root / f"stage-{phase}"
    for directory in ("bin", "dev", "proc", "sys", "mnt"):
        (stage / directory).mkdir(parents=True)
    shutil.copy2(binary, stage / "vm_powercut")
    shutil.copy2("/bin/busybox", stage / "bin/busybox")
    for applet in ("sh", "mount", "modprobe", "poweroff", "sync"):
        (stage / "bin" / applet).symlink_to("busybox")

    load_xfs_module = stage_filesystem_support(stage, kernel_release, filesystem)
    init = stage / "init"
    init.write_text(guest_init(phase, point, filesystem, load_xfs_module))
    init.chmod(0o755)
    with initramfs.open("wb") as out:
        cpio = subprocess.Popen(
            "find . -print0 | cpio --null -o -H newc --quiet | gzip -1",
            cwd=stage, shell=True, executable="/bin/bash", stdout=out,
        )
        if cpio.wait() != 0:
            raise RuntimeError("initramfs build failed")
    log = root / f"serial-{phase}.log"
    cmd = ["qemu-system-x86_64", "-accel", "tcg", "-m", "512M", "-smp", "1",
           "-nodefaults", "-nographic", "-serial", "stdio", "-no-reboot",
           "-kernel", str(kernel), "-initrd", str(initramfs),
           "-append", "console=ttyS0 quiet panic=1",
           "-drive", f"file={disk},format=raw,if=virtio,cache=none"]
    with log.open("wb", buffering=0) as output:
        proc = subprocess.Popen(cmd, stdout=output, stderr=subprocess.STDOUT,
                                start_new_session=True)
        deadline = time.monotonic() + timeout
        marker = (f"POWERCUT_POINT_{point}".encode() if phase == "write"
                  else b"POWERCUT_RECOVERY_PASS")
        seen = False
        try:
            while time.monotonic() < deadline:
                if marker in log.read_bytes():
                    seen = True
                    if phase == "write":
                        os.killpg(proc.pid, signal.SIGKILL)
                    break
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
            if not seen:
                raise RuntimeError(f"{phase}: marker missing; inspect {log}")
            proc.wait(timeout=10)
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
    print(f"{phase}: marker observed; serial log: {log}", flush=True)
    if phase == "write" and proc.returncode != -signal.SIGKILL:
        raise RuntimeError(f"QEMU did not die by SIGKILL: {proc.returncode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--kernel-release", required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--filesystem", choices=("ext4", "xfs"), required=True)
    parser.add_argument("--point", choices=(
        "acknowledged", "before_audit_fsync", "after_audit_fsync",
        "before_state_rename", "after_state_rename", "after_checkpoint_commit",
        "file_after_archive_rename", "file_after_archive_dirsync",
        "file_after_active_open", "file_after_active_dirsync"), required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    disk = args.output / "audit-disk.raw"
    with disk.open("wb") as file:
        file.truncate(512 * 1024 * 1024)
    if args.filesystem == "ext4":
        mkfs = ["mkfs.ext4", "-F", "-q", str(disk)]
    else:
        mkfs = ["mkfs.xfs", "-f", "-q", str(disk)]
    subprocess.run(mkfs, check=True)
    for phase in ("write", "recover"):
        boot(args.output, args.kernel.resolve(), args.kernel_release,
             disk.resolve(), args.binary.resolve(), args.filesystem,
             phase, args.point, 180)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
