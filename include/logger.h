#ifndef LOGGER_H
#define LOGGER_H
#include "logger_export.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
	LOGGER_TRACE,
	LOGGER_DEBUG,
	LOGGER_INFO,
	LOGGER_WARN,
	LOGGER_ERROR,
	LOGGER_FATAL,
	LOGGER_OFF
} logger_level_t;
typedef enum {
	LOGGER_DETAIL_MINIMAL = 0,
	LOGGER_DETAIL_NORMAL,
	LOGGER_DETAIL_VERBOSE,
	LOGGER_DETAIL_DEBUG
} logger_detail_t;
typedef struct {
	const char *request_id;
	const char *session_id;
	const char *trace_id;
} logger_context_t;
typedef enum {
	LOGGER_OUT_STDERR = 1u << 0,
	LOGGER_OUT_FILE = 1u << 1,
	LOGGER_OUT_SYSLOG = 1u << 2
} logger_output_t;
typedef enum {
	LOGGER_ROTATE_NONE = 0,
	LOGGER_ROTATE_SIZE,
	LOGGER_ROTATE_DAILY,
	LOGGER_ROTATE_SIZE_DAILY,
	LOGGER_ROTATE_EXTERNAL
} logger_rotate_mode_t;
typedef struct {
	logger_rotate_mode_t mode;
	size_t max_file_size;
	unsigned retention_days;
} logger_rotation_config_t;
typedef struct logger logger_t;
typedef enum {
	LOGGER_STATE_CREATED = 0,
	LOGGER_STATE_RUNNING,
	LOGGER_STATE_STOPPING,
	LOGGER_STATE_STOPPED
} logger_state_t;
typedef struct {
	const char *module;
	const char *file;
	const char *func;
	int line;
} logger_source_t;
#ifdef __cplusplus
#define LOGGER_SOURCE() \
	(logger_source_t{ LOG_MODULE, __FILE__, __func__, __LINE__ })
#else
#define LOGGER_SOURCE()                           \
	((logger_source_t){ .module = LOG_MODULE, \
			    .file = __FILE__,     \
			    .func = __func__,     \
			    .line = __LINE__ })
#endif

typedef enum {
	LOGGER_OVERFLOW_DROP = 0,
	LOGGER_OVERFLOW_SYNC = 1
} logger_overflow_policy_t;
typedef struct {
	uint64_t dropped[6]; /* 仅统计队列满时的 DROP；不包含同步回退。 */
	uint64_t queue_high_watermark;
	uint64_t enqueued;
	uint64_t sync_fallbacks;
	uint64_t consumer_batches;
	uint64_t consumer_records; /* 已出队记录数，不代表输出已经完成。 */
} logger_metrics_t;
/* 独立扩展：logger_metrics_t 保持原有二进制布局。
 * 活动快照可能跨越并发更新。生产者静默并完成刷新后，
 * completed == emitted + failed。failed 表示未能在所有已选择输出端上确认成功；
 * 一个部分失败的批次中可能已经包含实际写出的字节。 */
typedef struct {
	uint64_t async_completed;
	uint64_t sync_completed;
	uint64_t emitted_records;
	uint64_t failed_records;
	int first_error; /* 正 errno；实例生命周期内绝不清除。 */
} logger_io_metrics_t;

/* 统一的低开销可观测性快照。
 *
 * 该接口不会替代 logger_metrics_t/logger_io_metrics_t：这些布局继续为既有调用方保持冻结。
 * 诊断快照在不获取后端锁、不执行 I/O、不刷新、不重连且不清理错误状态的前提下，
 * 补充当前队列/工作线程状态以及客观生命周期观测信息。实时字段均为无数据竞争的近似快照，
 * 可能跨越并发更新。 */
typedef enum {
	LOGGER_DIAG_DROPS_OBSERVED = 1u << 0,
	LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED = 1u << 1,
	LOGGER_DIAG_QUEUE_SATURATED = 1u << 2,
	LOGGER_DIAG_SPILL_EXHAUSTIONS_OBSERVED = 1u << 3,
	LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED = 1u << 4,
	LOGGER_DIAG_WORKER_STOPPED = 1u << 5
} logger_diagnostic_flag_t;

