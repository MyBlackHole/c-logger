# LOGGER_2.0 公开 ABI 合同（稳定发布准备）

本文件将当前已存在的 ABI 2 可执行快照整理为发布合同；最终固定源码与发布包尚待验收，
不表示 2.0.0 已发布，也不通过文字引入新的函数、布局、默认值或运行时保证。

## 权威与来源

适用范围为 Linux x86_64/ELF64。动态导出清单为 `abi/logger-2.0.symbols`，
SONAME 为 `liblogger.so.2`，ELF 默认符号版本为 `LOGGER_2.0`。
项目源代码中的 `tests/packaging/current_abi_contract.c` / `.cpp` 是可执行布局、数值、
默认值和函数类型快照；`.cpp` 原样包含 `.c`。C11检查精确函数类型；C++11独立检查
布局、数值和默认初始化器，不宣称C++模板检查覆盖了所有C函数属性。

准备来源：55046bfe2833a13aac848f1575c763be3af868eb。以下断言及默认值代码原样摘录自该快照，
本次未修改测试源。最终验收以发布标签解析的源码及独立记录 `SOURCE_COMMIT` 为准；
这里不嵌入文档自身的提交号。源快照 SHA-256：`c8e0a2b93ae5ab799da5d49c3baab72bf15ec2d09e2e6a5d4123307dc8feeac2`。

## 兼容接口面与演进

2.0.0 当前合同包含47个生产C导出；不含Audit/crypto或旧fork辅助接口。
从v1迁移必须重新构建和验收，不承诺v1源码或二进制兼容。待本合同最终验收确认后，
兼容2.x更新应保持既有类型大小/对齐/字段偏移、数值、精确函数类型、配置版本及已有默认语义。
新增接口须显式ABI评审，不能用通配符扩大导出；破坏布局、函数类型、数值或所有权/错误
语义的变更不能伪装为兼容更新。配置扩展遵守[UAPI规则](../docs/UAPI.md)。

## ELF 之外的语义

- 宿主拥有显式实例，借用方不取得销毁权；销毁/卸载前先停止并join借用者。
- 普通I/O关闭失败仍消费实例；释放证明失败可保留内部容器，不能重试旧句柄。入口拒绝例外见[API](../API.md)。
- 状态接口逐项遵守0/-1+errno，`logger_log_sync_status`保留0/-errno。
- flush等待既定完成水位并同步文件；不提供硬实时I/O上界，不等于Syslog持久确认。
- 原始fork继承运行时拒绝使用；API不具备信号安全性，一般ASYNC+ENABLE取消不在合同内。
- 过载、部分写入、可见截断、粘滞错误、文件单所有者及可信目录边界见[API](../API.md)。
- 不提供Audit事务、哈希链、业务exactly-once、未确认尾部自动修复或物理断电认证。

具体语义继续以[API](../API.md)、[支持矩阵](../docs/v2/SUPPORT_MATRIX.md)、
[生命周期](../docs/LIFECYCLE.md)和[崩溃合同](../docs/LOGGER_CRASH_CONTRACT.md)交叉核对。
下面数值快照不能替代这些语义。

## #108 公开错误模型决策及生命周期冻结边界

### 比较和选择

#108 原始任务列出的三个方案现按已存在的 ABI 2 运行时代码进行评审：

| 方案 | 评价 | 2.0 本次决定 |
|---|---|---|
| A：所有公开状态入口也返回 0 / -errno | 便于传递内部错误，但会改变既有公开 POSIX 状态调用的返回惯例；与现行绝大多数 ABI 2 代码和消费方不一致 | 不采用为统一规则 |
| B：公开状态为 0 / -1 并设置 errno；内部为 0 / -errno | 与文件/进程操作的正常调用惯例一致，能把资源关闭错误与恢复状态明确报告给宿主；已有生产实现和安装方使用此契约 | **选择为默认规则** |
| C：新增公开错误枚举与细节对象 | 为当前普通 Logger 引入额外公开类型、返回值和生命周期，并不能解决日志队列的异步失败；缺少必须升级 ABI 的明确需求 | 本次不采用 |

