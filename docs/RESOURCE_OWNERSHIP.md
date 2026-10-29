# 资源所有权规范

> 当前创建/销毁以[生命周期合同](v2/LIFECYCLE_API.md)为准：直接使用拥有者槽与负错误码，不保留旧接口。
> [默认槽只借用宿主实例](v2/DEFAULT_BORROW.md)，绑定和解绑都不转移实例所有权。

资源所有权是 c-logger 生命周期模型的第一层。自动清理、加锁和原子操作
都只是实现工具，它们本身不能回答“最终由谁负责释放资源”。

对每一个非平凡资源，审查必须先回答四个问题：

1. 谁创建或获取资源？
2. 当前谁拥有资源？
3. 所有权在哪一个明确操作上发生转移？
4. 最终由谁释放？

任何一个问题无法明确回答，都说明设计还没有完成。

<a id="ownership-状态"></a>

## 所有权状态

c-logger 使用五种所有权/生命周期状态：
`OWNED`、`BORROWED`、`MOVED`、`SHARED`、`REFCOUNTED`。

<a id="owned"></a>

### 独占拥有（OWNED）

当前变量、对象或子系统负责最终释放资源。

示例：

- 使用 `__free(free)` 的局部指针；
- `logger_queue_init()` 成功后的 `struct logger_queue.slots`；
- 构造函数发布后的 `logger_t.worker_workspace`；
- 持有文件/目录/协调 fd 的 `struct logger_file`。

OWNED 资源允许移动，但禁止悄悄复制成第二个所有者。

<a id="borrowed"></a>

### 借用（BORROWED）

当前代码可以在有界生命周期内使用资源，但不能释放。

示例：

- SDK 从宿主借用的显式 `logger_t *`；
- 普通日志调用收到的 `logger_t *`；
- 在调用返回前已经复制内容的配置指针。

BORROWED 资源不能附加 `__free()`、CLASS 析构函数或其他隐式释放。

<a id="moved"></a>

### 已转移（MOVED）

所有权已经转移给新所有者，旧所有者必须在同一操作中失效。

典型形式：

```c
object->buffer = no_free_ptr(buffer);
return_ptr(object);
fd = take_fd(local_fd);
```

移动后源变量必须变成 `NULL`、`-1` 或等价空资源状态，禁止再次解引用或释放。

<a id="shared"></a>

### 共享访问（SHARED）

存在多个并发使用者，但生命周期由显式协议控制，而不是单一词法作用域所有者。

当前示例：

- 代次/准入 + 生命周期读写锁保护的默认实例借用；实例始终由宿主持有；
- 通过协作式停止 + join 结束生命周期的工作线程；
- 多生产者 / 单消费者共享的队列状态。

SHARED 不代表“没有所有者”。仍然必须有最终所有者/释放者，以及阻止使用期间被释放的协议。

<a id="refcounted"></a>

### 引用计数管理（REFCOUNTED）

多个独立所有者分别持有带计数的生命周期引用。`get` 获取一个引用，
`put` 消费一个引用，计数降到 0 的那个 `put` 执行最终释放。

REFCOUNTED 不等于 SHARED。工作线程 join、读写锁生命周期固定保护（pin）、宿主拥有 + 有界
借用者都属于 SHARED，但不需要引用计数。

当前 c-logger **没有生产环境 REFCOUNTED 对象**。引入前必须遵守
`REFCOUNTING.md`。

<a id="ownership-transfer"></a>

## 所有权转移

所有权转移必须在代码中可见，不能只靠普通指针赋值暗示。

推荐形式：

- 局部所有者 -> 对象字段：`field = no_free_ptr(local)`；
- 局部所有者 -> 调用方：`return_ptr(local)`；
- 被调用函数成功后消费所有权：`retain_and_null_ptr(local)`；
- POSIX fd 移动：`take_fd(fd)`。

对 `struct logger_file` 这类可移动结构体，移动后源必须立即恢复成定义好的空资源状态。

<a id="owner-与-releaser-必须成对"></a>

## 所有者与释放者必须成对

长期拥有资源的字段应在字段声明或构造函数/析构函数附近明确最终释放者。

```text
struct logger_queue.slots
  获取：logger_queue_init() 中 calloc()
  发布后所有者：struct logger_queue
  最终释放：logger_queue_destroy()

logger_t.worker_workspace
  获取：logger_worker_workspace_create()
  发布后所有者：logger_t
  最终释放：工作线程 join 后 logger_worker_workspace_destroy()

默认槽 g_logger
  绑定：logger_default_set(NULL, owner)
  实例所有者：宿主持有的 owner 槽；默认槽仅 BORROWED
  默认调用：代次/准入 + 生命周期读写锁保护每次借用
  归还默认借用：logger_default_set(owner, NULL) 解绑成功
  最终释放：宿主停止并等待其他借用者退出后 logger_destroy(&owner)
```

解绑等待默认入口的在途调用退出，不隐含队列排空、刷新或实例销毁。
默认绑定、退役读者或尚未证明归还的默认借用会保持 `default_borrowed` 标记；
此时销毁返回 `-EBUSY` 并保留拥有者槽，不能强行释放。
该标记不是引用计数，清零也不证明其他显式借用者已经退出。

<a id="显式-logger-的-public-ownership"></a>

## 显式 Logger 的公开所有权

公开显式实例模型保持宿主拥有：

```text
宿主创建 logger_t
    |
    +-- SDK / 调用方借用
    |
宿主停止并 join 所有借用者
    |
若实例仍绑定默认槽，宿主成功解绑并归还默认借用
    |
宿主显式刷新（如需）并 logger_destroy(&owner)
```

任何内部清理辅助函数都不能把原始借用 `logger_t *` 自动变成跨线程安全引用。
并发销毁仍属于调用者违反生命周期契约。

创建成功才向空拥有者槽发布实例；销毁开始前消费拥有者并清空槽。
入口拒绝（包括默认借用尚未归还）保留原槽；进入拆除后，即使 I/O 失败也不恢复旧指针。
无法证明线程退出或安全回收时，库会保守保留相关分配；槽为空不等于物理释放完成，
其他旧别名也不能用于重试。具体状态与取消边界以[生命周期合同](v2/LIFECYCLE_API.md)
和[默认借用合同](v2/DEFAULT_BORROW.md)为准。

<a id="review-checklist"></a>

## 审查清单

| 问题 | 必须提供的证据 |
|---|---|
| 谁创建/获取 | 明确函数或表达式 |
| 当前所有者 | 词法作用域变量、结构体、子系统或共享协议 |
| 转移点 | 明确语句/API，并使源失效 |
| 最终释放者 | 明确销毁/关闭/join/ref-drop 路径 |
| 借用者 | 为什么借用者生命周期内不会与释放竞争 |
| 释放失败 | 失败是否属于可观察 API 语义 |
| 销毁依赖 | LIFO 或显式释放顺序是否正确 |
| 跨线程使用 | 明确生命周期协议 |
| 多个独立所有者 | 说明选择 SHARED 还是 REFCOUNTED |
| 新增引用 | 若 REFCOUNTED，证明 get/try_get 时对象仍存活 |

所有权定义清楚后，再参考 `RESOURCE_CLEANUP.md` 选择词法作用域清理；
需要带计数的生命周期时先参考 `REFCOUNTING.md`。
