# LOGGER_2.0 当前开发 ABI 合同

> 默认槽借用、刷新/重开和Console已迁移；本增量统一记录与状态/指标查询。
> 当前生产导出43个，仍为未发布的开发ABI，不承诺与旧开发签名兼容。


本版尚未正式发布。生命周期迁移不兼容旧开发签名，没有适配层或旧 status
导出；必须使用同一源码的头文件和库重新构建调用方。版本身份不代表发布审批。

## 权威与接口变化

Linux x86_64/ELF64 的生产导出由 `abi/logger-2.0.symbols` 精确列出，当前43个；
SONAME 为 `liblogger.so.2`，符号版本为 `LOGGER_2.0`。移除
`logger_destroy_status`，`logger_create` 和 `logger_destroy` 的类型变为：

```c
int logger_create(const logger_config_t *, logger_t **);
int logger_destroy(logger_t **);
```

它们返回0/-errno并保留调用方errno。创建只写空槽，销毁准入成功后先清槽再
不可回滚地消费所有权；NULL槽不证明物理释放成功，入口拒绝保留原拥有者。
详见[生命周期接口](../docs/v2/LIFECYCLE_API.md)。

刷新与重开现在为：

```c
int logger_flush(logger_t *);
int logger_reopen(logger_t *);
int logger_default_flush(void);
int logger_default_reopen(void);
```

四者采用0/-errno并保留errno，删除旧status/instance包装，不保留符号别名。
默认绑定 `logger_default_set`、TLS上下文及脱敏函数也采用该规则。配置布局不变，
级别设置仍单独迁移。脱敏函数类型不变，但错误状态改为负错误码并保留errno，
失败保持输出不变；新旧语义不能混用。当前合同见[API](../API.md)。
详细控制语义与错误优先级见[控制操作合同](../docs/v2/CONTROL_API.md)。

## Console 函数类型与返回值迁移

```c
int console_init(const console_config_t *);
int console_set_color(console_color_mode_t);
int console_set_verbosity(console_verbosity_t);
int console_stdout_is_tty(int *);
int console_stderr_is_tty(int *);
```

配置由void改为int；TTY由无参布尔查询改为状态加输出槽。七个输出函数的C类型
不变但错误语义变为负errno，十二个入口均保留errno。不增加别名或旧接口，当前
43个符号数量不变不能证明二进制兼容。函数类型和安装实际调用按本源码重新验收，
详细见[Console合同](../docs/v2/CONSOLE_API.md)。

## 记录与查询函数类型迁移

`logger_log`、`logger_log_source`和`logger_global_write`由void改为int，参数与
printf属性位置保持；`logger_log_sync_status`仍为int，补齐入口errno保留。四个
入口统一0/-errno，宏直接传回状态，普通截断与严格拒绝的不同合同保持。

十二个查询均返回int；三个原标量结果改用输出槽：

```c
int logger_get_state(const logger_t *, logger_state_t *);
int logger_dropped(const logger_t *, uint64_t *);
int logger_global_dropped(uint64_t *);
```

六个void结构查询改为int，三个后端查询从-1/errno改为0/-errno；所有查询正常
返回保留errno，完整成功才交付结果。六个旧入口的失败清零明确替换为原槽不变。
数据结构不扩张，符号名称/数量不变，不保留旧函数指针类型或调用方式。语义详见
[记录合同](../docs/v2/RECORD_API.md)与[查询合同](../docs/v2/QUERY_API.md)。

## 单一可执行断言

`tests/packaging/current_abi_contract.c` 精确检查全部类型大小、对齐、字段偏移、
枚举/配置数值、默认值和当前43个函数类型；`.cpp`包含同一份源，独立检查
C++11布局与真实调用。实际安装消费者调用新记录、查询及所有权接口，不只是编译声明。
本文不再复制整段测试源码和手工维护的源码哈希，避免修改函数后文档留下旧快照。
原先完整快照可从Git历史复查，不作为本次新运行证据。

同一ABI号只标识未发布2.0开发主线，不承诺本轮之前的开发二进制与新头可混用。
最终发布时必须固定源码、测试、安装包与符号清单；不能用标签代替验证，正式
标签和Release仍需独立授权。已发布v1资产与历史基准不改写。

## 类型概览

| 类型 | sizeof | alignof |
|---|---:|---:|
| `logger_config_t` | 144 | 8 |
| `logger_rotation_config_t` | 24 | 8 |
| `logger_source_t` | 32 | 8 |
| `logger_context_t` | 24 | 8 |
| `logger_metrics_t` | 88 | 8 |
| `logger_io_metrics_t` | 40 | 8 |
| `logger_diagnostics_t` | 192 | 8 |
| `logger_file_metrics_t` | 168 | 8 |
| `logger_syslog_config_t` | 24 | 8 |
| `logger_syslog_metrics_t` | 104 | 8 |
| `console_config_t` | 16 | 4 |


`logger_t`是不透明对象，私有映射、锁和工作线程布局不属于公开结构体ABI。

## 必须分别验收的行为

生命周期：借用者静默、取消前后的拥有者交接、普通I/O错误与释放证明失败。
并发：默认借用归还与代次保护，不把空指针当作已经回收的证明。
fork：父方继续，子方有效快照后显式重建，新实例不能复活旧引用；支持范围见
[运行域合同](../docs/v2/FORK_RUNTIME.md)。
包装：真实静态/共享产物、精确导出、安装后C/C++11、pkg-config和PIC消费方。
类型测试：创建/销毁、控制、Console、记录、查询的新类型正例与旧签名负例；
缺失工具不得当作成功拒绝。动态加载还要确认已移除的status符号不存在。

最终结果按[发布工程](../docs/RELEASE_ENGINEERING.md)记录源码提交、树、实际工具、
原始通过/失败/跳过和产物摘要。不能从一次CI绿色推导所有第三方运行时或物理
存储持久性；不会恢复Audit或物理掉电测试。
