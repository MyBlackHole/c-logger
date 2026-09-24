# 引用计数使用规范

c-logger 当前没有为 `logger_t`、Audit runtime、queue storage、worker state 或
global logger 引入通用 per-object refcount。

这是有意设计：refcount 只是生命周期机制之一，不是默认 ownership 模型。

## 什么时候应该使用 refcount

只有同时满足以下条件时才考虑引用计数：

1. 多个独立 owner 都可能延长同一个对象的生命周期；
2. 这些 owner 可以逃逸出获得指针时的 lexical scope 或 lock；
3. 没有更简单的 owner/join/lifetime-pin 协议能确定“最后一个使用者”；
4. 对象确实需要一直存活到最后一个独立 owner 释放。

如果一个 structural owner 可以 stop/join 全部使用者，或者 rwlock/pin 已经保证借用期间
对象不会被销毁，则通常不需要再叠加 refcount。

## REFCOUNTED 与 SHARED

- **SHARED**：存在多个使用者，但生命周期由 lock pin、join、generation gate 或
  owner/borrow contract 管理。
- **REFCOUNTED**：每个独立生命周期 owner 都持有一个 counted reference，降到 0 时执行
  final release。

SHARED 资源不自动等于 REFCOUNTED。

当前保持非 refcount 的例子：

- explicit `logger_t *`：host owns，SDK/caller borrow；
- global logger：generation/admission + lifetime rwlock pin；
- worker：logger owner + cooperative stop + join；
- queue/workspace：单一 structural owner + join-before-free；
- `live_objects`：只是 process census/gate，不控制单个对象生命周期。

## 如果未来引入 refcount，必须满足的语义

### 初始 reference

creator 创建对象时拥有且仅拥有一个初始 reference。

publish pointer 本身不能偷偷制造新的 reference。

### get 前必须证明对象仍存活

普通 `get()` 只能在已有 lifetime guarantee 下执行，例如：

- 调用者已经拥有一个 reference；
- mutex/rwlock 在 lookup 期间阻止 final release；
- 专门的 `try_get` 能在 count 已经为 0 时原子失败。

仅仅先 atomic load 一个 raw pointer，再执行 count++ 是不够的：最后一个 put/free
可能发生在两步之间。

### put 消费一个 ownership

`put()` 精确消费一个 owned reference。

观察到 count 从 1 变 0 的那个 put 负责 final release。put 之后该 ownership 已经消失，
调用者不能继续依赖这个 reference 使用对象。

### 0 是终态

count 到 0 后禁止 resurrection。

final release 可能销毁 mutex、关闭 fd、释放内存，因此即使物理内存尚未复用，
对象也已经逻辑死亡。

### overflow / underflow 属于 bug

禁止直接用裸 `_Atomic unsigned` + 任意 fetch_add/fetch_sub 构造 lifetime refcount。

未来如果需要 refcount abstraction，必须通过窄 API 防止或检测：

- overflow；
- underflow；
- double put；
- resurrection。

### final release 必须唯一

概念接口应类似：

```c
object_t *object_get(object_t *obj);
bool object_try_get(object_t *obj);
void object_put(object_t *obj); /* 0 -> object_release(obj) */
```

仍有 counted reference 时，调用者不能直接调用 `object_release()`。

## 与 lexical cleanup 的关系

一个 counted reference 本身可以是 lexical OWNED resource，例如 scope 退出自动调用
`put()`。

这只代表自动 cleanup 管理“一个 reference”，并不代表它管理整个 shared object 的
生命周期。

## 引入 refcount 前的 review checklist

1. 为什么 single ownership 不够？
2. 为什么 join 或 lifetime lock/pin 不能定义最后一个使用者？
3. initial reference 在哪里创建？
4. 在什么保护下允许获取新 reference？
5. lookup 是否可能与 final put 竞争？
6. 每个 reference 在哪里被消费？
7. count 到 0 后由哪个唯一函数 release？
8. 如何防止 overflow、underflow、double put、resurrection？
9. 是否给 hot path 增加 atomic RMW？这个成本是否真的必要？

在出现真正需要这些语义的对象之前，不要预先加入无人使用的通用 refcount 实现。
