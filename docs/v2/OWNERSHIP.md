# c-logger 2.0 所有权与生命周期设计

状态：**#102 设计基线，已按当前 Audit-free ABI 2 对象图更新**。

当前设计入口见 [README.md](README.md)。Audit/crypto 已退役，不属于本文的对象或验证范围；迁移边界见 [AUDIT_REMOVAL.md](AUDIT_REMOVAL.md)。

本文记录当前实现的所有权语言、对象图和选择生命周期 primitive 的条件。当前保留 Global facade，
显式实例采用 host-owned borrow/join 模型；public API/ABI 最终冻结仍需 #104/#108 审查，
未来提案不改变这里记录的现行调用与销毁合同。

核心原则：

> 先证明谁拥有对象、谁只是借用、谁负责停止执行者，再选择 refcount、锁或其他 primitive。

---

## 1. 所有权分类

统一使用以下分类：

```text
AUTO
OWNED
BORROWED
MOVED
SHARED
REFCOUNTED
```

### AUTO

词法作用域内唯一 owner。适合临时 fd、heap candidate、临时 FILE、尚未 publish 的替代资源，可以使用 `__free` / cleanup helper。

### OWNED

结构性唯一 owner。owner 决定资源最终释放时间。

例如：

- `logger_t` 结构性拥有 queue/backend/workspace；
- queue 结构性拥有 slots/spill pool。

### BORROWED

只能在明确的 lifetime proof 覆盖期间访问。

BORROWED 不延长对象生命周期，不能执行 final release，不能逃逸其 lifetime scope，也不能因为“有一把锁”就假设自己拥有对象。

### MOVED

ownership 已从 source 转移到 destination。move 后 source 必须进入无资源状态，禁止重复 release。

### SHARED

描述多个执行者可以访问同一对象，但**不自动意味着 refcount**。

共享访问必须单独说明结构 owner、reader lifetime proof，以及 teardown 如何等待共享访问结束。

### REFCOUNTED

只有多个**独立 owner**都可以延长对象内存生命周期时才使用。

refcount 只证明“对象内存当前仍有 owner”，它不证明对象仍处于 RUNNING、backend 可用、worker 已停止、close 已完成，或当前线程允许执行 blocking teardown。

---

## 2. 不可替代关系

始终成立：

```text
lock != lifetime pin != join != refcount != atomic

refcount != admission state
refcount != execution quiescence
close != final free
queue empty != backend completion
atomic != ownership
```

---

## 3. 当前 2.0 基线判断：logger_t 不预先改成 REFCOUNTED

当前代码中的实际关系是：

```text
Host / Global controller
        |
        | OWNED
        v
     logger_t
        |
        +---- OWNED ---- file backend
        |
        +---- OWNED ---- syslog backend
        |
        +---- OWNED ---- queue
        |                   |
        |                   +---- OWNED ---- slots
        |                   |
        |                   +---- OWNED ---- spill pool
        |
        +---- OWNED ---- worker workspace
        |
        +---- OWNED ---- pthread worker lifetime
                             |
                             +---- BORROWED ---- logger_t
```

worker 是 logger 的**从属执行者**，不是独立 owner。

因此 #102 的基线决定是：

> **不因为 2.0 引入 refcount primitive 就把 logger_t 自动改成 refcounted。**

如果 #104 后续证明 SDK、插件或其他对象确实具有“可以独立于 Host 延长 logger 生命周期”的需求，再重新打开该决定。

### 为什么不做 per-log refcount

禁止把：

```text
logger_log()
    get
    ...
    put
```

作为默认安全方案。

原因：

- 每条日志增加 atomic RMW；
- 仍不能解决“裸指针已经被并发 free 后才执行 get”的问题；
- 混淆 caller ownership 与 API 调用同步；
- shutdown/final put 执行上下文更难证明。

如果未来采用 refcount，应优先是**长生命周期 reference**：

```text
SDK instance create  -> get
SDK lifetime         -> borrow
SDK instance destroy -> put
```

而不是每次日志调用 get/put。

---

## 4. refcount 的严格引入条件

只有同时满足以下条件才允许一个对象变成 REFCOUNTED：

1. 存在两个或更多独立 owner；
2. 任一 owner 都可能比创建者活得更久；
3. owner 之间不能通过更简单的结构化 join/parent lifetime 管理；
4. final release 的执行上下文可以明确证明安全；
5. refcount 成本和泄漏风险小于替代方案复杂度。

如果这些条件不成立，优先：

```text
单一结构 owner + BORROWED users + explicit quiescence
```

### refcount 本身的安全规则

未来若实现 `logger_refcount_t`，必须满足：

- 0 是 terminal state；
- 禁止 0 -> 1 resurrection；
- overflow 不得 wrap；
- underflow 不得 wrap；
- final release 只能发生一次；
- overflow/underflow/invariant failure 不得转化成错误 free；
- proof 失败时 retain/leak 优先于 UAF。

