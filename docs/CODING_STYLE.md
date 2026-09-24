# 代码与注释规范

## 语言原则

c-logger 的源码注释、内部设计说明和工程文档以**中文为主**。

英文只在以下场景保留，以避免翻译造成技术歧义：

- C/POSIX/Linux API 和函数名，例如 `pthread_join`、`fdopen`；
- 代码标识符，例如 `emit_mu`、`g_lifetime_lock`；
- 标准内存序，例如 `memory_order_acquire`；
- ownership/lifetime 约定词，例如 `OWNED`、`BORROWED`、`MOVED`、
  `SHARED`、`REFCOUNTED`；
- 标准错误码，例如 `EINVAL`、`EPIPE`；
- Linux/POSIX 原生术语、协议名称和外部引用标题。

不要为了“全中文”把标准技术名词强行意译。例如应写：

```c
/* 通过 memory_order_release publish payload。 */
```

而不是创造新的中文术语替代 `memory_order_release`。

## 注释应该解释什么

优先解释代码无法直接表达的约束：

1. **ownership**：谁拥有资源、何时转移、最终谁释放；
2. **lifetime**：对象何时开始可见、何时允许销毁；
3. **locking**：哪把锁保护什么，固定 lock order 是什么；
4. **concurrency**：publish/consume、atomic memory order、join/pin/refcount 关系；
5. **error semantics**：哪些错误必须向上返回，哪些 cleanup error 可以忽略；
6. **durability**：fsync/rename/dir fsync 的顺序为什么不能变化；
7. **平台约束**：Linux/ELF、GNU C 扩展、fork/cancellation 等边界。

不要写只重复代码表面的注释，例如：

```c
i++; /* i 加 1 */
```

## ownership 注释格式

复杂资源建议在创建、转移和最终释放位置分别写清楚：

```c
/* ownership transfer：词法临时 owner -> logger_queue_t。
 * 最终由 logger_queue_destroy() 释放。 */
q->slots = no_free_ptr(slots);
```

长期持有字段应在结构体声明附近注明：

```c
/* create 成功后由 logger_t 拥有；worker join 后才能释放。 */
logger_worker_workspace_t *worker_workspace;
```

## 锁和并发注释

不能只写“thread-safe”。必须说明保护关系或协议，例如：

```c
/* q.wait_mu 只负责 sleep/wakeup，不负责 slot payload publication。 */
```

涉及 atomic 时，说明它承担的是 reservation、publication、completion 还是
lifetime；禁止用“原子所以线程安全”这种模糊表述。

使用 `guard()/ACQUIRE()` 前必须先确认 lock hierarchy。单锁、lexical scope、
unlock error 无业务语义的路径优先 guard；多锁、cancellation-aware、跨 scope
ownership 或错误承载型 unlock 保持显式。完整规则见 `LOCKING.md` 与
`LOCK_MATRIX.md`。

## 错误与 cleanup 注释

如果 destructor/close 的错误被故意忽略，要解释原因。

如果 `close`、`fsync`、`closedir`、rename 或持久化失败会改变 API 结果，
必须说明为什么保持显式 finalization，不能隐藏到自动 cleanup。

## 历史英文注释迁移

不要求为了翻译而一次性重写整个历史文件。

但从本规范生效后：

- 新增解释性注释必须以中文为主；
- 修改到某段已有代码时，该段相关英文解释性注释应同步中文化；
- API 名、变量名、标准术语继续保留英文；
- 历史文档可分批迁移，不能因翻译改变行为、ABI 或协议语义。

## 对外文档

面向当前项目开发者的内部文档默认中文。

如果未来需要英文发布文档，应单独维护英文版本或生成版本，不要把中英文逐句混写到
核心源码注释中。
