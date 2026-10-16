# fork 检测器失败诊断与夹具修复（PR #223，第九阶段）

## 范围与证据身份

本阶段基于提交 `74caa6e2203b467d13259de7768513a8ff7e6428`，
源码树 `f5f29f2a12643af8ce2684f537b7a66ee904daaf`。
修改进程 fork 回归夹具，新增独立检测器诊断探针；不修改生产源码、
API、检测器开关、原有跳过列表、超时阈值或 CI 合并条件。

下列本地项目用例通过直接调用 GCC 构建，用于复现和控制变量诊断。
**这不是完整 Xmake 验收，也不代表当前 PR 可以合并。**
正式结果必须绑定本文件所在提交的实际 CI 运行，不能沿用旧提交通过数。

## 一、核心 TSan 的失败与夹具修复

旧提交的 core-validation 运行 `38819757977`、任务 `116713182236`
唯一失败项为 `process_fork_regression/process_fork_console-held`。
TSan 在子进程报告辅助线程已退出但未 join，子进程退出码为 66。
本地 GCC 14.2.0 下也复现同一报告。

旧夹具在 Logger prepare 的格式锁入口释放 Console 线程，但直到父进程
fork 返回后才 join。若辅助线程先退出，子进程就继承了未回收线程记录。
这是测试夹具的生命周期窗口，不是 Logger 工作线程泄漏的证据。

修复分为两部分：

1. 利用既有 vfprintf 和互斥锁包装器，将暂停点移至正文格式化之后、
   `fflush` 的格式锁已取得之时。主线程确实到达 prepare 的格式锁入口
   后才放行，保留真实格式区间与 prepare 的交错。
2. 在 Logger 注册前登记夹具 prepare 回调。prepare 逆序执行，故该回调
   在 Logger prepare 之后、实际 fork 之前 join 辅助线程，并验证完成标记。
   父进程返回后再验证 join 已发生，不再二次 join。

不能只在原有 vfprintf 暂停点之后增加 join：Console 随后还会为 fflush
再次取格式锁，而 prepare 已持有该锁，会形成 join 与格式锁的死锁。
本阶段抓到该候选方案的两方线程栈后将其撤回；最终方案不是它。

也没有通过保持辅助线程存活来触发 TSan 多线程 fork 后的忽略模式。
最终夹具 200 次重复验证的详细日志均显示 `parent had 1 threads`，
且无该线程泄漏报告；子进程 ECHILD 及禁止锁、分配、I/O 的断言保留。

| 本地 GCC 14.2.0 诊断集合 | 实际通过 | 跳过 | 失败 |
|---|---:|---:|---:|
| 普通构建，进程 fork 的全部 17 个场景 | 17 | 0 | 0 |
| ASan/UBSan，同一组场景 | 17 | 0 | 0 |
| TSan，同一组场景 | 17 | 0 | 0 |
| TSan，最终 console-held 夹具重复验证 | 200 | 0 | 0 |

## 二、ASan 的运行库问题：固定目标文件的对照

上阶段已抓到新工作线程阻塞在 ASan 分配器同步中，尚未进入
`logger_worker_main()`。本阶段进一步核对了上游实现：

- LLVM PR #75290（2023-12-13 合并）为 ASan 增加 pthread_atfork 处理，
  在 fork 前协调线程表、分配器和堆栈记录锁，并在父子两侧恢复。
- GCC 14.2.0 的 `asan_posix.cpp` 没有该注册实现；GCC 15.1.0 已包含。
- 本地 Swift LLVM 工具链虽显示 Clang 17，其实际运行库含
  `__asan::InstallAtForkHandler`，不能只凭编译器主版本判断修复是否存在。

独立探针不链接 Logger。8 个父线程持续分配、释放内存；主线程执行
96 次 fork，子进程创建并回收一个空线程，每轮用 alarm 限定时间。
探针只由 GCC 编译一次，三种链接均使用**同一个插桩目标文件**：

| 运行库与对照条件 | 完成 | 子进程超时 | 其他失败 | 原始退出码 |
|---|---:|---:|---:|---:|
| GCC 14.2.0 运行库 | 93 | 3 | 0 | 1 |
| 本地 LLVM 运行库，保留 atfork 注册 | 96 | 0 | 0 | 0 |
| 同一个 LLVM 运行库，仅移除 atfork 注册的负向对照 | 92 | 4 | 0 | 1 |

注册包装器未插桩；探针本身不注册 atfork。后两组分别记录到
`registrations=1 dropped=0` 与 `registrations=1 dropped=1`。
因此不仅是“另一个编译器通过”，还包含同一运行库有无回调的对照。
负向对照只在独立诊断可执行文件中删除回调，绝不链接进 Logger 或正常门禁。

该实验支持缺少运行库 fork 协调是已观察分配器挂起的一条实际成因；
它不证明所有平台、所有版本、所有远端超时均同源。此前较短的 12 次探针
三组均通过，说明这种挂起不是每次必现，不得用一次通过判定消失。

