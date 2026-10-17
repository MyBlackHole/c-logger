# fork 子进程显式运行域重建（#218）

**本阶段已有生产实现，并非只有拒绝路径检查。** 在普通 libc fork 取得可靠资源
快照后，子进程调用 `logger_process_init()` 可以创建新的进程域，再创建、记录、
刷新和销毁自己的同步或异步实例。父进程继续使用原对象。本文保留底部第四阶段的
历史定位记录，不能把它当作当前提交的验收结果；后续 #223/#224 的准确源码验证
见各 PR 回执。新公开状态接口已迁移，剩余的宿主外部状态、性能及工具覆盖
边界仍须独立审核；当前仍是开发版，不是正式发布。

## 1. 使用和失效合同

```c
/* 父方首次使用与 fork 子方重建均显式初始化，不兼容旧创建/销毁签名。 */
if (logger_process_init() != 0)
        return -1;
logger_t *parent = NULL;
if (logger_create(&parent_config, &parent))
        return -1;
/* 检查parent后由宿主调用fork。 */
pid_t pid = fork();
if (pid == 0) {
        /* parent在这里失效：不解引用，不执行普通销毁。 */
        if (logger_process_init() != 0)
                _exit(125);
        logger_t *child = NULL;
        int create_rc = logger_create(&child_config, &child);
        if (create_rc)
                _exit(126);
        LOGGER_INFO(child, "child", "子进程的新记录");
        _exit(logger_destroy(&child) ? 127 : 0);
}
/* pid < 0时父方状态仍需按错误回执处理；成功则继续使用parent。 */
```

父方普通上下文在首次使用前显式初始化。子方初始化只能在fork返回后的普通应用
上下文调用，不能从任何atfork处理函数调用。普通准入状态见
[显式进程准入](PROCESS_ADMISSION.md)。
初始化不得在子方新业务线程或普通日志之前被跳过。第一次子方成功为0，重复
初始化为-EALREADY。进程就绪后创建第二个实例不重置其他实例。创建失败只回滚
本次候选。普通入口在子方尚未初始化时仍以ECHILD拒绝，不隐式重建。

继承实例指针在子方不再代表合法对象；初始化成功不使它们重新有效。不保留
句柄墓碑、不提供逐个旧实例销毁接口，不承诺通过读取已失效地址诊断误用。
宿主必须替换自己的引用。库负责关闭自有继承fd及隔离运行态，不把资源义务转嫁
给应用。宿主fd保持原状；父子同文件的独占冲突继续返回EBUSY，不自动加PID后缀。

## 2. 内存与进程域

`logger_runtime` 统一拥有资源锁、格式敏感区间锁、Global 控制/生命周期锁、
Console锁/配置、实例计数、fd位图元数据、映射所有权链。域为独立匿名映射，
发布前完成 `MADV_DONTFORK`。实例、队列两块存储和七个worker工作区分配也通过
独立映射取得，均先设置不继承、再登记/发布，不在宿主malloc堆页上设置该属性。

物理映射站点、逻辑分配站点分开登记：当前直接资源获取19处，调用方逻辑分配
10处（`validation/runtime_alloc_sites.tsv`），fd逻辑获取6处。它们是不同统计
口径，不能用19替换22就假装覆盖率提高。高fd扩容的mremap仍为adjacent。

每个映射头保存长度、所有权链连接及max_align_t对齐。取得、DONTFORK、登记和
最终munmap由资源锁串行化，fork不能越过取得但尚未登记的窗口。配置DONTFORK
失败则回滚；若回滚munmap失败，候选仍由域持有且域被标记错误，不向调用方发布。

新测试在子方分配任何新域之前用mincore确认旧域、实例、队列和工作区页均不存在；
仅检查新旧指针不同不足以证明隔离，因为虚拟地址可能被重用。

新域只在全部同步对象初始化完毕后发布。Global指针/票据、Console默认配置和
实例计数从新域开始；当前线程的请求上下文、重入标志与时间缓存明确失效。
旧域不执行mutex/cond销毁或worker join；旧映射由内核在fork边界排除。

独立映射增加系统调用和页粒度开销。当前没有合并所有小块，也没有声称创建成本、
VMA/RSS或生产吞吐保持不变；进一步合并布局须有测量和失败覆盖，不凭直觉优化。

## 3. 描述符、DIR 与准备区间

fd位图仍采用继承映射，以便child回调消费数值快照；普通进程最后一个自有fd
关闭后归还空位图。子方close完成后，位图在显式初始化中munmap，失败可重试，
但绝不再次close同一组整数。子方close错误记录为不可忽略的隔离失败。

