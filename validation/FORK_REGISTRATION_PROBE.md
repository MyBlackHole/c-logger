# #220：fork 注册探针隔离与编译器矩阵

本文件记录测试探针的有限修复，不是 fork 子进程运行域的支持声明。
生产注册状态机和公开 ABI 未改；本次基线为已合并 #219 的
`c65a4107853870d522bd1a2aecd9f5fd6e09cb0c`。

## 缺陷与定位

旧 `test_process_fork.c` 包装所有 `pthread_atfork` 调用，既计入数量也
施加失败/暂停控制，因此把检测器或宿主自己的注册误认为 Logger 行为。
本地以同一 #219 生产归档和原测试源编译 Clang 17 ASan/UBSan，
`explicit` 与 `register-global` 均在精确注册次数断言失败。

独立临时诊断驱动记录首次非 Logger 调用的返回地址及三个处理函数地址。
使用非 PIE 驱动和 `addr2line -Cf` 定位结果为：

```text
调用者：__asan::AsanInitInternal() [clone .cold.1]
        compiler-rt/lib/asan/asan_rtl.cpp:498
prepare：__asan::BeforeFork()
         compiler-rt/lib/asan/asan_posix.cpp:151
parent：__asan::InstallAtForkHandler()::$_0::__invoke()
        compiler-rt/lib/asan/asan_posix.cpp:178
child：__asan::InstallAtForkHandler()::$_1::__invoke()
       compiler-rt/lib/asan/asan_posix.cpp:179
```

这些行号只对应本地 Clang 17 自带运行时，不作为其他版本的固定身份。
诊断驱动未进入生产源码或永久测试接口。原始失败和符号化输出单独保留。

## 修复与正反对照

以真实 `logger_process_invalidate_child` 函数指针识别 Logger 注册。
其他回调直接转交 libc；分流发生在数量、失败和暂停控制之前。
没有将断言改为 `>=1`，没有排除 Clang 或关闭 ASan。

每个原有场景先在故障和暂停开关均已设置时注册一个非 Logger 回调，
要求它成功，且 Logger 计数、故障开关、暂停开关和同步信号均未被消费。
`ATFORK_FOREIGN_BEFORE_MAIN` 记录宿主进入 main 前实际发生的外部注册数；
数量依工具链而异，不预设必须为零或一。

探针自检的三个独立进程使用同一个 Xmake 构建的真实测试程序：

| 场景 | 实际操作 | 必须结果 |
|---|---|---|
| healthy | 正常注册与原 explicit 场景 | 退出 0，Logger 注册次数恰为 1 |
| duplicate | 真实再调用一次 Logger 的 atfork 注册入口 | 次数恰为 2，精确次数断言失败，退出 1 |
| unobserved | 测试驱动故意漏记 Logger 注册 | 次数恰为 0，同一断言失败，退出 1 |

两个 `LOGGER_TEST_*` 开关只存在于该测试可执行文件，不进入生产库，
不会修改注册状态机。它们检验探针的拒绝能力，不是新日志功能。
独立 Python 验收器拒绝任意非零退出、信号、超时、77 跳过、错误计数、
缺失/重复记录以及不相关的 Sanitizer 诊断；其自身有十项正反单测。

## 执行与证据

`fork-registration-validation` 对 GCC/Clang 与普通/ASan+UBSan 四种组合运行：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -y -m debug --toolchain=clang --build_shared=n \
  --build_tests=n --build_private_tests=n --build_regression_tests=y \
  --policies=build.sanitizer.address,build.sanitizer.undefined
xmake test -v -j1 'process_fork_regression/*'
```

随后对该次构建的测试二进制运行探针自检，保存源码提交/树、编译器、
Xmake 版本、17 项原场景日志、正反例日志、二进制和日志摘要。
所有场景使用独立临时目录。原有 Xmake 场景不删除，原工作流不放宽。
新的远端结果必须属于本 PR 的精确 head，不能复制 #219 的通过记录。

这项修复不解决多线程 fork 后创建新线程的 TSan 限制，也不使本会话
未合并的运行域候选自动获得发布资格；#218 的运行域和完整新 API
继续独立验收。检测器自身注册的额外次数与真实 Logger 重复注册不可混淆。