特别注意：

> `inc_not_zero(ptr->ref)` 只有在调用者已经有办法证明 `ptr` 本身仍可安全解引用时才合法。

它不能让一个可能已经被其他线程 `free(ptr)` 的裸指针重新变安全。

---

## 5. Logger 的 BORROWED 调用模型

当前基线：

```text
owner create
    ↓
publish pointer to subordinate users
    ↓
users BORROW while owner guarantees lifetime
    ↓
owner closes admission to those users
    ↓
owner stop/join external borrowers
    ↓
logger teardown
```

因此：

- Logger 内部不能假装知道宿主所有外部线程；
- public destroy 不能“扫描线程”证明无 caller；
- SDK/插件若只是 BORROWED，Host 必须先停止并 join 它们；
- 如果未来 SDK 成为真正独立 owner，应显式升级为 get/put contract，而不是继续写“borrow”。

#104 可以改变 public 对象模型，但必须显式更新这里的 ownership graph。

---

## 6. worker / queue / workspace

### worker

worker 持有：

```text
BORROWED logger_t *
BORROWED logger_queue_t *
BORROWED worker_workspace *
```

proof：

```text
logger final teardown
    ↓
running = 0
    ↓
wake worker
    ↓
pthread_join succeeds
    ↓
worker borrow ends
    ↓
workspace/queue may be destroyed
```

禁止 worker 对 parent logger 加 refcount 来替代 join，也禁止 join 失败后继续释放 worker 可访问内存。

### queue

`logger_t` 结构性拥有 `logger_queue_t`。queue 结构性拥有 slots、spill pool、wait mutex/condition variable。

销毁：

```text
no producers/consumer
    ↓
worker joined
    ↓
destroy wait synchronization
    ↓
free spills/slots
```

同步对象 destroy 失败属于 lifetime proof failure，不得继续 free queue storage。

### record ownership

异步 enqueue 后必须完成 source-lifetime isolation：

```text
caller data
   |
   | copy/capture
   v
queue-owned slot / spill
   |
   | consumer copy
   v
worker-owned batch workspace
   |                         consumer copy 完成后（仍在 try_pop 内）
   |                             -> 归还 spill block
   |                             -> slot.seq release，slot 可复用
   |                             -> 推进 dequeue_pos
   |
   | backend output attempt（成功或失败）
   v
async_completed / completion watermark
```

`logger_queue_try_pop()` 在复制完整 record 后即归还 spill/slot；worker 随后只访问自己的 batch 副本。
因此 queue storage reuse 与 backend completion 是两个不同事件。`logger_worker_main()` 在
`emit_batch()` 返回后推进完成计数，并在同步协议正常时于 `progress_mu` 下推进 `completed_pos`。
后端 I/O 失败也会完成该次尝试并记录错误；completion 不承诺成功或 durable fsync。
flush 等待 reservation watermark，不能用 queue empty、dequeue 或 slot/spill 可复用替代。

queue 中不能保存 caller 的可变裸指针。

---

## 7. backend ownership

### File

`logger_file_t` 是可 move 的单 owner 资源。

```text
AUTO fd candidate
    ↓ validate
MOVED to logger_file_t
```

reopen/rotation：

```text
prepare candidate
    ↓
validate candidate
    ↓
sync/commit prerequisite
    ↓
install new owner
    ↓
retire old fd
```

简单 reopen 的 candidate 失败保留仍可用的 old owner。rotation 成功 rename 后，旧 fd 已属于 archive；
后续 active 创建/同步失败进入 detached 状态，不能继续写旧 archive。两条路径不能合并为“轮转失败永不改变旧状态”。

### Syslog

`logger_syslog_t` 由 logger 唯一拥有。socket replacement/reconnect candidate 失败时丢弃 candidate，不能把失败 socket 伪装成有效 owner。

backend 不独立延长 `logger_t` 生命周期。

---

## 8. Global ownership

当前实现：

```text
Global controller
    |
    | OWNED
    v
  g_logger
    ^
    |
BORROWED readers
    |
g_lifetime_lock read-side pin
```

`g_lifetime_lock` 是 lifetime pin，不是 refcount。

shutdown：

```text
close admission
    ↓
take lifetime writer lock
    ↓
wait old readers drain
    ↓
g_logger = NULL
    ↓
release lifetime lock
    ↓
destroy old logger while control remains exclusive
```

当前 Global facade 使用上述协议。#104 若将来提出替换或删除，必须作为新对象模型决策重新审查，不能把未来选项当作现行行为。

禁止仅因为已有 refcount 就替换成裸 atomic pointer + `inc_not_zero`；这存在“先 free、后访问 refcount 字段”的根本竞态。

---

## 9. process census 不是 refcount

`live_objects` 的语义是 process/fork lifecycle census，不是 `logger_t` reference count。

