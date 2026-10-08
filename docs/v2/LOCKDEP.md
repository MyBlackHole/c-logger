# 锁依赖调试检查

状态：#103 跟踪基础设施与 #126 获取前检查已实现。

c-logger 使用显式线程本地（TLS）持锁栈检查项目锁契约。Debug/test 构建启用
lockdep；生产构建通过 `LOGGER_ENABLE_LOCKDEP=0` 将检查与记账编译为空操作。

## 跟踪的锁类别

- Global control
- Global lifetime
- instance emit
- instance progress
- queue wait
- Console
- Audit control
- Audit operation

允许的嵌套关系来自 `docs/v2/LOCKING.md`：

```text
Global control -> Global lifetime -> one instance lock
Global control --------------------> one instance lock
Audit control  -> Audit operation  -> one instance lock
Audit control  --------------------> one instance lock
```

第二、四条只用于 controller 独占的 candidate 或已完成 unpublish/drain 的对象。
instance 锁之间禁止嵌套；Console 当前不允许与其他跟踪锁嵌套。

## 检查与记账时机

跟踪只覆盖项目显式接入的 mutex/rwlock 路径，不拦截任意 pthread 调用：

- 可能阻塞的 mutex、rwlock read lock 和 write lock：先无副作用地检查参数、递归
  获取、锁顺序及 TLS 栈容量，再调用 pthread primitive；只有 primitive 成功后才
  登记到 TLS。
- mutex trylock 和 rwlock try-write lock：先执行非阻塞 pthread primitive。失败时
  原样返回 pthread 错误码，不检查锁顺序，也不改变 TLS；成功后再检查并登记。
- unlock：先检查当前线程 TLS 栈顶的锁类别、对象地址及 LIFO 顺序，再调用 pthread
  primitive。pthread 解锁成功后才移除记账；失败时按既有路径 abandon 该项并把错误
  交由调用方处理。对象封闭方式仍由相应同步错误契约决定。
- `pthread_cond_wait()` 的 mutex 在等待期间由 pthread 暂时释放，并在返回前重新获取。
  这次等待不改 TLS 持锁栈；返回后仍按已持有处理，最终通过跟踪 unlock 释放。

## 已接入调用点

- Global 的 `g_control_mu` 与 `g_lifetime_lock`：分别使用 mutex 包装和共享
  `LOGGER_LOCK_GLOBAL_LIFETIME` 类的 rwlock 包装。
- Audit 的 control/operation mutex：覆盖 cancellation-aware scope 和 controller
  内的 operation 嵌套获取。
- instance emit、progress、queue-wait 和 Console mutex：通过显式锁类别 guard 接入；
  `logger_sync_outputs_locked()` 在调用点断言 emit 锁已持有。

这些检查分别由 `logger_lockdep_check_acquire()` 和
`logger_lockdep_check_release()` 提供。检查函数本身不改变 TLS 状态；成功获取/释放
后的 tracker API 才更新持锁栈。关系矩阵、Global/Audit 锁图和条件变量约束见
`docs/v2/LOCKING.md`。

## 诊断与边界

debug/test 还提供：

- `logger_lockdep_assert_held()`
- `logger_lockdep_assert_not_held()`

发生递归获取、禁止锁序、错误释放顺序或断言失败时，debug/test build 输出锁依赖诊断
并 `abort()`。生产构建不启用这些诊断，也不改变原有错误模型。

这不是完整 Linux lockdep 的用户态移植：实现使用固定锁类别关系表和固定容量 TLS
持锁栈，不推导动态锁图，不观测项目未接入包装的 pthread 锁，也不分析任意程序的
锁依赖。检查只证明已建模锁类别及已接入调用点满足本项目契约。

回归场景覆盖真实 NORMAL mutex 自重入、两线程反向锁序、错误 unlock 前置诊断、失败
trylock、rwlock 转换、TLS 线程隔离及条件变量等待返回后的记账状态。完整构建与测试
命令由仓库测试配置和 CI 维护。
