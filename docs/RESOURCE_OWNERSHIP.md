# 资源所有权规范

资源 所有权 是 c-logger 生命周期模型的第一层。自动 清理、锁机制 和 原子变量
都只是实现工具，它们本身不能回答“最终由谁负责释放资源”。

对每一个非平凡资源，review 必须先回答四个问题：

1. 谁创建或获取资源？
2. 当前谁拥有资源？
3. 所有权 在哪一个明确操作上发生转移？
4. 最终由谁释放？

任何一个问题无法明确回答，都说明设计还没有完成。

## 所有权 状态

c-logger 使用五种 所有权/生命周期 状态：
`OWNED`、`BORROWED`、`MOVED`、`SHARED`、`REFCOUNTED`。

### OWNED

当前变量、对象或 subsystem 负责最终释放资源。

示例：

- 使用 `__free(free)` 的局部指针；
- `logger_queue_init()` 成功后的 `logger_queue_t.slots`；
- 构造发布 后的 `logger_t.worker_workspace`；
- 持有 文件/目录/协调 fd 的 `logger_file_t`。

OWNED 资源允许 move，但禁止悄悄复制成第二个 owner。

### BORROWED

当前代码可以在有界生命周期内使用资源，但不能释放。

示例：

- SDK 从 host 借用的显式 `logger_t *`；
- 普通日志调用收到的 `logger_t *`；
- 在调用返回前已经复制内容的配置指针。

BORROWED 资源不能附加 `__free()`、CLASS destructor 或其他隐式 release。

### MOVED

所有权 已经转移给新 owner，旧 owner 必须在同一操作中失效。

典型形式：

```c
object->buffer = no_free_ptr(buffer);
return_ptr(object);
fd = take_fd(local_fd);
```

move 后源变量必须变成 `NULL`、`-1` 或等价 empty state，禁止再次解引用或释放。

### SHARED

存在多个并发使用者，但生命周期由显式协议控制，而不是单一 lexical owner。

当前示例：

- generation/admission + lifetime rwlock 保护的 global logger；
- 通过 cooperative stop + join 结束生命周期的 worker；
- 多 生产者 / 单 consumer 共享的 队列 state。

SHARED 不代表“没有 owner”。仍然必须有最终 owner/releaser，以及阻止使用期间被释放的协议。

### REFCOUNTED

多个独立 owner 分别持有 counted lifetime reference。`get` 获取一个 reference，
`put` 消费一个 reference，计数降到 0 的那个 `put` 执行 final release。

REFCOUNTED 不等于 SHARED。worker join、rwlock lifetime pin、host-owned + bounded
borrower 都属于 SHARED，但不需要 refcount。

当前 c-logger **没有 生产 REFCOUNTED 对象**。引入前必须遵守
`REFCOUNTING.md`。

## 所有权 transfer

所有权 transfer 必须在代码中可见，不能只靠普通指针赋值暗示。

推荐形式：

- local owner -> object field：`field = no_free_ptr(local)`；
- local owner -> caller：`return_ptr(local)`；
- callee 成功后消费 所有权：`retain_and_null_ptr(local)`；
- POSIX fd move：`take_fd(fd)`。

对 `logger_file_t` 这类 movable struct，move 后 source 必须立即恢复成定义好的 empty state。

## owner 与 releaser 必须成对

长期 owning 字段应在字段声明或 constructor/destructor 附近明确最终 releaser。

```text
logger_queue_t.slots
  acquire: logger_queue_init() 中 calloc()
  owner after publish: logger_queue_t
  final release: logger_queue_destroy()

logger_t.worker_workspace
  acquire: logger_worker_workspace_create()
  owner after publish: logger_t
  final release: worker join 后 logger_worker_workspace_destroy()

global g_logger
  acquire: logger_create()
  owner after publication: global lifecycle controller
  users: 只允许 lifetime-pinned BORROWED
  final release: logger_shutdown_status() / controlled teardown
```

## 显式 logger 的 public 所有权

public explicit-instance 模型保持 host-owned：

```text
host 创建 logger_t
    |
    +-- SDK / caller 借用
    |
host 停止并 join 所有 borrower
    |
host 销毁 logger_t
```

任何内部 清理 helper 都不能把 raw borrowed `logger_t *` 自动变成跨线程安全引用。
并发 destroy 仍属于调用者违反生命周期契约。

## review checklist

| 问题 | 必须提供的证据 |
|---|---|
| 谁创建/获取 | 明确函数或表达式 |
| 当前 owner | lexical variable、struct、subsystem 或 shared protocol |
| transfer point | 明确语句/API，并使 source 失效 |
| final releaser | 明确 destroy/close/join/ref-drop 路径 |
| borrower | 为什么 borrower 生命周期内不会与 release 竞争 |
| release failure | 失败是否属于可观察 API 语义 |
| teardown dependency | LIFO 或显式释放顺序是否正确 |
| cross-thread use | 明确 lifetime protocol |
| 多个独立 owner | 说明选择 SHARED 还是 REFCOUNTED |
| 新增 reference | 若 REFCOUNTED，证明 get/try_get 时对象仍存活 |

所有权 定义清楚后，再参考 `RESOURCE_CLEANUP.md` 选择 lexical 清理；
需要 counted lifetime 时先参考 `REFCOUNTING.md`。
