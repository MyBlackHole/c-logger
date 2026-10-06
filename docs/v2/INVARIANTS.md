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
