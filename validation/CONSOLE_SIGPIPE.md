# Console SIGPIPE validation

2026-10-11，Linux x86_64，GCC 14.2.0；基于 `db204a3` 的未提交工作树验证。
本记录只覆盖本次 Console/SIGPIPE 修订，不能代替完整发布 CI 或部署平台验收。

## 变更与回归证据

- 提取 Logger stderr 已有线程局部 SIGPIPE guard 为私有 `logger_sigpipe.h`，供 Console
  与原始 stderr/writev 路径共用。没有新增 public symbol 或改动公开结构体。
- Console 仍使用原始 stdio 操作及 FILE 缓冲/错误状态；没有改用 raw fd 输出，也没有新增
  可增长的中间格式缓冲。调用线程的完整 mask、已有 pending SIGPIPE 和进程全局 handler
  保持原契约；其他线程可正常接收其 SIGPIPE。
- `saw_epipe` 与首个错误独立：格式化先返回 EILSEQ、随后 flush 宿主缓冲字节得到 EPIPE 时，
  返回值仍报告 EILSEQ，同时消费此次写入生成的新 SIGPIPE。
- SIGPIPE guard setup 失败仍走显式 mutex release。释放失败会 poison Console 的锁状态，
  同时本次返回保留 setup 首错；下一次调用在锁准入前拒绝。debug-source 路径同样覆盖。
- 把新 closed-pipe 测试链接修订前的生产 static 库时，首个
  `closed/print/buffered=0` 子进程真实终止于 SIGPIPE（wait status 13）。

## 生产路径测试

`console_sigpipe_production_test` 不使用 wrapper 或私有入口，真实链接生产 static 库或 DSO。
共享版 ELF `NEEDED` 为 `liblogger.so.2`，实际解析到当前构建的生产 DSO。

12 个 Xmake 场景覆盖：

- 七个公开输出 API，各自的无缓冲/全缓冲 FILE；
- 默认 SIGPIPE、预先 blocked、预先 pending、自定义 handler、SIG_IGN；
- 健康输出逐字节一致，stdout/stderr 标签与换行语义及 `%m`/入口 errno 保持；
- 宿主先写入的 FILE 缓冲内容，以及空 Console 消息触发的宿主缓冲 flush；
- 真实 libc EILSEQ 后 fflush/EPIPE，返回首错并保留 ferror；
- 真实 fopencookie 回调期间，另一线程正常收到其 SIGPIPE；
- 有取消请求时，调用线程的完整 mask 在宿主 cancellation cleanup 前恢复；
- 32767 字节健康输出不被新实现截断。

另有 `console_sigpipe_guard_regression` 的 8 个 white-box 场景，覆盖 print/debug-source、
mask/pending setup 失败与正常恢复/同时 unlock 失败。wrapper 仅链接未安装的测试 archive。
这些场景断言失败 setup 无输出、一次获取对应一次释放、mask 恢复、首错保留及后续 poison 拒绝。

## 最终 focused 矩阵结果

- Release static：107/107 通过。
- Release shared：110/110 通过，包括额外 3 个 global shared 压力场景。
- Debug static ASan+UBSan：107/107 通过。`ASAN_OPTIONS=detect_leaks=0`；环境 ptrace
  限制下未执行 LeakSanitizer，因此本记录不声称完成泄漏验证。
- 三种最终生产产物均通过 `scripts/check_production_artifact.py`。
- 最终 shared 产物通过 `scripts/check_release_abi.py --manifest abi/logger-2.0.symbols`。
- `scripts/check_lock_error_policy.py` 和 `git diff --check` 通过。

这是 focused 矩阵，不是完整 suite；未在本轮重跑 socket/完整文件故障、安装包或发布矩阵。

## 复现

先导出 `LOGGER_PROJECT_VERSION=$(cat VERSION)`，分别配置：

```sh
xmake f -c -m release -o build-console-static --build_shared=n \
  --build_tests=y --build_private_tests=n --build_regression_tests=y

xmake f -c -m release -o build-console-shared --build_shared=y \
  --build_tests=y --build_private_tests=n --build_regression_tests=y

ASAN_OPTIONS=detect_leaks=0 xmake f -c -m debug -o build-console-asan \
  --build_shared=n --build_tests=y --build_private_tests=n \
  --build_regression_tests=y \
  --policies=build.sanitizer.address,build.sanitizer.undefined
```

每次配置后执行以下筛选；ASan 配置的 test 命令也需要 `ASAN_OPTIONS=detect_leaks=0`：

```sh
xmake test -j1 \
  'console_sigpipe_production_test/*' \
  'console_sigpipe_guard_regression/*' \
  'stderr_sigpipe_production_test/*' \
  'console_host_regression/*' \
  'console_test/*' \
  'explicit_scope_regression/console_*' \
  'explicit_scope_regression/explicit_contract_*' \
  'stderr_sigpipe_regression/*' \
  'global_*/*'
```

现有 Logger bootstrap、sync/async stderr、global lifecycle/cancel/stress wrapper 及 Console
取消/重入回归均在以上集合内；SIGPIPE helper 的提取没有绕开这些既有链接边界。