资源锁已移入不继承的域。prepare只保存TLS中的位图地址/长度；child不能通过
旧域指针访问元数据。child关闭自己的引用而不执行flock LOCK_UN，父方锁仍有效。
宿主和其他handler不得私自close/dup2替换仍由Logger拥有的内部fd。

`fdopendir`成功会持有资源租约直至同线程`closedir`。整个DIR遍历完成后fork
才能继续，因此不会把正在使用的libc DIR锁/缓冲遗留给正常重建路径。失败时fd
仍归原拥有者。内部调用不得在该租约期间再次取得资源锁或格式锁；现行retention
路径按此约束实现。目录遍历可能受存储I/O阻塞，fork不提供墙钟超时保证。

准备锁顺序：格式敏感区间锁 → 资源锁。正常实例操作可以先持有自己的输出锁，
但prepare不取得该实例锁、Global生命周期锁，也不等待worker join。资源租约中
不得反向进入格式区间。本库prepare尚未结束时，同线程普通Logger重入被拒绝。

parent只释放本次实际取得的锁，保留errno；child只消费快照、清TLS标记及失效
当前根指针。有效快照要求父方READY、没有资源粘滞错误、不是库内重入/嵌套准备。
原始SYS_fork、_Fork/vfork、信号处理函数fork或注册窗口不构成可信重建入口。

## 4. 库内 libc 协调不等于全进程 libc 修复

`logger_libc` 包装本库的格式化、时间转换、标准流输出/刷新，在format_mu保护
范围执行；fork等待这些本库调用离开区间，再固定fd。子进程不继承这把锁，
也不会在旧实例锁上等待消失的线程。一个format-held测试在真实localtime_r包装
边界暂停，验证fork等待后继续；emit-held则验证子方可在父方旧输出锁仍被持有时
完成独立重建，两个测试证明的是不同条件。

**本库无法用这一把锁保护宿主其他线程直接调用的localtime/tzset/setlocale、
自定义printf扩展、替换分配器、外部FILE锁或其他第三方内部状态。** 多线程fork
子方直到成功exec前的POSIX通用合同仍限于异步信号安全操作；当前完整重建是
Linux/glibc实现级候选，不能根据本地成功推广为任意libc/任意宿主都安全。

还需要最低平台和实际初始化/输出依赖的逐链审查。没有要求用户结束所有业务
线程，也没有把安全字节通道冒充本功能；同样没有声称日志库能让子进程任意非日志
代码合法继续运行。共享格式锁可能影响多实例吞吐，须独立性能验收。标准流输出/刷新也可能在满
管道等外部条件下阻塞该协调区间；不能声称依赖尚未产生的子进程解堵的宿主I/O
拓扑具有活性保证，此类部署需单独处理。

参考：POSIX.1-2024 fork、Linux man-pages fork(2)/madvise(2)/flock(2)，以及
GNU C Library 的 Creating a Process。外部依据说明系统边界，不证明本候选已验收。

## 5. 错误、释放与卸载

域初始化中的mmap、madvise、四个mutex、一个rwlock失败都有回滚及子方重试场景。
初始化失败不发布半成品，也不清掉普通入口拒绝状态。同步对象销毁或munmap无法
确认成功时保留候选和粘滞错误，不丢失所有者后声称回滚完成。

实际munmap成功后若协调锁解锁报错，映射已经被消费。`logger_runtime_free` 的
回执描述实际消费结果，解锁错误另记入域；最终实例销毁返回可观察错误，但仍给出
真实final-free回执，避免Global将已失效指针保存成退役对象。`final-unlock` 用
真实释放加合成解锁EIO验证该边界。munmap失败则保留映射登记，另有反向场景。

卸载前宿主必须先停止调用并销毁所有实例。析构入口只归还空的健康进程域，不
自动关闭活动实例或猜测借用者已经退出。当前共享库20轮dlopen/dlclose回归检查
实际200行输出；它不证明违反宿主寿命合同的卸载安全。

## 6. 可执行验证及明确跳过

权威入口（已注册，本轮本地没有Xmake，尚未执行这些权威命令）：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m debug --build_shared=n --build_tests=y \
    --build_private_tests=y --build_regression_tests=y
