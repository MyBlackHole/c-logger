# Lockless / Atomic Protocol Proof Template

任何新 lockless、atomic state machine、seqcount-like 或 RCU-like 方案在实现前复制本模板填写。

## 1. 目的

- 要消除/避免的锁或竞争是什么？
- benchmark/正确性问题证据是什么？

## 2. 参与者

```text
Thread/role A:
Thread/role B:
Thread/role C:
```

## 3. 共享对象

| 对象 | atomic/ordinary | writer | reader | lifetime owner |
|---|---|---|---|---|
| | | | | |

## 4. 状态机

列出所有状态和合法转换。

## 5. linearization points

每个操作必须指出唯一或可证明的 linearization point。

## 6. publication

```text
payload writes
  -> release operation
  -> acquire observation
  -> payload reads
```

逐条写明 atomic object 与 memory order。

## 7. happens-before

列出每条必要 HB edge；不能只写“用了 atomic”。

## 8. reclamation

- 谁决定对象不再可达？
- reader 如何退出？
- final free 之前的 proof 是什么？
- refcount/RCU/join/lock 分别承担什么，不承担什么？

## 9. ABA / wraparound

- generation/counter 是否会 wrap？
- 地址是否可能复用？
- CAS 是否可能把新对象误认成旧对象？

## 10. wakeup/progress

- blocking / lock-free / wait-free？
- lost wakeup 是否可能？
- starvation 是否允许？
- force wake/shutdown 怎么处理？

## 11. failure/retry

- CAS retry 是否重复 side effect？
- partial publication 如何处理？
- failure 是否可以安全回滚？

## 12. C11 legality

明确证明不存在普通对象的无同步并发读写。

## 13. verification

- unit/white-box；
- concurrency litmus；
- TSan；
- fault injection；
- stress；
- benchmark。

没有完整 proof 的 lockless 设计不得仅凭性能理由进入 production。
