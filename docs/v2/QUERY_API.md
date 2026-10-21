# 状态、指标与诊断查询合同

本增量迁移十二个现有查询入口。所有入口返回 `0/-errno`，所有正常返回保留
调用方 `errno`；只有完整成功才写输出槽。状态和计数不再与错误共用返回值。
这是 ABI 2 的语义变更：六个旧 `void` 指标入口的失败清零规则被替换，
旧签名和旧 `-1+errno` 约定不保留。公开结构布局及四十三个导出名称不变。

## 接口与保护范围

| 入口 | 输出槽类型 | 读取保护 |
|---|---|---|
| `logger_get_state` | `logger_state_t *` | 原子状态 |
| `logger_dropped` | `uint64_t *` | 原子累计计数 |
| `logger_get_metrics` | `logger_metrics_t *` | 原子与不可变字段 |
| `logger_get_io_metrics` | `logger_io_metrics_t *` | 原子计数与错误 |
| `logger_get_diagnostics` | `logger_diagnostics_t *` | 原子、不可变字段及队列观测 |
| `logger_get_file_metrics` | `logger_file_metrics_t *` | 显式作用域与 `emit_mu` |
| `logger_get_syslog_metrics` | `logger_syslog_metrics_t *` | 显式作用域与 `emit_mu` |
| `logger_global_dropped` | `uint64_t *` | 默认代次借用与原子计数 |
| `logger_get_global_metrics` | `logger_metrics_t *` | 默认代次借用与原子观测 |
| `logger_get_global_io_metrics` | `logger_io_metrics_t *` | 默认代次借用与原子观测 |
| `logger_get_global_diagnostics` | `logger_diagnostics_t *` | 默认代次借用与原子观测 |
| `logger_get_global_file_metrics` | `logger_file_metrics_t *` | 默认代次借用与 `emit_mu` |

三个原标量查询现在为：

```c
int logger_get_state(const logger_t *log, logger_state_t *out);
int logger_dropped(const logger_t *log, uint64_t *out);
int logger_global_dropped(uint64_t *out);
```

其余查询保留原参数列表，将结果统一放在已有的输出槽。没有新增默认 Syslog
查询、兼容别名、结构版本字段或大小参数。级别设置、TLS 上下文和脱敏辅助函数
的剩余返回形式继续单独迁移，不能据本增量宣称所有公开 API 已统一。

## 准入与输出发布

进程身份、同线程重入和进程域就绪检查先于任何实例解引用。未重建子方返回
`-ECHILD`，重入为 `-EDEADLK`，冷状态为 `-ENODEV`，初始化中为 `-EAGAIN`，
未发布的失败域返回保存错误。普通查询不隐式建域，也不等待初始化者。

准入成功后，空实例或空输出槽为 `-EINVAL`；未选择相应后端为 `-ENOTSUP`。
默认入口先建立外层准入，再检查输出槽及借用默认代次。有效输出槽必须在整个
调用期间可写并由调用者独占，不能与实例内部存储重叠或并发修改。

任何负返回均保持输出槽的全部原值，包括已经读取快照之后的解锁或取消恢复
失败。局部快照在最后一层收尾成功后才交付。恢复取消可能执行宿主清理函数，
因此收尾后只能复制局部数据，不能再解引用 Logger。函数因取消而未正常返回时，
不能把未交付的局部快照当成可用结果。

```c
logger_diagnostics_t snapshot;
int rc = logger_get_diagnostics(owner, &snapshot);
if (rc)
        return rc;
/* 只有 rc == 0 时使用本次 snapshot；errno 不携带本次状态。 */
consume_diagnostics(&snapshot);
```

原调用方依赖失败清零来隐藏旧值时，必须改为检查返回状态。业务若需要显示
“无数据”，由业务在失败分支明确选择；库不会把查询失败伪装为零计数或停止状态。

## 观测与生命周期

五个无锁显式查询不增加锁、内存分配、I/O 或取消策略切换。跨字段值来自并发
近似快照，累计计数的含义、队列水位和统计精度保持不变；返回成功不代表整个
实例在同一个瞬间满足全部字段组合，也不提供逐记录接受回执。

状态和指标允许读取仍然存活的 `STOPPING`、`STOPPED` 或故障实例。不能机械
套用记录入口的 `RUNNING` 限制，否则故障状态无法诊断。调用方仍须保证实例
存活，不能与销毁并发，不能在已消费拥有者、fork 继承失效或卸载之后查询。
输出里的 `STOPPED` 不是可以重新销毁旧指针的证明。

File/Syslog 查询在既有 `emit_mu` 下读取一致的后端字段。它们不执行 write、
writev、fsync、轮转、重开、连接或发送，也不清除粘滞错误。锁可能阻塞，查询
不提供墙钟超时。获取锁失败不尝试释放未取得的锁；释放证明失败记录同步错误，
拒绝发布快照。已有实例同步错误继续拒绝需要该锁的后端查询。

## 默认代次与错误优先级

默认查询固定一个代次：读取票据、取得生命周期读锁、复核票据，再读取实例。
空槽为 `-ENODEV`，绑定转换中为 `-EAGAIN`，排队之后代次失效为 `-ESHUTDOWN`；
不会转投新代次，不会借查询创建实例。输出发布必须等待最外层借用归还与取消
收尾成功，不能仅以内层 getter 成功为依据。

普通显式作用域保留主操作错误；主操作成功才返回取消恢复错误。默认生命周期
锁释放证明失败遵守既有安全优先级：返回该安全错误、毒化默认槽，并保守保留
未证明归还的借用。这可能覆盖先前的后端错误，详见[控制合同](CONTROL_API.md)。
本次迁移不改变共享的默认锁顺序、失败关闭或所有权协议。

## 验证边界

`query-api` 分组包含真实生产静态/共享的公开入口测试，以及同源故障驱动分别
链接支持静态归档和真实生产静态归档。GNU `--wrap` 不作为 DSO 内部拦截证明。
公开用例检查十二个入口的精确返回、完整输出、errno、空参数、冷状态与子方；
真实 File/Syslog 计数与实际记录相互核对。

故障驱动覆盖取消设置与恢复、emit 锁获取与释放、默认读锁获取与释放、主错与
收尾错误优先级，以及最终失败保持完整槽。无锁显式查询还拒绝新增取消操作和
后端 I/O。依赖真实 Unix 套接字的场景单列，环境拒绝不能记作测试成功。

旧冷状态、初始化中、失败域、重入、默认代次与 fork 探针迁移实际状态断言，
保留“不格式化、不碰继承锁、不建域”的见证。C11/C++11 类型正反编译和安装
消费者实际执行新签名；符号数量相同不能替代函数类型验证。具体运行结果绑定
准确提交、源码树、工具链与原始日志，见[测试入口](../../TESTING.md)。
