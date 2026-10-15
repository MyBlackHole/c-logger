# fork 描述符快照与继承引用隔离（#218 第二增量）

> 以下保留对应早期增量的设计边界。当前候选已加入运行域重建，
> 早期“子方仍 -ECHILD”的状态不再适用于有效快照路径；以
> [FORK_RUNTIME.md](FORK_RUNTIME.md) 为当前实现入口，不将历史测试当成新运行。

本增量基于 PR #219 的 `40e0c9a8e04be9dba3b81bada7414d04b943340a`。
**只实现文件/Syslog 描述符的登记、fork 一致快照及子方关闭，不是完整子方重初始化。**
大块堆内存、DIR 对象内存、Global/Console 运行域和 libc 隐含状态尚待下一阶段处理。
子方 `logger_process_init()` 仍为 `-ECHILD`，不得因 fd 已关闭就复用旧锁或创建新 worker。

## 1. 为什么不只登记运行实例

资源可以已由 open/openat/socket 取得，但尚未安装到实例字段；临时保留目录也没有
长期实例槽位。本实现把“取得并登记”放在同一 fd 协调锁中，关闭则在同锁内先消费
登记，再执行一次真正 close。候选发布/后端整体移动不改变 fd 的登记归属。

生产文件/Syslog 的六个逻辑获取站点都已迁移，分别保存在
`validation/fork_fd_sites.tsv`；底层直接系统调用及新增映射仍由原资源清单审核。
纯 fd 取得包装不是新公开 API，不增加兼容开关，也不替换业务 fork。

```text
取得 fd：锁定 → 系统调用 → 登记（失败则关闭候选）→ 解锁 → 交出 fd
关闭 fd：锁定 → 消费登记 → close 一次 → 释放空表映射 → 解锁
fork：   prepare 固定登记边界 → fork → parent 解锁
                                  └→ child 关闭快照中 fd，隔离旧锁
```

正常文件写入/入队不经过该锁；创建、关闭、重连和轮转资源变更经过。
协调锁可能等待文件系统操作，不声明 fork 硬超时或实时性能。库内调用者沿用既有
取消作用域；工作线程协作退出、不被外部取消。不能从信号或 syscall 插入回调重入。

## 2. 登记表只保存整数，不保存对象指针

私有匿名映射按 fd 建立位图，首次逻辑长度 4096 字节，覆盖 fd 0..32767；
只有更高编号才按两倍长度扩大。更新只在 fd 锁内，最后一个引用关闭后释放映射。
内存开销与最高 fd 相关，而不是只与实例数相关；例如高编号稀疏 fd 也可能扩容。
没有性能提升或零系统调用成本声明。

不用候选栈指针、实例地址或 DIR 指针作为链表节点，避免候选退出、结构移动或最终
释放之后出现悬垂登记。mmap/mremap 失败时原错误返回，刚取得但尚未交出的 fd
必须关闭一次；扩容失败不能破坏已有映射和描述符。子回调不调用 munmap/free。

本地环境的 fd 硬限额为 16384，高 fd 两项测试不能运行；测试输出显式
`FORK_FD_SKIP` 和退出77，不当作实际通过。Xmake 会显示该标记；汇总时须另计跳过，
不能只凭 Xmake 聚合成功写出高 fd 扩容已验证。原资源清单 mremap 行暂为 adjacent。

## 3. 条件移交与错误

fdopendir 失败时，fd 仍归调用方，由已迁移的候选 cleanup 关闭；成功时 DIR 接管，
登记保持至 closedir。子方只关闭该 fd，不调用继承 DIR 的普通析构或 readdir。
DIR 的堆内存隔离不是本阶段已完成的能力。

close 前先移除登记；即使真实 close 后返回 EINTR/EIO，也不恢复位或重试原整数。
关闭错误不能让随后新打开的同编号资源成为下一次 fork 的关闭目标。
映射取得失败的回滚关闭也不覆盖原始主错。

