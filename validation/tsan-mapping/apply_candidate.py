#!/usr/bin/env python3
"""仅对已固定 LLVM 源码应用实验性 mmap 寿命修正，不修改 Logger。"""
import argparse
from pathlib import Path
import subprocess

REVISION = "3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff"
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("source", type=Path)
a = p.parse_args()
revision = subprocess.check_output(["git", "-C", str(a.source), "rev-parse", "HEAD"], text=True).strip()
if revision != REVISION:
    raise SystemExit("LLVM 源码提交不匹配: " + revision)
path = a.source / "compiler-rt/lib/tsan/rtl/tsan_interceptors_posix.cpp"
old = "    if (fd > 0) FdAccess(thr, pc, fd);\n    MemoryRangeImitateWriteOrResetRange(thr, pc, (uptr)res, sz);"
new = """    if (fd > 0) FdAccess(thr, pc, fd);
    // 成功的新映射开启新的地址寿命。内核可因 MADV_DONTFORK 移除旧页，
    // 没有经过 munmap 拦截器；不能沿用父方的同步对象登记。
    // 复用现有解除映射元数据清理，再保留原有写入/数据竞争检查。
    UnmapShadow(thr, (uptr)res, sz);
    MemoryRangeImitateWriteOrResetRange(thr, pc, (uptr)res, sz);"""
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit("实验修正锚点缺失或不唯一")
path.write_text(text.replace(old, new))
