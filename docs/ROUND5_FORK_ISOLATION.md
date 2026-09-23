> Historical revision record, not the current crypto backend contract.
> The external backend has since been removed; see [BUILTIN_CRYPTO.md](BUILTIN_CRYPTO.md).
> Original test evidence and failure reports are preserved, not retroactively changed to pass.

# Round 5：Fork 契约收紧与生产故障钩子隔离

输入：Round 4 `prod_c_logger_audit_parser_recovery_fix.tar.gz`。
本轮修复普通 Logger/Console 的继承状态拒绝路径，与 Audit 统一进程身份；不恢复多线程 fork 后的运行时。
配置结构、公开函数签名、审计 hash 字节域与 checkpoint v2 布局不变，但旧实验版的 child reset/reinit **行为不再支持**。

## 1. 支持范围

| 场景 | 契约 |
|---|---|
| 应用真正单线程，且尚未使用本库运行时，先 fork | 父子可分别首次初始化；目的文件与单 writer 协调仍由调用者负责 |
| 本库已被使用或应用多线程，fork 后 exec | 推荐；exec 前准备工作在父进程完成，child 不调用 logger/console/audit |
| 继承本库状态，child 未 exec，调用 init/create | `NULL + ECHILD` 或 `-1 + ECHILD` |
| 继承显式实例、全局 facade、Console、Audit | 在锁、实例解引用、格式化与 I/O 之前防御性拒绝 |
| shutdown 后再 fork，希望 child 重建 | 进程身份仍保留；必须 exec，不以 shutdown 证明 libc/第三方状态已清理 |
| child 调用 after_fork_child | 只标记无效；不重置锁、不遍历/关闭 fd、不 free、不开放重新初始化 |
| vfork、raw clone、信号处理日志、一般 fork+continue | 不支持；SYS_fork 测试只检查当前 Linux 上的 PID 防线，不构成支持承诺 |

异步 logger 自己会创建 worker，不能把“业务只创建了 main 线程”误认为 fork 时是单线程。
即使本库成功拦截误用，也不表示任意公有 API 都是 async-signal-safe：日志宏的参数会先在调用者一侧求值，其他运行时/用户 atfork handlers 的问题也不归本库恢复。

## 2. 共享的最小 guard

`logger_process.c/.h` 保存不可变 owner PID 和 lock-free int 失效标志。
编译时要求 `ATOMIC_INT_LOCK_FREE == 2`；handler 仅进行一次原子标记。
父进程在注册 atfork 前先发布身份；入口在 pthread_once 之前检查 PID，因此覆盖：

- 用户更早注册的 child handler 在本库 handler 之前调用 API；
- 另一线程正在注册 atfork 时发生 fork；
- 防御测试中绕过 libc atfork handler 的原始 fork 系统调用。

pthread_atfork 的分配/注册错误向外传播，不能创建“看起来成功、实际上没有安装防线”的运行时；本进程注册错误保持 sticky，不重复尝试后宣称成功。
继承 fd 不在 handler 中清理，正常 CLOEXEC/进程退出负责关闭；不承诺 exec-less child 服务无泄漏可持续运行。

## 3. 公共错误语义与兼容性

- 构造返回 NULL/ECHILD；一般 int 控制/API 返回 -1/ECHILD。
- `logger_log_sync_status()` 保留内部式 `-ECHILD`，没有偷改返回约定。
- void 写入、控制、destroy/shutdown 无操作，并设置 errno=ECHILD。
- metrics 返回零；`logger_get_state()` 返回 STOPPED/ECHILD，这不是释放内存的证明。
- context getter 返回空视图；TTY predicate 返回 false/ECHILD。
- `audit_get_status()` 仍可返回 FORKED/error=ECHILD；child 的 audit failure policy 为 DENY。
- 纯 redaction/backend-name 等无运行时资源 helper 不是 child 重新初始化操作。

`logger_prepare_fork()` 保留符号，变成可选的兼容 marker，不再跨 fork 持有 global rwlock，也不 flush、冻结 queue 或暂停 worker。嵌套 marker 返回 EALREADY。
`logger_after_fork_parent()` 只清除 marker，fork 失败时也可调用；parent 正常 logger 不受 child copy 中的失效标记影响。
需要持久化的父进程应显式检查 flush status；不能把 prepare 当提交屏障。

可运行的推荐用法见 `tests/test_fork.c`：父进程准备 exe/argv，child 只 execve 或 write 固定错误文本后 _exit；全量库调用发生在新映像中。

## 4. 测试与生产是两个不同产物

```
logger                -> liblogger.a                 -> 无故障/退出环境控制
logger_test_support   -> liblogger_test_support.a    -> 仅测试注入
```

生产 `logger` 永远使用 PRIVATE `LOGGER_ENABLE_FAULT_INJECTION=0`，不编译 logger_fault.c。
私有 header 的生产分支直接消除 hook 调用及其参数字符串，Debug/-O0 同样成立，不依赖优化器消除。
`LOGGER_FAULT_*`、`LOGGER_CRASH_POINT` 不能开启生产钩子。
`LOGGER_SYSLOG_PATH` 环境覆盖也限定在测试支持库；生产仍使用 /dev/log，没增加新的公开路径配置。

启用 BUILD_TESTING 时，原有 fault/crash/syslog-path 等定向用例链接测试库，其他测试仍尽量链接真实生产库。
CMake 检查依赖的 LOGGER_VARIANT 属性，拒绝一个 consumer 同时链接两个 target。
这不是对任意手工链接命令的强制安全边界：交付应用不得自行链接私有测试 archive。

```sh
cmake -S . -B build-prod -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-prod
python3 scripts/check_production_artifact.py build-prod/liblogger.a

cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Release
cmake --build build-test --target check -j4
ctest --test-dir build-test -L fork-isolation --repeat until-fail:20 --output-on-failure -j4
```

这仍是开发阶段 static 构建；没有借本轮宣称 shared/install/SONAME 打包工作已完成。

## 5. 检测与复现方式

新增48个CTest场景：22个进程/handler/注册失败场景，13对同源的生产/测试钩子场景。
与改写后的 fork_test 共49项 fork-isolation。
其他线程实际持有 emit/progress/console/global reader 锁时进行 fork；测试包装器禁止拒绝分支触碰 pthread锁/once、分配器、格式化、I/O、线程创建等选定入口。
不仅验证退出而不死锁，还断言不会被错误开放重新初始化。

真实旧 fork_test 在3次ASan复现中2次超时，父进程do_wait，child两线程__futex_wait。没有完整符号化线程栈，不能武断定位具体锁，更不以“CPU throttling”解释。
新测试改成fork+exec是契约修正，不是保留不支持的child reinit功能后忽略超时。
旧库反向执行33个负向探针全部检出错误；最终各种构建/专项结果详见 validation/ROUND5_RESULTS.md。

## 6. 未完成事项

普通Global init/shutdown的generation与重复init候选创建副作用、本库非fork生命周期、同路径文件多owner、归档时间碰撞、阻塞syslog、动态source指针、配置ABI、老内核/glibc验证均未由本轮解决。
本轮增加guard检查和测试支持库构建，没有测量或宣称性能收益。
完整OpenSSL TSan仍有系统libcrypto路径的摘要并发race报告；旧库和无logger的独立EVP程序也复现同类报告，尚未区分真实依赖问题与缺少依赖插桩导致的误报，不属于已通过发布门禁。

参考：Linux man-pages fork(2)/pthread_atfork(3)；TSan官方手册的Non-instrumented code说明。具体原始链接和命令见本轮验证报告。