typedef struct {
	logger_state_t state;
	logger_level_t level;
	unsigned outputs;
	unsigned observation_flags;
	int async_mode;
	int worker_running;
	int consumer_waiting;
	int first_error;

	uint64_t dropped_records;
	uint64_t sync_fallbacks;
	uint64_t enqueued;
	uint64_t consumer_batches;
	uint64_t consumer_records;
	uint64_t async_completed;
	uint64_t sync_completed;
	uint64_t emitted_records;
	uint64_t failed_records;

	uint64_t queue_capacity;
	uint64_t queue_depth;
	uint64_t queue_high_watermark;
	uint64_t completion_backlog;
	uint64_t queue_storage_bytes;

	uint64_t spill_capacity;
	uint64_t spill_in_use;
	uint64_t spill_exhaustions;

	uint64_t worker_wait_count;
	uint64_t producer_wake_signals;
	uint64_t force_wake_signals;
} logger_diagnostics_t;

/* 文件后端的一致性快照，在实例输出互斥锁保护下获取。
 * 计数器只覆盖已选择的文件输出端；stderr/Syslog 分别独立。
 * bytes_written 包含在后续写入失败之前已经实际提交的部分字节。
 * last_error 表示最近一次文件操作结果（文件操作成功后为 0）；
 * 专用 last_* 错误字段保留对应类别最近一次失败。 */
typedef struct {
	uint64_t write_operations;
	uint64_t failed_write_operations;
	uint64_t write_syscalls;
	uint64_t interrupted_write_syscalls;
	uint64_t failed_write_syscalls;
	uint64_t bytes_written;

	uint64_t data_sync_attempts;
	uint64_t data_sync_failures;
	uint64_t directory_sync_attempts;
	uint64_t directory_sync_failures;

	uint64_t rotation_attempts;
	uint64_t rotation_successes;
	uint64_t rotation_failures;
	uint64_t reopen_attempts;
	uint64_t reopen_successes;
	uint64_t reopen_failures;
	uint64_t retention_deletions;

	uint64_t current_size;
	int detached;
	int last_error;
	int last_write_error;
	int last_sync_error;
	int last_rotation_error;
	int last_reopen_error;
} logger_file_metrics_t;

/* 本地 Unix 数据报 Syslog。不存在逐输出端后备队列，也不存在阻塞重试队列。
 * REQUIRED 保留构造期连接失败语义；DEFERRED 只允许端点暂时不可用，
 * 并通过指标/粘滞状态暴露该失败。 */
typedef enum {
	LOGGER_SYSLOG_START_REQUIRED = 0,
	LOGGER_SYSLOG_START_DEFERRED = 1
} logger_syslog_start_t;
/* 使用带命名空间的 facility 值，避免强制包含 <syslog.h>；其中的 LOG_* 宏会与
 * 历史全局便利宏冲突。其他标准 facility 编号 0..23 也可以按 (number * 8) 传入。 */
#define LOGGER_SYSLOG_FACILITY_USER (1u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL0 (16u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL1 (17u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL2 (18u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL3 (19u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL4 (20u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL5 (21u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL6 (22u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL7 (23u << 3)
typedef struct {
	const char *path; /* NULL 表示 /dev/log；构造时复制。 */
	unsigned facility; /* 带命名空间的 facility，或标准编号 * 8。 */
	uint32_t reconnect_interval_ms; /* 0 表示 1000 ms；否则范围为 1..60000。 */
	logger_syslog_start_t startup;
} logger_syslog_config_t;
#ifdef __cplusplus
static inline logger_syslog_config_t logger_syslog_defaults_cpp(void)
{
	logger_syslog_config_t c = {};
	c.facility = LOGGER_SYSLOG_FACILITY_USER;
	c.reconnect_interval_ms = 1000u;
	c.startup = LOGGER_SYSLOG_START_REQUIRED;
	return c;
}
#define LOGGER_SYSLOG_DEFAULT_CONFIG() logger_syslog_defaults_cpp()
#else
#define LOGGER_SYSLOG_DEFAULT_CONFIG()                                      \
	((logger_syslog_config_t){ .path = NULL,                            \
				   .facility = LOGGER_SYSLOG_FACILITY_USER, \
				   .reconnect_interval_ms = 1000u,          \
				   .startup = LOGGER_SYSLOG_START_REQUIRED })
#endif

