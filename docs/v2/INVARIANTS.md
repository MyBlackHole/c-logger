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