同步状态不确定或空映射释放失败时，记录粘滞 gate_error，停止新的资源取得，
后续子方不遍历不可靠快照。解锁失败后的资源可能由登记表保守保留；不假装完整回滚。
已经等待不确定锁的线程不具备活性承诺。成功 fdopendir 后解锁失败时仍交出 DIR
所有权，后续资源关闭会报告 gate_error；不能丢失该指针假装从未移交。
这些是异常平台/故障注入边界，不是普通业务 I/O 失败的推荐处理。

## 4. atfork 与其他处理函数

prepare 只固定 fd 登记，不获取实例输出锁、Global 控制锁，不等待工作线程退出。
本库 prepare 至 parent 结束期间，同线程其他 atfork handler 的普通 Logger 调用
通过作用域提前以 EDEADLK 拒绝，避免持有 fd 锁后反向取得输出锁。
parent 成功/失败路径只释放本轮取得的协调锁，保留 errno。

child 先关闭普通准入，再读取可靠快照、消费登记、调用 close；不操作旧 pthread
对象、不执行 LOCK_UN、不分配、不格式化、不建立线程。关闭错误记录到 child_error，
即使一个 close 失败也继续消费其他已知引用。没有快照或继承状态中再次 fork 时，
不触碰已经隔离的 mutex，不再次关闭被消费的 fd。

宿主和其他 handler 不得私自关闭、dup2 替换本库仍拥有的内部 fd；仅凭整数无法
事后识别这种所有权破坏。host 的描述符不进入本表，保留在子方。原始系统调用、
_Fork/vfork 或信号处理函数 fork 不属于本协议支持入口，PID 防御仍保留。

Linux flock 与打开文件描述关联，继承引用共享锁。子方只能关闭自己的引用，
不能以 LOCK_UN 解掉父方锁。测试还要求子方保持存活时，父方销毁后独立描述符
立即能够重新取得文件锁，以检查没有子方残留引用。
依据：Linux man-pages 的 flock(2)、close(2)、pthread_atfork(3)。

## 5. 可执行验收

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release --build_shared=n --build_tests=y \
    --build_private_tests=y --build_regression_tests=y
xmake test -g fork-fd -j1
xmake test -j1
```

`fork_resources_test` 用公开 API 连接真正生产静态库/DSO，父方枚举真实 fd，子方
仅用 fcntl 检查 Logger 的三个引用已关闭，父方继续输出，且子方存活不延长锁寿命。

`test_fork_fd.c` 十五个独立场景：真实 Syslog、handler 顺序、fd/DIR、嵌套 fork、
子方 close 后 EINTR 与整数复用、取得未登记窗口、关闭后整数复用窗口、父方锁寿命、
mmap 失败回滚、高 fd 扩容/失败、外来 fd 拒绝、parent 回调收尾、munmap 失败及锁失败。
包装只在测试驱动中；子方直接 malloc/calloc/free/普通格式化/线程创建/互斥锁会被探针拒绝。
父回调收尾场景只验证回调合同，不冒称真正注入 fork 系统调用失败。

底层生产静态归档和白盒支持归档分别运行这些场景；静态包装不冒称 DSO 内部拦截。
`test_fork_fd_policy.py` 检查六个逻辑获取站点和禁止绕回裸调用的限定词法规则，
不是预处理器/跨函数证明，也不代替实际动态回归。

## 6. 下一阶段边界

本阶段不新增公开导出，不改变配置/队列记录布局，不修改文件轮转/同步顺序。
但 fork 后 Logger 自有 fd 不再保留至 exec/_exit，这是本阶段有意实现的运行行为。
完整子方新运行域、大块内存回收、显式初始化前提、新 create/destroy 接口以及
Linux/glibc 格式化/时区/线程链仍未完成。同文件多进程写入/轮转也不能从本快照推出。