**唯一明确保留的历史公开例外**：logger_log_sync_status() 直接返回
0 / 负 errno（例如非法输入为 -EINVAL、过长为 -EOVERFLOW）。
它不是统一的 POSIX 状态接口，也不能把返回 -1 当作所有错误的判定条件。
同一实参、返回值和函数指针类型属于已验证的 ABI 2 语义快照；若将来计划
消除例外，必须单独评审公开行为变更，不能在普通内部整理时静默改动。

普通 0 / -1+errno 入口包括实例与全局的 flush、reopen、destroy/shutdown
及文件/Syslog 状态指标。创建返回实例所有权指针或 NULL+errno；
无返回值的记录、诊断与 Console 配置入口在失败时依既有 API 合同设置
errno，不能假定 void 接口会提供每条记录是否已接收的确认。

### 所有权、状态与可观察错误

- 显式 logger_t 只由成功创建后的宿主拥有；SDK/插件是借用者。销毁前宿主
  停止并等待它们退出，最终销毁即消费原有裸指针。普通最终 I/O 错误
  不把已释放实例变为可重试对象。
- Global 入口保留阶段+代次与生命周期固定保护，借用权不跨越一次 API；
  STARTING/STOPPING、代次不匹配、BROKEN 与原始 fork 子进程的拒绝行为
  分别由公开 errno 合同确定。
- async 日志入口返回值不承诺逐条成功；DROP、同步回退、输出完成/失败和
  粘滞首错通过不同指标观测。flush 成功不等于 Syslog 守护进程持久确认，
  更不等于真实设备物理掉电证明。
- 对不支持的配置版本、过短结构、非零未来尾部，必须按
  EPROTONOSUPPORT / EINVAL / E2BIG 拒绝；不悄悄截断或把未知语义当成
  安全零值。可扩展配置仅允许已冻结部分原布局与追加零值尾部。
- 同线程重入、延迟取消、原始 fork、跨静态库副本句柄的边界必须与
  [现行架构](../docs/v2/ARCHITECTURE.md)及 [API](../API.md) 一致。

### 可执行冻结检查与审批状态

- tests/test_api_contract.c 直接链接当前生产 logger：验证 NULL 创建、
  实例 flush/reopen、Global 初始化前 flush、无效初始化、NULL 销毁以及
  同步状态接口的 -EINVAL 特殊约定。
- tests/test_config_abi.c 验证配置默认、版本/大小/值拒绝和未来结构的
  零尾/非零尾规则；未以旧 1.x 短结构前缀冒充 2.0 兼容。
- tests/packaging/current_abi_contract.c 与其 C++11 消费方冻结类型大小、
  对齐、偏移、数值常量、默认值及 47 个函数签名；ELF 检查独立验收
  allowlist、SONAME 2、LOGGER_2.0 符号版本，不以编译成功替代 ABI 检查。
- 发布验证还须以最终固定源码实际生成的 static/shared TGZ，分别在
  最低受支持环境与新系统运行安装后的 C/C++11/pkg-config/PIC SDK 消费方。

**此处完成“错误模型选型与现行接口语义”设计决定，不等于签署最终稳定发布。**
正式 API/ABI 冻结仍要求 #104 对象生命周期边界与 #108 的最终源码/产物
审查、全部适用 CI 及明确的发布审批。未经独立授权，不创建或使用 v2.0.0
标签触发 GitHub Release。

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

`logger_t` 为不透明句柄，内部布局不属于公开结构体合同。

## 原样可执行断言

下面 `ABI_TYPE` 表示sizeof/alignof，`ABI_FIELD`表示offsetof，`ABI_ASSERT`表示常量断言，
`ABI_FN`表示C11精确函数指针类型检查。内容是已有测试的文档快照，不是新测试或新保证。