因此：

- count -> 0 不触发 logger release；
- acquire 不产生 logger owner；
- release 不代表对象 final put；
- overflow/underflow 是 process-lifecycle invariant failure。

禁止把 census 与对象 refcount 合并。

---

## 10. 构造失败 rollback 必须使用同一套 lifetime proof

当前构造回滚已遵守以下边界：

> **construct rollback 与 normal destroy 具有同样严格的 proof-before-free 要求。**

不能因为对象“尚未 publish”就认为所有 synchronization destroy 必然成功，然后无条件 `free(container)`。

构造过程必须记录已经成功建立的资源，例如：

```text
ALLOCATED
EMIT_MUTEX_READY
PROGRESS_MUTEX_READY
PROGRESS_COND_READY
FILE_READY
SYSLOG_READY
QUEUE_READY
WORKSPACE_READY
WORKER_STARTED
PUBLISHED
```

rollback 只释放确定已经建立的资源，并按依赖顺序先停止/join 执行者，再释放其可达资源；不要求互相独立的资源机械地按文本逆序释放。

如果以下 proof 失败：

- worker join；
- queue synchronization destroy；
- progress/emit synchronization destroy；
- process census release；

则：

```text
do not free container
do not free memory still reachable by uncertain users
record invariant/lifecycle failure
return original construction failure when it remains primary
```

cleanup failure 的优先级必须单独定义，不能覆盖 primary construction error 后让 caller 误判根因。

未 publish 的对象没有合法外部 borrower，但同步对象 destroy 失败仍代表内部 invariant 无法证明。

2.0 继续坚持：

> proof failure -> safe retain/leak，不能为了“构造失败必须无泄漏”冒 UAF 风险。

---

## 11. cancellation ownership handoff

成功构造但尚未把 ownership 返回 caller 时，library 仍是 OWNED。只有函数真正返回指针后 ownership 才转移给 caller。

pending cancellation 必须在 ownership handoff 前解决。

恢复 cancellation 前：

- library locks 已释放；
- lifetime pin 已释放；
- lexical resources 已清理。

cancellation 不等价于 transaction rollback。

---

## 12. fork / dlclose

fork child 不获得一个“可继续使用”的 logger owner。继承的 pthread 状态、fd 和对象内存保持不可安全复用状态，直到 exec/_exit。当前没有 fork reinit 或继承 runtime 的原地重建接口。

不能通过重置 mutex、重置 refcount 或修改 owner pid 伪造安全的新生命周期。

任何可能执行 logger 代码的 borrower/worker/callback 必须在 dlclose 前退出。refcount 只能保护 heap object，不能保护已经被卸载的 DSO text。

---

## 13. 2.0 ownership matrix

| 资源 | 创建者 | 结构 owner | borrower | move 点 | quiescence proof | final release |
|---|---|---|---|---|---|---|
| logger_t | create/controller | Host 或 Global controller | public caller / worker | API handoff | caller protocol + worker join | logger teardown |
| worker | logger_t | logger_t | 无独立 owner | 无 | pthread_join | logger teardown |
| workspace | logger_t | logger_t | worker | 无 | worker joined | workspace destroy |
| queue | logger_t | logger_t | producer/worker | 无 | producer quiescence + worker joined | queue destroy |
| queue slot/spill | queue | queue | producer/consumer 临时访问 | enqueue/consume protocol | slot generation protocol | queue destroy |
| file backend | constructor | logger_t | emit path | candidate install | emit path quiescent | file close |
| syslog backend | constructor | logger_t | emit path | candidate install | emit path quiescent | socket close |
| global logger | logger_init | Global controller | global calls | candidate publish | lifetime readers drained | global shutdown |
| process census | process layer | process layer | fork gate | 无 | checked decrement | 非对象释放机制 |

#102 后续实现或 #104 对象模型改变时必须同步维护该表。

---

## 14. 第一阶段设计决定

当前冻结：

1. `logger_t` **不预先变成 REFCOUNTED**；
2. worker 不持有 parent refcount；
3. queue/backend/workspace 保持结构性 OWNED；
4. Global 的 lifetime pin 可在 #104 重做，但不能用裸 pointer + refcount CAS 简化；
5. process census 永远不等价于 object refcount；
6. constructor rollback 已采用 proof-before-free；
7. refcount primitive 只有真实对象选择 REFCOUNTED 后才进入 production；禁止为了完成 checklist 引入未使用基础设施。

尚未冻结：

- 2.0 public Logger API 是否提供 long-lived get/put；
- Global facade 未来是否被新对象模型替换（当前保留）；
- 最终冻结时是否调整现有 Logger close/destroy/public status API；
- 哪个未来对象第一个真正需要 REFCOUNTED。

这些未来冻结问题进入 #104/#108 决策；在新决策落地前，当前 public API 与以上生命周期防护持续适用。
