# Global facade 生命周期与取消契约

本轮只收敛可选的应用默认 Logger。第三方 SDK 仍优先借用宿主显式实例或使用
回调；没有默认启动线程，没有新增进程管理、扫描或自动 fork 恢复。

## 内部状态与所有权

默认实例不向用户暴露。一个内部 64 位原子 ticket 编码 generation 和 phase，
避免分别读取两个原子值造成不一致组合。每次实际开始 init 候选创建时消耗一个
generation；失败也不回退 generation。达到计数上限返回 EOVERFLOW，绝不复用。

```
IDLE --init--> STARTING --成功发布--> RUNNING --shutdown--> STOPPING --> STOPPED
                 |                                                        |
                 +--失败--> IDLE（启动前）/STOPPED（曾关闭） <---再次 init-----+
```

- control mutex 串行候选创建、发布、旧实例完整销毁及文件 owner 释放。
- lifetime rwlock 在数据/控制入口为实例提供读侧 pin，不让所有 producer 串行执行。
- 锁顺序为 control → lifetime → 实例锁；读侧入口不会再获取 control。
- Global 调用不能获得显式指针。显式 logger_t 的销毁仍须由 owner 停止并 join 使用者。

RUNNING 的重复 init 立即 EALREADY，不读取配置内容、不打开候选文件/锁/socket，
不创建 worker。STARTING/STOPPING 中到达的 init 通过控制锁等待；取得控制后重新
判断是否已有实例。它可以在旧生命周期完整结束后创建新实例，并非热更新接口。

## 全部读侧入口共享准入

覆盖 global_write/LOG_*、flush/status、set_level、reopen、dropped、metrics 和
I/O metrics。捕获 ticket，先检查 phase，再获取 lifetime 读锁，随后复核同一
完整 ticket。此时才使用 g_logger。调用若被延迟到 shutdown/reinit 之后，不能
写入、修改级别、reopen 或读取新实例，返回/设置 ESHUTDOWN；不自动重试新代次。

阶段结果：

| 情况 | status API / void API 的 errno |
|---|---|
| 尚未成功初始化（IDLE） | ENODEV；仅 LOG_* 保留原有 stderr 启动输出 |
| 正在创建（STARTING） | EAGAIN |
| RUNNING 且 ticket 未改变 | 使用当前实例 |
| STOPPING/STOPPED，或捕获 ticket 已失效 | ESHUTDOWN |
| fork 继承状态 | ECHILD，在触及继承锁之前拒绝 |
| 同线程嵌套 global 调用 | EDEADLK |

void API 不返回状态，指标在拒绝时为零（errno 区分“无可用实例”与健康的零计数）。
NULL metrics 输出维持 no-op。logger_set_level 现在拒绝超出 TRACE..OFF 的枚举。
正常成功不覆盖调用者进入 API 前的 errno；成功后的 errno 不作为业务确认协议。

shutdown 关闭准入之后，新控制 reader 也不进入 rwlock，避免持续 metrics/level
流延长旧 reader 集合。已经准入的调用仍完成旧实例操作；等待者复核失败后退出。
这不保证线程调度公平、固定等待时限，或替底层阻塞 I/O 提供超时。

启动 stderr 路径也持有 lifetime pin：一条已经开始输出的启动消息完成前，不能
发布新实例；shutdown 即使没有实例，也等待已有启动输出结束。关闭后不再回落
到 stderr。此便利行为只面向应用，业务 SDK 不应隐式依赖它。

## Shutdown 的 generation 和状态返回

新增 `int logger_shutdown_status(void)`，返回 0 或 -1 + errno；原 `logger_shutdown`
调用同一实现并丢弃状态。公开结构布局、原有函数签名没有改变。

1. 控制调用捕获它观察到的 generation。
2. 等待控制锁后复核；若已换代，ESHUTDOWN，不关闭新实例。
3. 同一代关闭已完成，返回 0（不会重试已释放实例）。
4. 关闭准入，等待已有 reader，解除发布。
5. 保持 control 锁，排空/同步/join/close/释放 owner，之后才发布 STOPPED。

已观察到 STARTING 的 shutdown 可以等待同代 init 结束再关闭；若等待期间又进入
了另一个代次，则拒绝。契约绑定“库入口内捕获 ticket”的时点，不是业务线程在
调用函数之前的任意程序时点。

普通 I/O 错误下旧实例仍销毁，status 返回首次错误。重连/后续 I/O 成功不抹掉
该实例历史故障。下一次同代空 shutdown 返回 0，不是此前错误被撤销。新实例可
按正常流程重新创建。不能通过重复 shutdown 重试旧指针或重放业务事件。

## 取消与重入

Global API 在进入 once/共享锁/实例输出之前暂时关闭 pthread cancellation；
释放 lifetime/control 和内部 pin，清除 TLS 重入标志之后，恢复原取消状态。
不改变调用者的 cancellation type。已经禁用取消的调用者仍保持禁用。

- 同步输出、异步 enqueue/fallback、等待输出水位的 flush、reopen、init 和完整
  teardown 均由这层覆盖。取消不是事件/文件/成功发布实例的回滚。
- 恢复 ENABLE 后，挂起取消可立即生效或在后续 cancellation point 生效；用户的
  cleanup handler 此时可重新调用 global API，不会继承本库尚未释放的 global pin。
- **阻塞 I/O 或等待很长时，取消也被延迟。没有新增 I/O deadline 或强制线程终止。**
- 防护只覆盖本库 global scope，不能让任意宿主代码都变成 async-cancel-safe。
- 同线程嵌套调用在拿锁前 EDEADLK，用于 printf/插桩回调误入；不是信号安全保证，
  不支持跨线程回调等待环，也不允许 pthread_exit/longjmp 穿过库调用。
- LOG_* 参数在进入库之前求值，拒绝机制不保护参数求值的副作用/数据寿命。
- **显式实例、Console 的全入口取消和重入仍未完成专项收敛。**

旧 opt-in fork helper 同样获得控制锁，避免旁路与正常生命周期相互覆盖；其
原有单线程前置条件和默认不编译策略不变。raw fork 的 ECHILD 契约没有放宽。已完成关闭后的 reopen（包括旧 helper 后）由
此前 ENODEV 收紧为 ESHUTDOWN；初始 IDLE 的 reopen 仍为 ENODEV。这是错误语义
变化，不是 ABI 布局变化，调用者不应将 ESHUTDOWN 当成可重试旧实例。

## 证据与范围

详见 `validation/GLOBAL_LIFECYCLE_RESULTS.md`。测试用 linker wrappers 控制精确
交错，不向生产库增加测试钩子。共享配置的白盒测试使用同源静态库，真实 `.so`
另有并发 init/生命周期集成测试。没有重新测吞吐，不能据本轮推导性能收益。

参考语义：
- POSIX rwlock reader 的优先级/等待行为： https://man7.org/linux/man-pages/man3/pthread_rwlock_rdlock.3p.html
- 取消状态及长时间禁用的代价： https://man7.org/linux/man-pages/man3/pthread_setcancelstate.3.html
- cond_wait 取消前重新取得 mutex： https://man7.org/linux/man-pages/man3/pthread_cond_wait.3p.html

## 与显式入口的联动（后续加固）

显式 Logger、Console 或 worker 内部回调再次进入全局 API 时，在取得控制锁/生命周期锁之前返回 EDEADLK。正常 Global → 显式后端调用仍合法，内层 scope 不会提前恢复外层已禁用的取消状态。详见 EXPLICIT_CONSOLE_SCOPE.md。