同时进行了生产配置对象的链接对照：故障注入和 LOCKDEP 都关闭，
所有 Logger 对象及公开重初始化测试对象均由同一个 GCC 14.2.0 编译。
只改变链接的检测器运行库后，两组的 6 个公开重初始化场景本轮均通过。
这是一轮本地诊断，不推翻旧 CI 的超时，也不等于生产完整验收。

## 三、独立诊断的复现入口

```sh
CC=gcc LLVM_LINKER=clang \
  bash tools/diagnostics/compare_asan_fork_runtime.sh /tmp/asan-fork-evidence
```

该入口不编译 Logger，不替代项目的 Xmake 构建权威。
记录编译器、链接工具链、内核、glibc、源码、目标文件及二进制摘要、
每组完整输出和独立退出码。任一探针非零，脚本即返回非零；
负向对照不被计算为普通通过用例。没有重试直到成功。

先核对注册数和各组的原始结果，再解释对照。若某组未复现超时，
如实记录未复现；不能增加跳过或关闭检查来制造期望结果。
跨实现链接仅用于这次 ABI 可链接的诊断对象，不推荐为生产构建方式。

最终入库入口另行运行一轮 96 次：GCC 运行库完成 94 次、超时 2 次；
LLVM 保留回调完成 96 次；LLVM 删除回调完成 95 次、超时 1 次。
三组原始退出码为 1/0/1，入口整体退出码为 1，没有把负向结果转换成成功。
该轮与前述对照分别保留源码路径、排版和二进制摘要，不混用收据。

## 四、尚未完成的条件

当前核心 ASan、诊断 ASan 和 Xmake 一致性工作流没有在本阶段被放宽。
下一步需要在远端原工具链复核夹具修复，并依据上游修复确认检测器
运行库基线或受控回移方案，再完成精确提交的完整 Xmake 集合。
不能把本地 17 个场景或 96 次独立探针写成 637 项完整集合通过。

PR #223 保持未合并；本文件不关闭 #218/#221，也不构成发布许可。

## 上游与历史运行

- https://github.com/llvm/llvm-project/pull/75290
- https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-14.2.0/libsanitizer/asan/asan_posix.cpp
- https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-15.1.0/libsanitizer/asan/asan_posix.cpp
- https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-13.3.0/libsanitizer/tsan/tsan_rtl.cpp
- https://github.com/MyBlackHole/c-logger/actions/runs/38819757977/job/116713182236
- https://github.com/MyBlackHole/c-logger/actions/runs/38819759146


## 第十阶段：运行库修复成为可执行工具链前提

完整 ASan/UBSan 集合改用匹配的 Clang 18 编译器与 compiler-rt；普通 GCC、
TSan、最低 GCC9/Clang10、glibc 和发布包基线不变。不在 Logger 里操作检测器私有锁。

这不是根据“换一个编译器会绿”选择工具。上游
[LLVM #75290](https://github.com/llvm/llvm-project/pull/75290) 的
`e065841` 系列修复增加 fork 前后的分配器、线程表、堆栈记录协调；GCC14.2
尚未携带该实现。第九阶段的固定 GCC 对象文件、同一 LLVM 运行库有无回调的
对照保留。Clang18 系列包含该修复；每次 CI 仍记录实际包版本与最终二进制摘要。

三个既有 ASan 工作流保留原 job 名称和全部项目用例，先使用同一套编译/链接
工具运行独立 Xmake 工具验收，再配置项目完整集合及专项脚本。没有新增 ASan
跳过、延长原超时、设置 report/exitcode 抑制或修改生产默认工具链。

独立工具工程 `tools/diagnostics/asan-runtime/xmake.lua` 不编译或安装 Logger。
它检查最终静态链接产物保留上游 `InstallAtForkHandler` 实现符号，真实注册
数量为1、未删回调；随后在八个持续分配线程下执行96次 fork，子方创建/join
线程，逐轮退出状态必须全部为0。一次压力通过不是完整竞态证明，符号存在
也不能代替动态结果。

同时健康程序必须正常完成，刻意的堆越界必须输出 ASan heap-buffer-overflow，
刻意的有符号溢出必须输出 UBSan signed integer overflow，错误退出码严格为1。
只有该工具自检的 UBSan 反例关闭错误恢复，以确定它确实终止；项目测试策略
不变。原始输出、退出码、超时和哈希进入 `asan-runtime-evidence/result.json`。
若未检测到刻意错误、回调缺失、漏跑/重复任何一轮或使用非默认检测器选项，
资格检查失败，不降级为跳过。Python判定器正反例另行验收。

本地可显式选择已经核对修复的工具，不能默认把 `clang` 命令当作Clang18：

```sh
xmake f -P tools/diagnostics/asan-runtime -m debug --toolchain=clang \
  --cc=clang-18 --cxx=clang++-18 --ld=clang++-18 --sh=clang++-18
xmake test -P tools/diagnostics/asan-runtime -v -j1
```

使用 `-P` 是必要的，避免 Xmake 将嵌套工具误识别为仓库根项目。此工具的
通过不代替根项目637项完整静态集合、最低平台、共享库或生产发布验收。
远端最终状态以准确PR head 的实际记录为准，不能从本地结果推定。