/* 快照在输出互斥锁保护下获取。connected 只表示本地关联状态，
 * 不是对端健康检查；send 成功也不是持久投递确认。
 * failed_records 包含背压、冷却期拒绝和格式化错误；
 * backpressure_records/cooldown_records 是其子集。初始 DEFERRED 失败
 * 会修改 last_error 和实例粘滞 first_error，但不会计入记录数。 */
typedef struct {
	uint64_t submitted_records;
	uint64_t sent_records;
	uint64_t failed_records;
	uint64_t backpressure_records;
	uint64_t cooldown_records;
	uint64_t connect_attempts; /* 一次 socket+connect 周期，包含初始连接。 */
	uint64_t connect_failures;
	uint64_t reconnect_successes; /* 初始周期之后成功完成的连接周期。 */
	uint64_t send_attempts; /* 系统调用次数，包含有界 EINTR 重试。 */
	uint64_t interrupted_calls;
	uint64_t close_failures;
	int connected;
	int last_error; /* 最近一次操作错误；成功 send 后清除。 */
	int last_close_error; /* 最近一次清理失败；成功 send 不会清除。 */
} logger_syslog_metrics_t;

#define LOGGER_CONFIG_VERSION 1u
typedef struct {
	uint32_t struct_size;
	uint32_t version;
	logger_level_t level;
	logger_detail_t detail;
	unsigned outputs;
	const char *file_path;
	logger_rotation_config_t rotation;
	unsigned file_mode;
	int async_mode;
	size_t queue_capacity;
	logger_level_t flush_level;
	int include_pid, include_tid, include_source;
	const char *ident;
	logger_overflow_policy_t overflow[6];
	/* 仅追加扩展。较旧的 version-1 前缀尺寸使用 Syslog 默认值。
     * 要么提供完整扩展，要么完全不提供；不完整尾部会被拒绝。
     * 不支持没有版本字段的旧二进制。见 docs/SYSLOG_BACKEND.md。 */
	logger_syslog_config_t syslog;
} logger_config_t;
#define LOGGER_CONFIG_V1_PREFIX_SIZE offsetof(logger_config_t, syslog)
/* 所有权/线程模型：
 * - logger_create() 返回由调用方拥有的实例；logger_destroy() 负责释放。
 * - cfg 只在 create 调用期间借用；不得并发修改。
 * - 业务 .so 借用宿主的实例；只有所有者才能销毁。
 * - 每个异步实例拥有独立工作线程和队列。仅加载 .so 不会启动工作线程、
 *   不会打开日志，也不会初始化全局门面。
 * - 实例存活期间，日志写入/级别/指标/刷新可以并发调用。
 * - 销毁前，所有者必须停止并等待退出所有显式实例使用者。
 * 错误约定：返回指针的构造函数失败时使用 NULL + errno；int 类型公开操作
 * 除非另有明确说明，否则成功返回 0，失败返回 -1 并设置 errno。 */
/* 普通文件目标只能由单一所有者持有（<basename>.logger.lock）。第二个独立所有者
 * 会收到 EBUSY，应改为共享同一个活动实例。目录解析在构造时完成绑定；
 * 最终路径分量为符号链接/硬链接时会被拒绝。内部轮转要求支持不覆盖 rename。
 * 兼容性、资源成本和平台限制见 docs/FILE_BACKEND.md。 */
/* 显式调用会保护库拥有的资源，在线程取消发生时延迟执行，直到关键工作、va_end 和解锁
 * 全部完成。这不意味着提供回滚或 I/O 截止时间。同一线程内嵌套资源操作（包括另一个实例、
 * Console，以及异步工作线程回调中再次进入）会在获取锁/产生副作用之前以 EDEADLK 拒绝；
 * 返回状态的 sync_log 继续使用 -errno 约定。
 * 这不代表具备信号安全性，也不代表通用异步取消安全性。
 *
 * create 在交付指针前设置延迟取消检查点：如果存在待处理取消请求，会处置所有尚未返回的候选实例。
 * 当取消状态为 ENABLED 且类型为 ASYNCHRONOUS 时，会在分配实例/后端资源之前以 ENOTSUP 失败
 * （此时可能已经注册进程防护）；更合适的做法是在指针所有权转移期间禁用取消。
 * 任何成功返回的指针都归调用方所有，调用方必须为后续取消安排清理。 */
