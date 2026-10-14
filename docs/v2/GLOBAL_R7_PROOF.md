# Global 候选与退役对象阶段证明（#199）

本文件记录当前 ABI 2 的控制器生命周期设计。控制互斥锁、生命周期读写锁、
工作线程 join 与最终内存释放回执是四种不同的证明，不能互相替代。

## 状态、唯一所有权与线性化点

| 阶段 | 可达性与所有权 | 关键动作 |
|---|---|---|
| IDLE / STOPPED | 无已发布对象 | 可以初始化下一代，旧裸指针不得重用 |
| STARTING | 控制器独占未发布的 candidate；内部 worker 可能已启动 | 创建候选；在生命周期写锁下完成发布或退役 |
| RUNNING | Global 读者仅在生命周期读锁期间借用；控制器保留结构所有权 | 对外提供当前代次的实例 |
| STOPPING | 关闭读者准入；写锁排空已有读者；撤销发布转为控制器独占退役 | 解锁证明成功后才销毁 |
| BROKEN | 释放证明不成立时保留对象/锁 | 阻止后续所有新代次和全局访问 |

1. 初始化持有 g_control_mu，通过 CAS 发布 G_STARTING；logger_create 返回的
   candidate 在整个阶段未写入 g_logger，合法的 Global 读者不能借用它。
2. 取得 g_lifetime_lock 写锁后，先设置 g_logger=candidate，再以 CAS 发布
   同一代次的 G_RUNNING。Global 读者先读取票据，获取生命周期读锁，
   然后重新读取并比较完整票据；不同代次不能交叉借用。
3. 候选写锁获取失败，或发布 CAS 失败，必须仍由控制器退役候选。
   发布 CAS 失败时保持写锁先撤销 g_logger；不能先让候选暴露再 free。
4. shutdown 首先将 RUNNING/IDLE 的票据转换为 STOPPING 关闭准入，
   取得生命周期写锁后才表示历史读者排空，继而设置
   g_retired=old、g_logger=NULL。STOPPING 原子值自身不是静默期证明。
5. 写锁获取失败且未发生并发 poison 时，仅对仍匹配的 STOPPING 代次
   执行 CAS 回滚；旧实例未销毁，能够继续服务。错误路径不得覆盖 BROKEN。
6. 撤销发布后，写锁解锁必须成功，才能调用 logger_destroy_status。
   解锁错误可能意味着真实锁依然持有，此时既不能销毁，也不能恢复取消
   或错误地完成 STOPPED。
7. logger_destroy_receipt_read 是最终 free 是否发生的回执，
   不是从销毁 errno、live_objects 或同步对象 destroy 返回码推断。
   已 free（包括有收尾 I/O 错误）可清空 g_retired，未 free 则保留并
   标记 BROKEN，阻止重建。
8. 内部 worker 的退出需要实际 stop/wake/join；全局读者退出需要
   生命周期锁与准入重检，两者职责不能混为一谈。

锁类别跟踪器只检查已建模的锁顺序，不知道对象阶段和最终 free 的事实。

## 确定性生产库回归

- normal：两代正常初始化、关闭，恰好两次 logger_t final free
- candidate-writer-failure：候选写锁获取失败后实际退役释放，并可创建下一代
- candidate-retain：候选写锁失败且销毁无回执，旧对象保留，后续新代次被拒绝
- shutdown-writer-failure：关闭写锁失败后旧代次恢复，实例未提前释放
- shutdown-unlock-failure：已撤销发布但写锁解锁失败，禁止销毁、恢复取消和新代次
- shutdown-retain：撤销发布后销毁无回执，必须 retain/poison，不得 free
- shutdown-io-receipt：真实已 free 但收到合成 I/O 错误，可继续初始化下一代
- start-gate / stop-gate：已有读者排空与发布前拒绝
- proof-handoff writer / acquire-fail：读者失效与写者获取交错不能抹除 BROKEN

上述场景由 tests/regression/test_global_lifecycle.c 的 GNU ld 链接包装控制；
通过包装 free，仅当指针等于被捕获的 logger_t 才记录实际 free。
所有失效场景独立进程运行，绝不在仍被持有或所有权不确定的 pthread 锁上
做二次 destroy 或强行解锁。

tests/regression/run_global_r7.sh 直接链接当前检出构建的生产静态归档，
而非回归支持库；提供普通、ASan+UBSan 和 TSan 模式，记录 SOURCE_COMMIT、
实际编译器、受测归档及测试驱动 SHA-256，以及每个场景的最终结果。
Xmake 仍保留带锁跟踪的原回归支持库场景，以便两条验证链互补。

## 验证边界

注入的 EIO、EDEADLK 等不意味着有效的 POSIX 普通锁天然会返回这些值；
它们是检验不变量失败路径的受控刺激。测试和规范推理均不能独自覆盖
全部调度交错。#199 的关闭必须基于精确源码、真实 CI 和检查器证据；
不代表 #103/#104/#108 全阶段已经完成，也不授权创建正式发布标签。