```c
/* Linux/ELF x86_64 当前 ABI 2 聚合类型布局契约。 */
ABI_TYPE(logger_config_t, 144, 8);
ABI_FIELD(logger_config_t, struct_size, 0);
ABI_FIELD(logger_config_t, version, 4);
ABI_FIELD(logger_config_t, level, 8);
ABI_FIELD(logger_config_t, detail, 12);
ABI_FIELD(logger_config_t, outputs, 16);
ABI_FIELD(logger_config_t, file_path, 24);
ABI_FIELD(logger_config_t, rotation, 32);
ABI_FIELD(logger_config_t, file_mode, 56);
ABI_FIELD(logger_config_t, async_mode, 60);
ABI_FIELD(logger_config_t, queue_capacity, 64);
ABI_FIELD(logger_config_t, flush_level, 72);
ABI_FIELD(logger_config_t, include_pid, 76);
ABI_FIELD(logger_config_t, include_tid, 80);
ABI_FIELD(logger_config_t, include_source, 84);
ABI_FIELD(logger_config_t, ident, 88);
ABI_FIELD(logger_config_t, overflow, 96);
ABI_FIELD(logger_config_t, syslog, 120);
ABI_ASSERT(LOGGER_CONFIG_SIZE == 144);

ABI_TYPE(logger_rotation_config_t, 24, 8);
ABI_FIELD(logger_rotation_config_t, mode, 0);
ABI_FIELD(logger_rotation_config_t, max_file_size, 8);
ABI_FIELD(logger_rotation_config_t, retention_days, 16);

ABI_TYPE(logger_source_t, 32, 8);
ABI_FIELD(logger_source_t, module, 0);
ABI_FIELD(logger_source_t, file, 8);
ABI_FIELD(logger_source_t, func, 16);
ABI_FIELD(logger_source_t, line, 24);

ABI_TYPE(logger_context_t, 24, 8);
ABI_FIELD(logger_context_t, request_id, 0);
ABI_FIELD(logger_context_t, session_id, 8);
ABI_FIELD(logger_context_t, trace_id, 16);

ABI_TYPE(logger_metrics_t, 88, 8);
ABI_FIELD(logger_metrics_t, dropped, 0);
ABI_FIELD(logger_metrics_t, queue_high_watermark, 48);
ABI_FIELD(logger_metrics_t, enqueued, 56);
ABI_FIELD(logger_metrics_t, sync_fallbacks, 64);
ABI_FIELD(logger_metrics_t, consumer_batches, 72);
ABI_FIELD(logger_metrics_t, consumer_records, 80);

ABI_TYPE(logger_io_metrics_t, 40, 8);
ABI_FIELD(logger_io_metrics_t, async_completed, 0);
ABI_FIELD(logger_io_metrics_t, sync_completed, 8);
ABI_FIELD(logger_io_metrics_t, emitted_records, 16);
ABI_FIELD(logger_io_metrics_t, failed_records, 24);
ABI_FIELD(logger_io_metrics_t, first_error, 32);

ABI_TYPE(logger_diagnostics_t, 192, 8);
ABI_FIELD(logger_diagnostics_t, state, 0);
ABI_FIELD(logger_diagnostics_t, level, 4);
ABI_FIELD(logger_diagnostics_t, outputs, 8);
ABI_FIELD(logger_diagnostics_t, observation_flags, 12);
ABI_FIELD(logger_diagnostics_t, async_mode, 16);
ABI_FIELD(logger_diagnostics_t, worker_running, 20);
ABI_FIELD(logger_diagnostics_t, consumer_waiting, 24);
ABI_FIELD(logger_diagnostics_t, first_error, 28);
ABI_FIELD(logger_diagnostics_t, dropped_records, 32);
ABI_FIELD(logger_diagnostics_t, sync_fallbacks, 40);
ABI_FIELD(logger_diagnostics_t, enqueued, 48);
ABI_FIELD(logger_diagnostics_t, consumer_batches, 56);
ABI_FIELD(logger_diagnostics_t, consumer_records, 64);
ABI_FIELD(logger_diagnostics_t, async_completed, 72);
ABI_FIELD(logger_diagnostics_t, sync_completed, 80);
ABI_FIELD(logger_diagnostics_t, emitted_records, 88);
ABI_FIELD(logger_diagnostics_t, failed_records, 96);
ABI_FIELD(logger_diagnostics_t, queue_capacity, 104);
ABI_FIELD(logger_diagnostics_t, queue_depth, 112);
ABI_FIELD(logger_diagnostics_t, queue_high_watermark, 120);
ABI_FIELD(logger_diagnostics_t, completion_backlog, 128);
ABI_FIELD(logger_diagnostics_t, queue_storage_bytes, 136);
ABI_FIELD(logger_diagnostics_t, spill_capacity, 144);
ABI_FIELD(logger_diagnostics_t, spill_in_use, 152);
ABI_FIELD(logger_diagnostics_t, spill_exhaustions, 160);
ABI_FIELD(logger_diagnostics_t, worker_wait_count, 168);
ABI_FIELD(logger_diagnostics_t, producer_wake_signals, 176);
ABI_FIELD(logger_diagnostics_t, force_wake_signals, 184);

ABI_TYPE(logger_file_metrics_t, 168, 8);
ABI_FIELD(logger_file_metrics_t, write_operations, 0);
ABI_FIELD(logger_file_metrics_t, failed_write_operations, 8);
ABI_FIELD(logger_file_metrics_t, write_syscalls, 16);
ABI_FIELD(logger_file_metrics_t, interrupted_write_syscalls, 24);
ABI_FIELD(logger_file_metrics_t, failed_write_syscalls, 32);
ABI_FIELD(logger_file_metrics_t, bytes_written, 40);
ABI_FIELD(logger_file_metrics_t, data_sync_attempts, 48);
ABI_FIELD(logger_file_metrics_t, data_sync_failures, 56);
ABI_FIELD(logger_file_metrics_t, directory_sync_attempts, 64);
ABI_FIELD(logger_file_metrics_t, directory_sync_failures, 72);
ABI_FIELD(logger_file_metrics_t, rotation_attempts, 80);
ABI_FIELD(logger_file_metrics_t, rotation_successes, 88);
ABI_FIELD(logger_file_metrics_t, rotation_failures, 96);
ABI_FIELD(logger_file_metrics_t, reopen_attempts, 104);
ABI_FIELD(logger_file_metrics_t, reopen_successes, 112);
ABI_FIELD(logger_file_metrics_t, reopen_failures, 120);
ABI_FIELD(logger_file_metrics_t, retention_deletions, 128);
ABI_FIELD(logger_file_metrics_t, current_size, 136);
ABI_FIELD(logger_file_metrics_t, detached, 144);
ABI_FIELD(logger_file_metrics_t, last_error, 148);
ABI_FIELD(logger_file_metrics_t, last_write_error, 152);
ABI_FIELD(logger_file_metrics_t, last_sync_error, 156);
ABI_FIELD(logger_file_metrics_t, last_rotation_error, 160);
ABI_FIELD(logger_file_metrics_t, last_reopen_error, 164);

ABI_TYPE(logger_syslog_config_t, 24, 8);
ABI_FIELD(logger_syslog_config_t, path, 0);
ABI_FIELD(logger_syslog_config_t, facility, 8);
ABI_FIELD(logger_syslog_config_t, reconnect_interval_ms, 12);
ABI_FIELD(logger_syslog_config_t, startup, 16);

ABI_TYPE(logger_syslog_metrics_t, 104, 8);
ABI_FIELD(logger_syslog_metrics_t, submitted_records, 0);
ABI_FIELD(logger_syslog_metrics_t, sent_records, 8);
ABI_FIELD(logger_syslog_metrics_t, failed_records, 16);
ABI_FIELD(logger_syslog_metrics_t, backpressure_records, 24);
ABI_FIELD(logger_syslog_metrics_t, cooldown_records, 32);
ABI_FIELD(logger_syslog_metrics_t, connect_attempts, 40);
ABI_FIELD(logger_syslog_metrics_t, connect_failures, 48);
ABI_FIELD(logger_syslog_metrics_t, reconnect_successes, 56);
ABI_FIELD(logger_syslog_metrics_t, send_attempts, 64);
ABI_FIELD(logger_syslog_metrics_t, interrupted_calls, 72);
ABI_FIELD(logger_syslog_metrics_t, close_failures, 80);
ABI_FIELD(logger_syslog_metrics_t, connected, 88);
ABI_FIELD(logger_syslog_metrics_t, last_error, 92);
ABI_FIELD(logger_syslog_metrics_t, last_close_error, 96);


ABI_TYPE(console_config_t, 16, 4);
ABI_ASSERT(CONSOLE_CONFIG_SIZE == 16);
ABI_FIELD(console_config_t, struct_size, 0);
ABI_FIELD(console_config_t, version, 4);
ABI_FIELD(console_config_t, verbosity, 8);
ABI_FIELD(console_config_t, color, 12);

/* 数值 API 契约。 */
ABI_ASSERT(LOGGER_TRACE == 0);
ABI_ASSERT(LOGGER_DEBUG == 1);
ABI_ASSERT(LOGGER_INFO == 2);
ABI_ASSERT(LOGGER_WARN == 3);
ABI_ASSERT(LOGGER_ERROR == 4);
ABI_ASSERT(LOGGER_FATAL == 5);
ABI_ASSERT(LOGGER_OFF == 6);
ABI_ASSERT(LOGGER_DETAIL_MINIMAL == 0);
ABI_ASSERT(LOGGER_DETAIL_NORMAL == 1);
ABI_ASSERT(LOGGER_DETAIL_VERBOSE == 2);
ABI_ASSERT(LOGGER_DETAIL_DEBUG == 3);
ABI_ASSERT(LOGGER_OUT_STDERR == 1);
ABI_ASSERT(LOGGER_OUT_FILE == 2);
ABI_ASSERT(LOGGER_OUT_SYSLOG == 4);
ABI_ASSERT(LOGGER_ROTATE_NONE == 0);
ABI_ASSERT(LOGGER_ROTATE_SIZE == 1);
ABI_ASSERT(LOGGER_ROTATE_DAILY == 2);
ABI_ASSERT(LOGGER_ROTATE_SIZE_DAILY == 3);
ABI_ASSERT(LOGGER_ROTATE_EXTERNAL == 4);
ABI_ASSERT(LOGGER_STATE_CREATED == 0);
ABI_ASSERT(LOGGER_STATE_RUNNING == 1);
ABI_ASSERT(LOGGER_STATE_STOPPING == 2);
ABI_ASSERT(LOGGER_STATE_STOPPED == 3);
ABI_ASSERT(LOGGER_OVERFLOW_DROP == 0);
ABI_ASSERT(LOGGER_OVERFLOW_SYNC == 1);
ABI_ASSERT(LOGGER_DIAG_DROPS_OBSERVED == 1);
ABI_ASSERT(LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED == 2);
ABI_ASSERT(LOGGER_DIAG_QUEUE_SATURATED == 4);
ABI_ASSERT(LOGGER_DIAG_SPILL_EXHAUSTIONS_OBSERVED == 8);
ABI_ASSERT(LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED == 16);
ABI_ASSERT(LOGGER_DIAG_WORKER_STOPPED == 32);
ABI_ASSERT(LOGGER_SYSLOG_START_REQUIRED == 0);
ABI_ASSERT(LOGGER_SYSLOG_START_DEFERRED == 1);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_USER == 8);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL0 == 128);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL1 == 136);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL2 == 144);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL3 == 152);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL4 == 160);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL5 == 168);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL6 == 176);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL7 == 184);
ABI_ASSERT(LOGGER_CONFIG_VERSION == 1);


ABI_ASSERT(CONSOLE_COLOR_AUTO == 0);
ABI_ASSERT(CONSOLE_COLOR_ALWAYS == 1);
ABI_ASSERT(CONSOLE_COLOR_NEVER == 2);
ABI_ASSERT(CONSOLE_QUIET == 0);
ABI_ASSERT(CONSOLE_NORMAL == 1);
ABI_ASSERT(CONSOLE_VERBOSE == 2);
ABI_ASSERT(CONSOLE_DEBUG == 3);
ABI_ASSERT(CONSOLE_CONFIG_VERSION == 1);

/* 精确公开函数类型契约：47 个默认生产导出。 */

ABI_FN(console_debug, int, const char *, ...);
ABI_FN(console_debug_source, int, const char *, int, const char *, const char *, ...);
ABI_FN(console_error, int, const char *, ...);
ABI_FN(console_info, int, const char *, ...);
ABI_FN(console_init, void, const console_config_t *);
ABI_FN(console_print, int, const char *, ...);
ABI_FN(console_set_color, void, console_color_mode_t);
ABI_FN(console_set_verbosity, void, console_verbosity_t);
ABI_FN(console_stderr_is_tty, int, void);
ABI_FN(console_stdout_is_tty, int, void);
ABI_FN(console_verbose, int, const char *, ...);
ABI_FN(console_warn, int, const char *, ...);

ABI_FN(logger_context_clear, void, void);
ABI_FN(logger_context_get, logger_context_t, void);
ABI_FN(logger_context_set, void, const logger_context_t *);
ABI_FN(logger_create, logger_t *, const logger_config_t *);
ABI_FN(logger_destroy, void, logger_t *);
ABI_FN(logger_destroy_status, int, logger_t *);
ABI_FN(logger_dropped, uint64_t, const logger_t *);
ABI_FN(logger_flush, void, void);
ABI_FN(logger_flush_instance, void, logger_t *);
ABI_FN(logger_flush_instance_status, int, logger_t *);
ABI_FN(logger_flush_status, int, void);
ABI_FN(logger_get_global_diagnostics, void, logger_diagnostics_t *);
ABI_FN(logger_get_global_file_metrics, int, logger_file_metrics_t *);
ABI_FN(logger_get_global_io_metrics, void, logger_io_metrics_t *);
ABI_FN(logger_get_global_metrics, void, logger_metrics_t *);
ABI_FN(logger_get_diagnostics, void, const logger_t *, logger_diagnostics_t *);
ABI_FN(logger_get_file_metrics, int, logger_t *, logger_file_metrics_t *);
ABI_FN(logger_get_io_metrics, void, const logger_t *, logger_io_metrics_t *);
ABI_FN(logger_get_metrics, void, const logger_t *, logger_metrics_t *);
ABI_FN(logger_get_state, logger_state_t, const logger_t *);
ABI_FN(logger_get_syslog_metrics, int, logger_t *, logger_syslog_metrics_t *);
ABI_FN(logger_global_dropped, uint64_t, void);
ABI_FN(logger_global_write, void, logger_level_t, const char *, const char *, int,
       const char *, const char *, ...);
ABI_FN(logger_init, int, const logger_config_t *);
ABI_FN(logger_log, void, logger_t *, logger_level_t, const char *, const char *, int,
       const char *, const char *, ...);
ABI_FN(logger_log_source, void, logger_t *, logger_level_t, logger_source_t,
       const char *, ...);
ABI_FN(logger_log_sync_status, int, logger_t *, logger_level_t, const char *,
       const char *, int, const char *, const char *, ...);
ABI_FN(logger_mask_secret, int, const char *, char *, size_t, size_t, size_t);
ABI_FN(logger_redact, const char *, void);
ABI_FN(logger_reopen, int, void);
ABI_FN(logger_reopen_instance, int, logger_t *);
ABI_FN(logger_set_instance_level, void, logger_t *, logger_level_t);
ABI_FN(logger_set_level, void, logger_level_t);
ABI_FN(logger_shutdown, void, void);
ABI_FN(logger_shutdown_status, int, void);
```

