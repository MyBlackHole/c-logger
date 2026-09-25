#!/usr/bin/env python3
"""Boot a minimal Linux guest twice, killing QEMU after acknowledged audit writes."""

import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def guest_init(phase):
    return f"""#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t ext4 /dev/vda /mnt || exec sh
cd /mnt || exit 1
/vm_powercut {phase}
result=$?
echo GUEST_EXIT_$result
sync
poweroff -f
"""


def boot(root, kernel, disk, binary, phase, timeout):
    initramfs = root / f"initramfs-{phase}.cpio.gz"
    stage = root / f"stage-{phase}"
    for directory in ("bin", "dev", "proc", "sys", "mnt"):
        (stage / directory).mkdir(parents=True)
    shutil.copy2(binary, stage / "vm_powercut")
    shutil.copy2("/bin/busybox", stage / "bin/busybox")
    for applet in ("sh", "mount", "poweroff", "sync"):
        (stage / "bin" / applet).symlink_to("busybox")
    init = stage / "init"
    init.write_text(guest_init(phase))
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
        marker = b"POWERCUT_READY" if phase == "write" else b"POWERCUT_RECOVERY_PASS"
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
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    disk = args.output / "audit-disk.raw"
    with disk.open("wb") as file:
        file.truncate(256 * 1024 * 1024)
    subprocess.run(["mkfs.ext4", "-F", "-q", str(disk)], check=True)
    for phase in ("write", "recover"):
        boot(args.output, args.kernel.resolve(), disk.resolve(), args.binary.resolve(),
             phase, 180)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