xmake test -g fork-runtime -j1
xmake test -g fork-fd -j1
xmake test -j1
```

新的公开黑盒6场景：同步、异步、Global、先shutdown、24次重复派生、4层嵌套。
21个运行域场景覆盖实际旧页排除、新域失败、10处逻辑分配失败、错误保留、原始
系统调用拒绝、早注册handler、旧emit锁/时间区间/DIR租约交错及最终释放错误。
静态完整配置把21场景分别链接生产归档和支持归档，加公开6项和一个内含5项
单测的策略门禁，共49个Xmake注册项；共享配置28项，不把静态包装称为DSO内部拦截。

本轮手工构建普通/Clang ASan+UBSan的新27场景全部通过，真实共享DSO另6场景
通过。GCC TSan默认拒绝“多线程fork后创建新线程”，本候选未设置die_after_fork=0、
未屏蔽竞态。TSan保留9个实际可执行场景通过，18项退出77明确跳过；专门添加
同步子方的映射与三种交错场景，避免把整个fork测试面全跳过。

Xmake沿用项目退出77的跳过等价约定并打印专用SKIP标记；汇总必须另计跳过，
不能把框架返回成功冒称该场景实际通过。在当时的历史环境中，高 fd 两项因硬限额 16384 跳过；2026-10-17
已通过 #250 在独立宿主（fd 软、硬限额均 65536）执行补证，
见下方“高 fd 扩容”。其他 31 项 TSan fork 能力跳过仍独立。
局部阴性检测不等于所有调度、所有原子内存序的机器证明。

继有获取/释放回归的包装符号随映射分配迁移，仍观测真正分配地址和释放事件。
构造故障用例先完成进程域启动再设置实例失败计数；根域失败单独验证，不把新增
四个初始化mutex误算为原实例的三个。原console-held测试在prepare精确进入
协调区间时释放夹具，而不是等待fork返回再释放，避免测试夹具自己形成依赖环；
真正区间等待由新的format-held/directory-held保持正反断言。

## 7. 当前验收边界与尚未完成

较早阶段“未运行远端CI、未迁移create/destroy与默认借用”的记录已经过时。
截至2026-10-17，主线合并PR #235所用源码树已在真实CI通过完整Xmake、
静态/共享XPack、最低Linux 5.10.271/glibc 2.31、父子实际输出、映射
隔离、错误注入与安装消费。完整证据身份见[发布前核查](RELEASE_PREFLIGHT.md)，
不把其他提交上的成功假称当前重新执行。

**仍未解决**：GCC TSan 对 31 个 fork 后 worker/线程场景明确跳过；
高 fd 原先的未测缺口已有单独 #250 的真实运行证据，但仅限经核实
的 Ubuntu 24.04/GCC/Clang/硬限额 65536 环境；宿主自己的 stdio、区域设置、
动态装载、自定义格式化/分配器等外部libc状态不能由本库修复，
也不据此承诺任意多线程fork安全或墙钟硬时限。是否在严格限定的
Linux/glibc合同下发布，需要维护者单独接受这些风险。

同一日志目标的多个独立进程持有者仍按单所有者EBUSY合同拒绝；
本库没有承诺多进程共同轮转或改名。#218/#221维持开放，
不能因该文档或某一轮CI成功自动关闭。正式标签/Release需独立授权。

## 运行域集成补强（#223）

格式协调与资源协调已接入既有锁类跟踪，禁止资源到格式的反向获取。
格式锁的释放错误不再被丢弃；主操作错误优先，成功操作也不能掩盖释放失败。
GNU `%m` 仍使用进入调用时的 errno。普通整行或批次的非容量格式错误不再
输出半行/空行或增加成功记录；批量预处理失败时整批失败，完成水位仍按处理推进。

`libc-coordination` 增加十二项操作/错误/锁序回归。旧 Console/SIGPIPE 和输出锁
故障探针改为准确的对象地址，不能靠“第一个 mutex”识别目标。
格式化错误替代实现返回 -1 时显式设置 EILSEQ，避免把上次容量错误残留 errno
误当本次 libc 失败。所有原断言保留。

CAS 判定器包含故意错误的临时源码和全树扫描，运行上限由 30 秒调为 90 秒；
这是功能测试的完成时间，不是生产性能预算，没有删除负例、放宽扫描规则或
改变其他故障场景超时。各模式完整运行结果属于其具体源码和工作流，历史手工
结果不代替本轮 Xmake 验收。

格式锁取得后再次检查进程域封闭状态；等待期间被封闭的调用不进入 libc。
对应 post-lock-poison 用例在真实加锁成功后注入封闭，检查输出未改变且锁实际归还。

## 高 fd 扩容：独立不可跳过的验证入口（#218）

常规 `fork_fd_regression` / `fork_fd_production` 的 `grow` 与
`grow-failure` 在 fd 硬限额不足的环境仍明确退出 77；这是诚实跳过，
**不是通过**。新增独立 `fork-highfd-validation`（`.github/workflows/fork-highfd-validation.yml`）
在 Ubuntu 24.04 GitHub 宿主（非原 Ubuntu20.04 容器）直接构建两个静态
库身份并分别执行 `grow` 与 `grow-failure`，以实际编号至少 32768
的 fd 扩容/回滚分支为验收入口。

独立判定器读取 `RLIMIT_NOFILE`，软限额不够时只能在已有硬限额内上调；
硬限额不足直接失败，不能自动跳过或假装执行。驱动自身 `grow` 断言
fd 实际至少 32768，`grow-failure` 断言 mremap 故障已被消费；
所有四项真实子进程均须退出 0 并产生 `FORK_FD_PASS`，
任何 77、`FORK_FD_SKIP`、缺失成功标记、超时均判定未验证。
原始提交/tree、实际 GCC/Clang 与 Xmake 版本、二进制摘要、
fd 限额、每个子进程的 stdout/stderr/退出码和执行时长保存为独立制品。

这只针对高 fd 位图扩容及故障回滚，不能推广为任意大句柄上限、
所有 Linux/glibc 组合、fork 后多线程检测器覆盖或宿主外部 libc 安全。
**首次真实补证**：[#250 的高 fd 工作流](https://github.com/MyBlackHole/c-logger/actions/runs/39029945235)
在原始 head `583365edb65179dce98e6479b4d5542b1d9fcb4e`、
受测合并提交 `3da308e1eda20cbef036c34b01045f0f818f2491`、
树 `77130ddada72368d329709cb6c41a6a15b64d725` 上，
GCC 与 Clang 各执行生产静态/白盒 `grow`、`grow-failure`
四项，共 **8/8 真正成功、0跳过**。
宿主实测 RLIMIT_NOFILE 软、硬限额均为65536；
原始 ZIP artifact 为 GCC `12099722524`、Clang `12100191695`，
均含四个真实子进程退出码0、stdout成功标记、
编译器和Xmake版本、驱动摘要、源码tree与位图复原检查。
它不是旧 Linux5.10 客机上的高fd动态验证，也不等于解除31项TSan限制。

**最终合并与验收结论必须以精确 PR 最终 head 的实际运行结果为准**；
这段原始 head 证据不会被文档追加后的新提交冒称为同一SHA的重跑；
先前 skip 原始记录保留。

## TSan fork 后工作线程能力跳过的同源强制审计（#251）

`sanitizer-evidence` 的完整 TSan 静态测试中，由于运行库对多线程 fork
后创建工作线程的限制，Xmake 会将这些场景的明确退出 77 记作
skip-equivalent。因此 995 项测试登记且聚合结果为 0 失败时，
**不能写成 TSan 实际执行了 995 项**；在 #250 已归档的准确源码上，
实际为 TSan 执行 964 项、31 项明确跳过。

新增同一 CI 工作流的 `fork-coverage-accounting` 后置审计 job，
从两份**同源**运行证据 `sanitizer-evidence-thread`、
`sanitizer-evidence-address-undefined` 以及独立的 `tsan-fork-model`
原始 artifact 自动核对：

- 固定 31 项：公开 `fork_reinit_public_test` 5 项，
  真实生产归档 `fork_runtime_production_regression` 13 项，
  白盒支持归档 `fork_runtime_regression` 13 项；逐项验证明确的
  TSan `tsan-post-fork-worker` 原因及 Xmake skip 回执，不把 77 算作检测通过；
- ASan/UBSan 对应 31 项必须有真实 `FORK_REINIT_PUBLIC_PASS`
  或 `FORK_RUNTIME_PASS` 语义标记，且完整日志逐项运行/通过名目一致、
  两构建的源 commit/tree 一致并匹配当前 PR HEAD，退出码都为 0；
- TSan 已支持的 9 个真实同步 fork 锚点仍须有 PASS 回执；
  检测器资格的健康案例退出 0、真实锁逆序与真实数据竞争诊断各退出 66
  且出现指定的警告文本。缺任一原始制品、缺跳过/真实通过标记、
  重复或额外跳过、篡改原因、异源日志及关闭警告均失败；
- 审计器和其正反自测结果保存在独立 JSON/原始输入 SHA-256 制品中。
  实际接入结果只有在该最终版本 CI 成功后才可以认定为有效门禁。

这仅补齐**跳过与对照执行的证据可核验性**，不能通过文本审计
创造缺失的31条 TSan 多线程 fork 后 worker 动态覆盖；
`#221` 的运行库能力缺口、`#218` 宿主调用边界与 D1–D6 风险签收
仍单独保持开放，不得以该审计器的绿色结果关闭或签收。
