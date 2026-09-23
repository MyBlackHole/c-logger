# Round 2：Audit 生命周期与提交失败状态

本轮输入是 `prod_c_logger_queue_flush_fix.tar.gz`，不是早先的多实例版本。
只收敛 Audit 生命周期、单 writer 资源归属、日志已提交后的 checkpoint 失败处理、checkpoint 持久化清理，以及直接相关的 Audit child 拒绝路径。
**不是 Production v1 验收；不宣称 crypto、解析器、跨归档恢复或全部 fork 行为已安全。**

## 生命周期与锁

每个 active lifetime 拥有私有 `audit_runtime_t`，其中保存 logger、writer lock fd、state 路径、instance ID、seq/txn、链头、待提交 checkpoint 和故障状态。
没有跨实例共用的 `g_writer_lock_fd` / `g_state_path` / `g_prev_hash`。

`g_control_mu` 串行化完整 init/shutdown/dispose；`g_operation_mu` 串行化 Audit 写入、flush、实例快照和实例发布/分离。
正常写入不持有 control mutex。固定锁序是 control -> operation，内部 helper 不反调 public init/write/shutdown。
Audit 原本就是同步串行链，此处没有给普通 MPSC logger 增加锁。

```text
init:     control -> STARTING -> 创建隐藏 candidate -> 恢复 -> START+checkpoint
                                                        -> operation -> 发布 -> RUNNING
shutdown: control -> STOPPING -> operation（等当前写完成）-> 维护 checkpoint / STOP
                  -> 分离实例 -> 销毁旧 logger -> 关闭旧 writer fd -> IDLE -> 释放 control
```

START 与其 checkpoint 都成功之前，不发布实例。失败初始化直接清理候选，不追加假 STOP。
STOPPING 先关闭 admission，再等待 operation，等待中的写入进锁后再次检查；已经进行中的写入允许完成。
正常 STOP 是最后一条记录；若 I/O 状态不确定或待修 checkpoint 无法提交，不追加成功 STOP。

调用进入时捕获内部 generation，取得 operation 后复核，避免原生命周期的延迟调用跨过 shutdown/reinit 后写到新实例。
内部 generation 是 admission 边界，不是持久化 seq 或公开事务 ID。

重复 init 稳定返回 EALREADY，没有创建另一组候选文件的副作用。其他遵守同一 POSIX record-lock 协议的进程竞争同一 destination 得到 EBUSY。
writer fd 由对应 runtime 持有到 logger 完成清理之后；旧 shutdown 不可能再关闭新 session 的 fd。
不删除 `.audit.lock`，不把其存在当作锁持有证明。

## 故障状态与提交事实

| 状态 | 新记录 | audit_flush | shutdown_status |
|---|---|---|---|
| RUNNING | 同步 write + file/directory sync，再提交 checkpoint | 同步并检查错误 | 正常 STOP、checkpoint、清理 |
| CHECKPOINT_FAILED | 拒绝，返回保存的 checkpoint 错误 | 只重试已确认记录的 checkpoint；成功后恢复 RUNNING | 尝试修 checkpoint，成功再 STOP；仍失败则不追加 STOP、清理并报错 |
| IO_FAILED | 拒绝，返回保存的 I/O 错误 | 报错，不清除故障或自动恢复写入 | 不追加 STOP，清理并报原错误 |

记录经严格后端确认后，立即推进内存 `head` 和 `seq`，再读取 offset、写 checkpoint。
checkpoint 错误不会把内存链头留在上一条；已确认记录的 hash/seq 保存在 pending checkpoint 中。
修复过程不重新追加原始事件，不改变记录 seq，不把修复当成一次新业务操作。

进入 logger 之后的失败保守归为 IO_FAILED：可能零字节、半条、整条未确认甚至已持久化。**返回失败不是“肯定没写入”。**
即使移除故障后下一次 syscall 能成功，原 runtime 也不恢复追加；需要关闭并核对/恢复日志后再建立新实例。
现有恢复器仍有已知阻断项，不能据此承诺任意日志损坏都能靠重新 init 自动恢复。

参数、枚举和编码超长在进入后端前检查；这些无输出副作用的错误不破坏一个健康 runtime，也不消耗 committed seq。
编码使用真实 `LOGGER_MESSAGE_MAX` 上限，避免 Audit 先以 8 KiB 编码、后端再以 4 KiB 截断的假设。
普通用户事件不能伪造名为 AUDIT_START/AUDIT_STOP 的系统事件。

`failure_policy` 仍只是提供给业务的策略值，不替应用自动撤销/执行受保护操作。无论 REPORT/DENY，库都不能在已坏链头上盲写。
本轮仍然强制审计文件同步；旧 `fsync_each_record` 字段并不提供弱化 durability 的新模式。

## 新增的最小控制接口

```c
int audit_get_status(audit_status_t *out);
int audit_shutdown_status(void);  /* 0 / -1 + errno */
```

`audit_status_t` 提供 state、error_code、checkpoint_dirty、committed_seq、checkpoint_seq、instance_id。
committed_seq 是本实例已确认严格日志提交的序号，不等于 checkpoint 已追上，也不是对摘要实现正确性的认证。
STARTING/STOPPING/FORKED 快照不暴露 candidate/session 的部分字段。
IDLE 保留最近一次关闭/初始化失败快照；重复 shutdown 为幂等 no-op，不抹除这一诊断记录。

