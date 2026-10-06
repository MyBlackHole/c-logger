# c-logger 2.0 Invariants

状态：**逐步冻结**。

当前已冻结的元不变量：

1. partially initialized object 不得 publish。
2. final free 前必须证明所有可能使用者已经退出。
3. proof failure 时安全 retain/leak 优先于 UAF。
4. `lock != lifetime pin != join != refcount != atomic`。
5. 用户态并发实现必须满足 ISO C11 data-race rules。
6. hot path 资源必须 bounded，不能用无限增长掩盖 overload。
7. silent truncation、silent durability downgrade、silent security downgrade 禁止。
8. Linux 内核 primitive 只能由真实问题驱动，不作为风格装饰。

对象级 invariant 随 #102/#103/#104/#105 逐步补充。


## #102 所有权补充不变量

9. 构造失败 rollback 与正常 teardown 一样遵守 proof-before-free；同步对象、worker 或 census 的证明失败时禁止继续 final free。
10. refcount 只能用于真实独立 owner；不能为了让裸指针并发 destroy “看起来安全”而给每次 API 调用机械 get/put。
11. 从属 worker/queue/backend 不因为跨线程访问就自动成为 parent 对象的 refcount owner；其 lifetime 由结构 owner + quiescence/join 证明。
12. `inc_not_zero` 的调用者必须已经拥有可安全解引用 refcount 字段的 lifetime proof，它不能复活或保护一个可能已经 free 的裸指针。
13. process-level census 与 object refcount 是不同机制，禁止合并语义。


## #103 并发与锁补充不变量

14. reservation counter、metrics counter、state atomic 与 payload publication
    primitive 必须分开说明；“都是 atomic”不能合并语义。
15. 任何 lockless/atomic state machine 必须有 linearization、publication、
    happens-before、reclamation、ABA、progress 和 failure/retry proof；TSan
    不能替代该 proof。
16. Queue wakeup 的 live `consumer_waiting` 修改必须保持在已证明的 acq_rel
    RMW handoff protocol 中；不能重新插入 plain concurrent store 或用 SC fence
    注释代替 C11 proof。
17. condition variable 的 signal/broadcast 只是通知，predicate 才是状态；wait
    必须在 mutex 下循环重新检查 predicate。
18. debug lockdep 是 order/assertion checker，不是同步 primitive，也不是 lifetime
    pin；production correctness 不能依赖 `LOGGER_ENABLE_LOCKDEP=1`。
19. `Global/Audit control -> instance` 只在 controller 独占 candidate，或对象已经
    unpublish + drain 成为 retired object 后合法；published object 仍必须经过
    lifetime/operation scope。
20. instance emit/progress/queue-wait lock 之间禁止嵌套；worker 必须保持
    queue consume -> emit -> progress 的分阶段结构。
