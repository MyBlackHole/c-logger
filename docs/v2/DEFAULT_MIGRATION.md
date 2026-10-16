# 默认实例迁移与测试责任映射

默认槽由“库拥有并构造/销毁实例”改为“借用宿主创建的实例”。本次不保留旧API
包装层。`tests/regression/default_fixture.h` 只是测试宿主的两步组合：其显式
owner由具体调用方保存，不安装、不导出，不成为生产实现。生产 `logger_global.c`
不再调用 create/destroy。测试不能把解绑成功冒充最终释放或持久化。

## 原场景逐族对应

| 旧场景/机制 | 当前承担责任的测试 | 迁移原因与保留断言 |
|---|---|---|
| global_generation 的十个入口 | `test_global_lifecycle.c` 的十个 generation | 读锁前暂停；替换后销毁旧拥有者，再放行延迟读者，必须ESHUTDOWN；新实例文件数量/内容不改变 |
| start-gate、stop-gate、closed-readers | `global_closed_gate`；`default_binding*/held-reader` | 绑定更新关门后八线程反复访问所有十类接口，必须EAGAIN且不再获取读锁；在途读者退出前不能解绑/销毁 |
| shutdown-generation、stop-during-start、double-stop | `stale-control`、`aba`、公开stress、`test_reinit` | 不再关停库内实例；控制请求固定完整代次，期望不匹配ESTALE；并发争用恰有一个首次绑定成功 |
| bootstrap-generation、bootstrap-stop | `test_stderr_sigpipe.c` 的unbound分支、message_truncation的无绑定分支、default_binding/cold | 自动stderr回退已删除；%n不执行、无字节输出、信号掩码/待处理SIGPIPE不变，不将“没有输出”冒充旧回退仍可用 |
| contract、child | `global_contract`、`default_binding_test/fork`、`test_fork_reinit` | 未绑定ENODEV；父方继续，子方重建后为空，显式创建绑定，不复用旧指针或重放父队列 |
| reentry write/reopen与旧create/destroy回调 | `default_binding*/reentry`、explicit_scope、global_cancel | 默认set重入EDEADLK；宿主的实例创建/销毁重入与取消留在实例专项，不能要求默认set再调用已不存在的内部构造 |
| acquire-error read/control/publish/stop | default_binding的read-error/control-error/writer-error | 原始获取错误不进入临界区；写锁失败恢复原绑定，候选仍可由宿主处理 |
| release-proof reader/writer/control | lifetime-unlock/control-unlock；default_publish的两个交接场景 | 锁真的释放后合成错误，延迟读者毒化不得被控制回滚/最终CAS覆盖；不从pthread成功推导借用归还 |
| publish-cas-poison | `default_publish_whitebox/publish-poison` | 用真实读者解锁故障和LOCKDEP=1断言暂停最终CAS前窗口；新候选不被发布、临时借用归还、原对象保守保留 |
| proof-handoff writer/acquire-fail | default_publish的writer-fail-poison（生产/支持各一份）及publish-poison | 验证在读者实际解锁后、错误回执到达前的交接；不得把旧阶段回滚覆盖新BROKEN |
| R7 normal/candidate-writer-failure/shutdown-writer-failure/shutdown-unlock-failure | default-binding及default_publish；生产`run_default_borrow.sh` | 当前默认层不再锁住未发布候选调用内部析构，旧R7对象角色不适用；新的借用标记/指针发布及解锁顺序实际验证 |
| R7 candidate-retain/shutdown-retain/shutdown-io-receipt、join/cond-destroy错误 | `lifecycle_owner_*`、`destroy_status_regression`、constructor_rollback、worker_startup/worker_failure | 属于普通实例所有者的处置，不是默认set；继续检查真实join、munmap、fd关闭、owner消费、保守保留和独立最终释放回执 |
| global_cancel write/queue/flush/flush-void/reopen/reader/control-wait/restore-policy | 同名现行场景 | 实际调用期间取消禁用，完成解锁以后交付取消，清理能够查询/记录，随后再用下一次绑定 |
| global_cancel create/create-fail/shutdown/bootstrap | bind/unbind；lifecycle_owner cancel-create/cancel-destroy；无绑定拒绝测试 | 默认层的创建/销毁和bootstrap已删除；宿主拥有对象的取消验证仍由实例专项执行，不能把旧场景名称保留而做空操作 |
| 文件重复初始化/全局关闭期间独占 | `test_file_global` repeat-no-effects/teardown；file_legacy_probe | 默认重复绑定不触发文件创建；解绑后文件仍由宿主对象持有，到真实destroy结束前同路径EBUSY |
| Global工作线程启动失败/执行器失败 | `test_worker_startup`、`test_worker_failure`的宿主组合 | 创建者拥有槽、包装器仅借用；保持等待者、预留空洞、错误水位、取消、实际最终释放与errno边界 |

旧机制退出不等于旧测试全部通过。完整注册项总数可能减少；上表说明责任移动或机制
删除的原因，不能用新旧数量相等证明覆盖。旧源码可从基线
`5d3366408d4e00e084e2691c6d4f55fc5b20e246` 追溯，不改写历史日志和冻结ABI1基准。

## 普通消费方

程序先显式初始化进程域，创建owner，再绑定默认槽；停止便利调用者后解绑，随后
停止其他显式借用者并销毁owner。动态加载的每个实际库副本维持自己的进程域。
本轮更新当前示例、基准、测试、格式化模拟后端与生产专项；构建不存在旧符号不是
充分验证，必须同时运行真实数据、失败与取消场景。