状态查询是瞬时诊断，不是某条并发调用的提交回执。不能因为随后看到了 RUNNING，就把此前返回错误的业务事件盲目重放。
例如：

```c
if (audit_write(&event) != 0) {
    int error = errno;
    audit_status_t status;
    if (audit_get_status(&status) == 0 &&
        status.state == AUDIT_STATE_CHECKPOINT_FAILED) {
        /* 某条日志可能已提交。通知维护/控制线程，禁止自动重写 event。
         * 恢复后由该控制线程调用 audit_flush() 追平派生 checkpoint。 */
    }
    /* 使用独立错误通道处理 error，遵循业务失败策略。 */
}
```

原 `audit_shutdown()` 保留为 void wrapper；需要知道 STOP 或 checkpoint 是否失败，应检查 status 版本。
`audit_flush()` 在 IO_FAILED 时不再假装“再次 fsync 就修好了”。
`audit_instance_id_copy()` 在 operation 保护内复制；旧 pointer API 现在返回线程本地快照，下一次同线程调用替换它，失败为空串。

正常 API admission 错误：尚无实例 ENODEV；初始化过程中 EAGAIN；关闭中或跨生命周期的延迟调用 ESHUTDOWN；继承 child runtime ECHILD。
seq 分配、编码、提交、txn 分配在同一个 operation 序列内。调用者依然必须保证 event/字符串有效，不能并发修改同一个 event。
ATTEMPT/RESULT 不是业务事务的原子提交；应避免跨 shutdown 保留未完成的 txn。

## Checkpoint 持久化修复

`audit_checkpoint_persist()` 使用唯一 `mkostemp(..., O_CLOEXEC)` 的 0600 临时文件。
流程仍为 write-all -> fsync(tmp) -> close -> rename -> fsync(parent)，格式仍为 checkpoint v2，没有声明 v1 自动迁移。
短写继续；EINTR 重试；零字节写按 EIO 失败，不能无限循环。
fsync/close/rename/目录 fsync 的错误都向上传播，错误路径必然执行所拥有 fd 的关闭，不再用 `fsync(fd) || close(fd)` 短路。
Linux close 失败不按同一 fd 号重试；保存最初错误，清理不覆盖它。
rename 后若目录 fsync 失败，目标可能已经改变：报告失败并重试相同的 committed checkpoint，不删除目标或倒退链头。
本轮不新增生产热路径测试回调；新的故障与调度测试均在测试可执行文件中用链接器 --wrap 注入。
原有环境故障 hooks 仍在库中，必须在后续做生产构建隔离。

## Audit fork 与取消

本轮为 Audit 移除了“给继承的 pthread 对象直接赋初始化常量”的路径。
`audit_after_fork_child()` 只做失效标记；只要本进程已安装过 Audit fork guard，child 的 audit_init/write/flush/id-copy/shutdown-status 都在触碰继承锁之前拒绝。
新实例需要在 exec 后初始化；或者在应用创建线程、初始化 Audit 之前安排 fork。
CLOEXEC fd 在 exec 时关闭；此函数不是继承内存/fd 的通用回收器，不能在长期 fork+continue child 中调用它然后期望正常服务。
旧的 single-writer/crash 测试改为 child 立即 exec 后再操作 Audit，没有放宽测试断言或把失败忽略。
普通 Logger/Console 的 fork 契约仍待单独收敛，不因本次 Audit 改动被宣布解决。

持有生命周期/operation 锁时暂时禁止 pthread cancellation，离开后恢复旧状态。
这样取消不会把 mutex 永久留给消失的线程；取消也不是日志提交的回滚，调用线程被取消时记录可能已提交。
没有新增磁盘 I/O 超时，API 仍可能等待阻塞的 write/fsync。

## 单 writer 保证的环境边界

此版本继续用传统 process-associated POSIX record lock 兼容现有格式/协议，不以新 Linux OFD lock 替换它。
它是 cooperative coordination，不是对恶意管理员或绕开锁直接写文件的防护。
应用不得从其他位置打开后关闭同一 lock inode，也不得 unlink/recreate 活跃 lock 文件；传统 fcntl 锁会被同进程对该 inode 的其他 close 影响。
要求专用、可信、本地目录；网络文件系统和路径别名/多个名称复用相同 state 文件不在本轮已验证保证内。

## 依据与范围

Linux man-pages `fsync(2)`：文件 fsync 不自动持久化目录项，目录需独立同步。
Linux man-pages `fcntl_locking(2)`：传统锁为进程关联，任何同 inode fd 关闭会释放该进程的锁，且为 advisory。
Linux man-pages `fork(2)`：多线程 fork 后到 exec 前只应调用 async-signal-safe 函数。

源码/测例与原始输出见 `validation/round2/` 和 `validation/ROUND2_RESULTS.md`。
现有解析器与 crypto 层没有在本轮被认证；特别是 void EVP hash 失败输出零摘要、SM3 的移位 UB、超长恢复记录/归档身份问题仍是发布阻断。
