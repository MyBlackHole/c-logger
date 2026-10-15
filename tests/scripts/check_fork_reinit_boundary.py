#!/usr/bin/env python3
"""有限源码门禁：有效快照才可重建；运行时正确性由 fork-runtime 分组验证。"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
process = (root / "src/logger_process.c").read_text()
fd = (root / "src/logger_fork_fd.c").read_text()
child = re.search(r"void logger_fork_fd_child\(void\).*?\n}\n", fd, re.S).group()
checks = {
    "先关闭普通准入再消费快照": bool(re.search(r"logger_process_invalidate_child\(void\).*?atomic_store_explicit\(&forked_child,\s*1,.*?logger_fork_fd_child\(\)", process, re.S)),
    "拒绝缺失可信快照的重建": "!logger_fork_fd_child_valid()" in process and "return -ECHILD;" in process,
    "普通入口拒绝未初始化子方": bool(re.search(r"int logger_process_ensure\(void\).*?if \(logger_process_is_child\(\)\)\s*return -ECHILD;", process, re.S)),
    "回调不操作继承同步对象": not re.search(r"\b(?:free|munmap|closedir|flock|pthread_join|pthread_mutex_lock|pthread_mutex_unlock|pthread_create)\s*\(", child),
    "新域构造后才发布": process.index("logger_runtime_create(&runtime)") < process.index("logger_runtime_publish(runtime)") and "logger_runtime_invalidate_child();" in process,
}
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL") + ": " + name)
raise SystemExit(0 if all(checks.values()) else 1)