LOGGER_API logger_t *logger_create(const logger_config_t *);
LOGGER_API void logger_context_set(const logger_context_t *);
LOGGER_API void logger_context_clear(void);
LOGGER_API logger_context_t logger_context_get(void);

/* 显式敏感信息处理辅助接口。Logger 不会检查任意 printf 文本并猜测其中的秘密信息；
 * 调用方必须在记录敏感数据之前自行脱敏。 */
LOGGER_API const char *logger_redact(void);
LOGGER_API int logger_mask_secret(const char *value, char *out, size_t out_size,
				  size_t visible_prefix, size_t visible_suffix);

LOGGER_API void logger_log_source(logger_t *, logger_level_t, logger_source_t,
				  const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
	;

/* 仅允许独占所有者调用：必须先停止并等待退出所有使用者。返回 0 或 -1+errno。
 * 普通 I/O 失败时实例仍然会被释放；绝不能重试旧指针。NULL 视为成功。
 * ECHILD 会拒绝继承状态，不触碰也不释放它。取消会延迟到拆除完成后执行。
 * 不存在隐式引用计数。 */
LOGGER_API int logger_destroy_status(logger_t *);
/* 兼容包装接口，会丢弃拆除状态。 */
/* dispose 末尾存在待处理取消时，指针已经释放后可能继续执行宿主清理逻辑。
 * 因此必须在合法的独占所有者边界调用 destroy **之前**，先从宿主清理所有权中移除该指针；
 * 或者在“清空指针 + dispose”整个过程禁用取消。嵌套/子进程拒绝发生在处置之前。
 * 即使最终 I/O 返回错误，也绝不能重试已经释放的指针。 */
LOGGER_API void logger_destroy(logger_t *);
LOGGER_API logger_state_t logger_get_state(const logger_t *);
/* 输入字符串必须在整个调用期间保持有效。异步记录只快照 detail/include_* 实际需要的元数据；
 * 当需要 module/source 时，会分别复制 module（127 字节）、source basename（255 字节）
 * 和 function（127 字节）。只有对应 detail 级别可能输出 DEBUG 上下文时才复制该上下文。
 * 更长的 source 标签会使用 '~' 明确标记截断；后续格式化所需的任何队列指针
 * 都不会继续引用可卸载业务 .so 的存储。
 * 但这**不代表**可以在未完成调用和 Logger 工作线程结束之前卸载 liblogger 本身或回调提供方。 */
LOGGER_API void logger_log(logger_t *, logger_level_t, const char *,
			   const char *, int, const char *, const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 7, 8)))
#endif
	;
/* 高级同步状态路径。内部返回 0 或负 errno；为兼容性保留。
 * 新应用代码通常应使用 logger_log()/LOG_* 和 logger_flush_instance()。 */
LOGGER_API int logger_log_sync_status(logger_t *, logger_level_t, const char *,
				      const char *, int, const char *,
				      const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 7, 8)))
#endif
	;
/* 状态型刷新会等待进入函数时捕获的队列预留水位，
 * 其中包括已经出队但尚未输出的批次，随后对文件后端执行 fsync。
 * 并发调用也可能被包含；队列满导致的丢弃**不是**已接受记录。
 * 刷新之前成功完成的同步日志调用也在覆盖范围内。返回 0 或 -1 + errno。
 * 更早发生的输出/同步故障保持粘滞。这里既不是远端 Syslog 持久化确认，也不提供 I/O 超时。
 * 调用方必须在整个过程中保护显式实例生命周期。 */
LOGGER_API int logger_flush_instance_status(logger_t *);
/* 兼容包装接口：等待语义相同，但丢弃状态；NULL 直接空操作。 */
LOGGER_API void logger_flush_instance(logger_t *);
LOGGER_API int logger_reopen_instance(logger_t *);
LOGGER_API void logger_set_instance_level(logger_t *, logger_level_t);
LOGGER_API uint64_t logger_dropped(const logger_t *);
LOGGER_API void logger_get_metrics(const logger_t *, logger_metrics_t *);
LOGGER_API void logger_get_io_metrics(const logger_t *, logger_io_metrics_t *);
/* 无锁且无需后端锁的可观测性快照。非法访问、重入访问或子进程访问会返回全零快照，
 * 并沿用现有指标接口的 errno 语义。调用方仍必须保护 logger_t 生命周期，
 * 避免与并发销毁发生竞争。 */
