# Lockdep-style debug assertions

状态：**#103 第三阶段基础设施**。

c-logger 不通过 `pthread_mutex_trylock()` 推断“当前线程是否持锁”。debug/test
构建使用显式 TLS held-lock stack；production 通过
`LOGGER_ENABLE_LOCKDEP=0` 把调用点编译为空操作。

## 当前 lock class

- Global control
- Global lifetime
- instance emit
- instance progress
- queue wait
- Console
- Audit control
- Audit operation

当前允许的嵌套边严格来自 `docs/v2/LOCKING.md`：

```text
Global control -> Global lifetime -> one instance lock
Audit control  -> Audit operation -> one instance lock
```

instance lock 之间禁止嵌套；Console 当前不允许与其他 tracked lock 嵌套。

## 检查内容

debug engine 记录：

- 当前线程持有的 lock class；
- 具体 lock object 地址；
- acquire stack/depth；
- LIFO release；
- recursive/reacquire；
- allowed predecessor/successor。

提供：

- `logger_lockdep_assert_held()`
- `logger_lockdep_assert_not_held()`

发现 invariant violation 时 debug/test build 直接 `abort()`，与 Linux
debug invariant 的 fail-fast 用途一致；production 不改变错误模型。

## 边界

本 PR 只建立 tracking engine 和 relation-matrix regression。下一步把真实
Global/Audit/instance mutex/rwlock acquisition 接入 tracking，然后在已有
locked helpers 上放置 assert-held。这样避免“为了 lockdep 重写锁抽象”与
“同时修改 production locking semantics”混在一个 commit 中。


## 真实接入：Global / Audit

tracking 只跟随已经成功发生的真实 pthread transition，不改变锁语义：

- Global `g_control_mu`：成功 lock/trylock 后登记，成功 unlock 后撤销；
- Global `g_lifetime_lock`：read/write/try-write 共用
  `LOGGER_LOCK_GLOBAL_LIFETIME` class；
- Audit `g_control_mu`：现有 cancellation-aware scope 成功获取后登记；
- Audit `g_operation_mu`：普通 operation 可以独立获取；controller 内的嵌套
  acquisition 必须从 Audit control 进入。

Global publish/unpublish 与 Audit runtime borrow 位置加入 `assert_held`，让 debug
build 直接验证“访问受保护对象时对应 lock contract 仍成立”。

本阶段不改 instance emit/progress/queue-wait guard；它们单独进入下一接入阶段。


## Instance / Console guard 接入

instance lock 不复用 generic `pthread_mutex` guard 的 class：

- `pthread_mutex_emit[_checked]`
- `pthread_mutex_progress[_checked]`
- `pthread_mutex_queue_wait[_checked]`
- `pthread_mutex_console[_checked]`

每个 guard 仍调用原始 pthread mutex primitive，只在成功 transition 后更新 debug
tracking。这样 call site 直接暴露 lock class，reviewer 不需要根据 mutex 地址猜
语义。

`logger_sync_outputs_locked()` 增加 emit assert-held，明确 locked helper contract。
