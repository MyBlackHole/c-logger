# 显式 Logger / Console：取消、重入与所有权

此文描述当前加固版本。公开函数签名、公开结构布局、普通文件/Audit 格式未变。
它不让裸指针在并发 destroy、释放后访问或卸载代码时自动变安全。

## 私有 scope 的目的和限制

`logger_scope.c/.h` 使用每线程标记与取消状态保存，不使用全局执行锁或实例引用计数。
Constructor、日志格式化/提交、同步回退、flush、reopen、Syslog metrics、最终处置和 Console I/O
在接触资源前禁用取消；资源清理、va_end、解锁后清除标记，再恢复原取消状态。
有效的延迟取消请求会保留，取消不回滚已输出的记录、配置更新或已经完成的销毁。

其他线程对活跃实例的正常并发仍然允许，实例内部锁仍负责编排。禁用取消不是超时机制：
文件、stdio、worker 输出、锁及调度可能等待。不要靠 pthread_cancel 强制完成关闭。
Logger worker 由 running/notify/join 协作关闭；宿主不得取消私有 worker。

所有普通 API 都不是信号安全接口，也不承诺异步取消从任意指令发生时的通用安全。
调用者不得在 `PTHREAD_CANCEL_ASYNCHRONOUS + PTHREAD_CANCEL_ENABLE` 状态下把 Logger/Console
API 当作 AC-Safe 函数使用；POSIX 并不要求这些普通库函数具备该性质。若宿主必须保留
ASYNCHRONOUS type，应先把 cancellation state 设为 DISABLE，再调用 Logger，并由宿主在自己的
安全边界决定何时恢复 ENABLE。库必须保持这种 caller-disabled 状态和原 type，不得擅自启用。
不得从 printf 扩展、FILE 回调/拦截函数中重新启用取消、pthread_exit、longjmp 或抛出穿越 C 边界的异常。

## 所有权交接必须区别于普通写入

`logger_create()` 在构造成功之后、指针交给宿主之前保留取消 cleanup。恢复原状态后有一次
`pthread_testcancel()`（仅原状态 ENABLE），挂起的延迟取消先触发，cleanup 释放尚未交出的实例。
文件/协调文件等初始化副作用不撤销；不能据此假设文件系统事务回滚。

已启用 ASYNCHRONOUS 取消的直接构造返回 NULL/ENOTSUP，不读取后端配置或创建实例/后端资源；
进程身份防护的注册可能已发生，因此不承诺完全没有运行时副作用。
原因是裸 C 指针从 callee 返回到宿主赋值没有可对异步取消原子化的交接点。宿主可以先禁用取消，
建立自己的所有权和 cleanup 后再恢复。原来已经 DISABLE 的策略不被库擅自启用。
Global/Audit 在其外层已禁用取消的生命周期内调用 constructor，行为仍合法。

宿主对于成功返回的指针要负责后续线程取消的清理。例如，在已满足独占销毁前提时：

```c
static void release_owned(void *opaque)
{
    logger_t **slot = opaque;
    logger_t *owned = *slot;
    *slot = NULL; /* 先从宿主 cleanup 所有权中移除，避免取消后再次 destroy */
    (void)logger_destroy_status(owned);
}

/* 默认 deferred cancellation；真实项目仍要在这里处理构造/输出错误。 */
logger_t *owned = NULL;
pthread_cleanup_push(release_owned, &owned);
owned = logger_create(&cfg);
if (owned) {
    LOGGER_INFO(owned, "host", "work");
    /* 停止并 join 所有其他借用者之后，才有独占销毁权。 */
    release_owned(&owned);
}
pthread_cleanup_pop(0);
```

如果最终处置在恢复取消状态时触发宿主 cleanup，原实例可能已经释放；不要重复销毁。
NULL destroy 不注册新运行时。ECHILD / EDEADLK 是处置前拒绝；正常处置末尾的 I/O 失败仍释放实例。
不允许多个 owner 同时 destroy，不允许把 get_state(STOPPED) 当成旧指针可再次使用的证据。

## 重入契约

同一线程已在 Logger/Console 内部执行时，从其格式化、后端、FILE 回调或拦截器中再次调用
资源 API，返回 EDEADLK（void 入口不输出并设置 errno；查询返回约定的零/停止值）。
这是保守的跨实例规则：即使嵌套目标是另一个 Logger，也不支持在内部回调中获取另一套资源。
它不阻止正常先后使用 A/B，不阻止不同线程使用不同或同一个活跃实例，也不妨碍 SDK → 宿主日志
适配器 → Logger 这一普通顶层调用；SDK 回调尚未执行在 Logger 内部。

Worker 也设置本线程标记，避免 callback → flush 等待自身完成、callback → destroy join 自身。
Global 入口及 Audit 的锁准入在该标记下先拒绝，防止对内部 Logger 的回调反向取得生命周期锁。
这不是通用死锁检测：跨线程 callback 等待环、宿主持有 flockfile 再逆序调用 Console 等不受保证。
纯字符串 redaction helper 不是资源 scope，不带隐式递归检测。

## Console 配置与错误

verbosity/color 使用一个原子打包值。整次 console_init 一次提交；setter CAS 只改自己的字段；
每次输出捕获一次配置。并发整次 init 不会拼出一个从未配置过的 verbosity/color 组合。

- console_print 仍是 stdout、无标签、不自动追加换行；诊断仍到 stderr。
- 所有 fmt=NULL 返回 EINVAL，包括被级别过滤的消息。
- 非法配置/枚举不改变已有配置，void setter/init 通过 errno 报告 EINVAL。
- stdio 输出失败后即使后续 fflush 也失败，保留第一次错误。
- console_debug_source 消息 3072 字节（含 NUL）、合成字段 4096 字节（含 NUL）；超限返回
  EOVERFLOW，在写出诊断前拒绝，而不静默截断。
- TTY 自动探测不覆盖供 printf `%m` 使用的 errno；成功操作恢复入口 errno。
- 配置或输出提交后收到取消，不做回滚。

TLS context set/get 的自赋值与 request/session 等交换使用临时快照，避免输入仍指向 TLS 时
边读边覆盖。TLS/context 生命周期和异步 source 复制容量没有改变。

## 验证与生产范围

回归通过链接器 --wrap 安排真实资源持有窗口，另用无 wrapper 的 fopencookie 宿主回调测试实际
共享产物。wrapper 不编译进生产库，未增加环境控制故障点或进程管理功能。
详见 validation/EXPLICIT_CONSOLE_RESULTS.md；新增测试通过不代表已验收所有部署平台和存储掉电。
