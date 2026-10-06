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