## 原样默认值断言

```c
static int defaults_match(void)
{
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	if (l.struct_size != 144 || l.version != 1 || l.level != LOGGER_INFO ||
	    l.detail != LOGGER_DETAIL_VERBOSE || l.outputs != LOGGER_OUT_STDERR ||
	    l.file_path != NULL || l.rotation.mode != LOGGER_ROTATE_SIZE_DAILY ||
	    l.rotation.max_file_size != 100u * 1024u * 1024u ||
	    l.rotation.retention_days != 30 || l.file_mode != 0640 ||
	    l.async_mode != 1 || l.queue_capacity != 8192 ||
	    l.flush_level != LOGGER_ERROR || l.include_pid != 1 ||
	    l.include_tid != 1 || l.include_source != 1 || !l.ident ||
	    strcmp(l.ident, "app"))
		return 1;
	const logger_overflow_policy_t overflow[6] = {
		LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP,
		LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_SYNC, LOGGER_OVERFLOW_SYNC
	};
	for (unsigned i = 0; i < 6; ++i)
		if (l.overflow[i] != overflow[i])
			return 2;
	if (l.syslog.path != NULL ||
	    l.syslog.facility != LOGGER_SYSLOG_FACILITY_USER ||
	    l.syslog.reconnect_interval_ms != 1000u ||
	    l.syslog.startup != LOGGER_SYSLOG_START_REQUIRED)
		return 3;


	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	if (c.struct_size != 16 || c.version != 1 ||
	    c.verbosity != CONSOLE_NORMAL || c.color != CONSOLE_COLOR_AUTO)
		return 5;
	return 0;
}
```

## 验证要求

最终源码须通过当前C11/C++11可执行契约、真实共享库47符号/版本/SONAME检查、静态和共享
安装消费方、配置负向测试与生产隔离检查。仅编译本文或仅数出符号不能替代它们。
最终验收结果及来源按[发布工程](../docs/RELEASE_ENGINEERING.md)记录；未执行项保持未验收。