LOGGER_API void logger_get_diagnostics(const logger_t *, logger_diagnostics_t *);
/* 文件后端的一致性快照。返回 0 / -1+errno；未选择文件输出时返回 ENOTSUP。
 * 该操作不会执行刷新/fsync/重新打开，且快照只读。 */
LOGGER_API int logger_get_file_metrics(logger_t *, logger_file_metrics_t *);
/* 要求实例处于活动状态，并使用与其他控制操作相同的外部生命周期保护。
 * 返回 0 / -1+errno。未选择 Syslog 时返回 ENOTSUP；失败时输出对象保持不变。
 * 该调用不会主动连接、重试、刷新，也不会重置粘滞 I/O 错误。 */
LOGGER_API int logger_get_syslog_metrics(logger_t *, logger_syslog_metrics_t *);
/* 可选的应用全局门面。并发数据/控制调用会固定到同一 generation，绝不会静默操作更新的实例。
 * 启动期间返回 EAGAIN；关闭期间/之后或者 pin 已过期时返回 ESHUTDOWN；
 * 尚无初始实例时返回 ENODEV（只有 LOG_* 继续保留历史启动期 stderr 回退）。
 * 同一线程嵌套调用全局接口会以 EDEADLK 失败。该接口不具备信号安全属性。
 * 全局调用会把取消延迟到所有库锁/pin 释放之后；取消不会回滚已经输出的数据，
 * 也不会回滚已经完成的初始化/关闭，同时不会引入 I/O 超时。
 * 显式实例操作使用前述内部取消/重入作用域。 */
LOGGER_API int logger_init(const logger_config_t *);
/* 串行化准入关闭、排空、等待退出、关闭和所有者释放。同一 generation 重复关闭视为成功；
 * 延迟到来的关闭绝不会销毁更新 generation（返回 ESHUTDOWN）。发生 I/O 失败时旧实例仍然释放；
 * 错误只向控制者返回一次，而不是交回一个可重试指针。与 void 包装接口不同，这个接口允许检查
 * 最终同步/关闭结果。 */
LOGGER_API int logger_shutdown_status(void);
LOGGER_API void logger_shutdown(void);
/* 对全局门面提供相同保证。 */
LOGGER_API int logger_flush_status(void);
LOGGER_API void logger_flush(void); /* 兼容包装接口；不返回错误。 */
LOGGER_API int logger_reopen(void);
/* 原始 fork 契约（本库不管理宿主进程）：
 * - 要么在任何库运行时使用之前、真正单线程状态下 fork；要么使用 fork+exec。
 *   异步初始化完成后，进程已经是多线程状态。
 * - 原始 fork 继承的 Logger/Console/Audit 运行时必须 exec 后才能重新初始化。
 *   构造函数/状态 API 以 ECHILD 拒绝（sync_status 返回 -ECHILD）；void 类型日志/销毁/控制
 *   直接空操作并设置 errno；指标返回全零，get_state 返回 STOPPED 并设置 errno=ECHILD，
 *   这些都不是“已经成功处置继承状态”的确认。
 * - prepare/parent 只作为兼容标记：fork 前后不会跨进程持锁，也不会刷新/冻结队列。
 *   子进程辅助接口只负责失效标记，绝不会重置锁，也不会释放/关闭继承状态；调用它**不会**启用重新初始化。
 * - 不得把这些辅助接口用于 vfork、信号处理器，也不能用它们让任意 fork 后调用变得安全。
 *   LOG_* 参数会在拒绝逻辑执行之前完成求值。
 * 父进程描述符/队列保持不变；子进程 fd 一直存活到 exec/_exit。 */
/* 可选历史应用辅助接口只由 logger_fork_compat.h 声明。
 * 常规库绝不会调用 fork，也不会扫描宿主线程列表。 */
#if defined(LOGGER_ENABLE_LEGACY_FORK_HELPER) && \
	LOGGER_ENABLE_LEGACY_FORK_HELPER
#include "logger_fork_compat.h"
#endif

LOGGER_API int logger_prepare_fork(void);
LOGGER_API void logger_after_fork_parent(void);
LOGGER_API void logger_after_fork_child(void);
LOGGER_API void logger_set_level(logger_level_t);
LOGGER_API uint64_t logger_global_dropped(void);
LOGGER_API void logger_get_global_metrics(logger_metrics_t *);
LOGGER_API void logger_get_global_io_metrics(logger_io_metrics_t *);
LOGGER_API void logger_get_global_diagnostics(logger_diagnostics_t *);
/* 通过当前全局代次获取同样的快照。返回 0/-1+errno，并保持全局准入/代次语义。 */
LOGGER_API int logger_get_global_file_metrics(logger_file_metrics_t *);
LOGGER_API void logger_global_write(logger_level_t, const char *, const char *,
				    int, const char *, const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 6, 7)))
#endif
	;
#ifndef LOG_MODULE
#define LOG_MODULE "app"
#endif
#define LOG_TRACE(...)                                                    \
	logger_global_write(LOGGER_TRACE, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_DEBUG(...)                                                    \
	logger_global_write(LOGGER_DEBUG, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_INFO(...)                                                    \
	logger_global_write(LOGGER_INFO, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_WARN(...)                                                    \
	logger_global_write(LOGGER_WARN, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_ERROR(...)                                                    \
	logger_global_write(LOGGER_ERROR, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_FATAL(...)                                                    \
	logger_global_write(LOGGER_FATAL, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOGGER_LOG(instance, level, module, ...)                      \
	logger_log((instance), (level), (module), __FILE__, __LINE__, \
		   __func__, __VA_ARGS__)
#define LOGGER_TRACE(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_TRACE, (module), __VA_ARGS__)
#define LOGGER_DEBUG(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_DEBUG, (module), __VA_ARGS__)
#define LOGGER_INFO(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_INFO, (module), __VA_ARGS__)
#define LOGGER_WARN(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_WARN, (module), __VA_ARGS__)
#define LOGGER_ERROR(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_ERROR, (module), __VA_ARGS__)
#define LOGGER_FATAL(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_FATAL, (module), __VA_ARGS__)
#ifdef __cplusplus
static inline logger_config_t logger_defaults_cpp(void)
{
	logger_config_t c = {};
	c.struct_size = sizeof(c);
	c.version = LOGGER_CONFIG_VERSION;
	c.level = LOGGER_INFO;
	c.detail = LOGGER_DETAIL_VERBOSE;
	c.outputs = LOGGER_OUT_STDERR;
	c.rotation.mode = LOGGER_ROTATE_SIZE_DAILY;
	c.rotation.max_file_size = 100u * 1024u * 1024u;
	c.rotation.retention_days = 30;
	c.file_mode = 0640;
	c.async_mode = 1;
	c.queue_capacity = 8192;
	c.flush_level = LOGGER_ERROR;
	c.include_pid = c.include_tid = c.include_source = 1;
	c.ident = "app";
	c.overflow[LOGGER_ERROR] = c.overflow[LOGGER_FATAL] =
		LOGGER_OVERFLOW_SYNC;
	c.syslog = LOGGER_SYSLOG_DEFAULT_CONFIG();
	return c;
}
#define LOGGER_DEFAULT_CONFIG() logger_defaults_cpp()
#else
#define LOGGER_DEFAULT_CONFIG()                                             \
	((logger_config_t){                                                 \
		.struct_size = sizeof(logger_config_t),                     \
		.version = LOGGER_CONFIG_VERSION,                           \
		.level = LOGGER_INFO,                                       \
		.detail = LOGGER_DETAIL_VERBOSE,                            \
		.outputs = LOGGER_OUT_STDERR,                               \
		.file_path = NULL,                                          \
		.rotation = { .mode = LOGGER_ROTATE_SIZE_DAILY,             \
			      .max_file_size = 100u * 1024u * 1024u,        \
			      .retention_days = 30 },                       \
		.file_mode = 0640,                                          \
		.async_mode = 1,                                            \
		.queue_capacity = 8192,                                     \
		.flush_level = LOGGER_ERROR,                                \
		.include_pid = 1,                                           \
		.include_tid = 1,                                           \
		.include_source = 1,                                        \
		.ident = "app",                                             \
		.overflow = { LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP,   \
			      LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP,   \
			      LOGGER_OVERFLOW_SYNC, LOGGER_OVERFLOW_SYNC }, \
		.syslog = LOGGER_SYSLOG_DEFAULT_CONFIG() })
#endif
#ifdef __cplusplus
}
#endif
#endif
