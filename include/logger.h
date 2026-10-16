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
	uint64_t dropped[6]; /* 仅统计队列满时的 DROP；不含同步回退 */
	uint64_t queue_high_watermark;
	uint64_t enqueued;
	uint64_t sync_fallbacks;
	uint64_t consumer_batches;
	uint64_t consumer_records; /* 已出队的记录数，并非已完成输出的记录数 */
} logger_metrics_t;
/* 独立扩展：logger_metrics_t 保留原有二进制布局。
 * 实时快照可能跨越并发更新。生产者停止产生日志后执行 flush，
 * 有 completed == emitted + failed。失败表示未能确认所有选定输出均成功
 * （部分完成的批次中可能已有字节写入）。 */
typedef struct {
	uint64_t async_completed;
	uint64_t sync_completed;
	uint64_t emitted_records;
	uint64_t failed_records;
	int first_error; /* 正值 errno；在实例存续期间不会清除 */
} logger_io_metrics_t;

/* 统一的低开销可观测性快照。
 *
 * 此快照不替代 logger_metrics_t/logger_io_metrics_t：
 * 这些布局为现有调用者保持不变。诊断信息增加当前队列/工作线程状态及
 * 生命周期内的客观观测结果，不获取后端锁，不执行 I/O、刷新、重连或
 * 清除错误。实时字段是无数据竞争的近似快照，可能跨越并发更新。
 */
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

/* 一致的 File 后端快照，在实例的输出互斥锁保护下获取。
 * 计数器仅覆盖选定的文件输出端；stderr/Syslog 单独统计。
 * bytes_written 包括后续写入失败前已提交的部分字节。
 * last_error 是最近一次文件操作的结果（文件操作成功后为 0）；
 * 各类 last_* 错误字段保留该类别最近一次失败。
 */
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

/* 本地 Unix 数据报 Syslog。没有每个输出端的暂存区或阻塞重试队列。
 * REQUIRED 保留创建时的连接失败；DEFERRED 仅允许端点暂时不可用，
 * 并通过指标和不会自动清除的错误状态暴露该情况。
 */
typedef enum {
	LOGGER_SYSLOG_START_REQUIRED = 0,
	LOGGER_SYSLOG_START_DEFERRED = 1
} logger_syslog_start_t;
/* 带命名空间的 facility 值避免依赖 <syslog.h>，其中的 LOG_* 宏
 * 会与旧版全局便捷宏冲突。其他标准 facility 编号 0..23
 * 以 (number * 8) 的形式接受。 */
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
	const char *path; /* NULL：/dev/log；创建时复制 */
	unsigned facility; /* 带命名空间的 facility 或标准编号 * 8 */
	uint32_t reconnect_interval_ms; /* 0：1000 ms；其他值为 1..60000 */
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

/* 在输出互斥锁保护下获取快照。connected 表示本地关联，
 * 并非对端健康检查。发送成功并非持久化送达确认。
 * failed_records 包括背压、冷却期拒绝及格式错误；
 * backpressure_records/cooldown_records 是其子集。
 * 初始延迟连接失败会改变 last_error 和实例的粘滞 first_error，
 * 不改变记录计数。
 */
typedef struct {
	uint64_t submitted_records;
	uint64_t sent_records;
	uint64_t failed_records;
	uint64_t backpressure_records;
	uint64_t cooldown_records;
	uint64_t connect_attempts; /* 一轮 socket+connect，包括首次连接 */
	uint64_t connect_failures;
	uint64_t reconnect_successes; /* 首轮之后成功的连接轮次 */
	uint64_t send_attempts; /* 系统调用次数，包括有界 EINTR 重试 */
	uint64_t interrupted_calls;
	uint64_t close_failures;
	int connected;
	int last_error; /* 最近一次操作结果；发送成功后清除 */
	int last_close_error; /* 最近一次清理失败；不会因发送而清除 */
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
	/* v1 结构的一部分。未来扩展追加在当前结构之后，
	 * 传给旧版库时必须为零。 */
	logger_syslog_config_t syslog;
} logger_config_t;
#define LOGGER_CONFIG_SIZE 144u
/* 所有权与线程约定：
 * - logger_create() 返回具有所有权的实例；logger_destroy() 释放它。
 * - cfg 仅在创建期间借用；不得并发修改。
 * - 业务 .so 借用宿主的实例；仅所有者可以销毁。
 * - 每个异步实例拥有独立的工作线程和队列。仅加载 .so 不会启动工作
 *   线程、打开日志或初始化全局门面。
 * - 在实例存续期间，日志记录、级别设置、指标查询和刷新可以并发调用。
 * - 所有者必须先停止并等待所有显式实例使用者结束，再销毁实例。
 * 错误约定：返回指针的创建操作使用 NULL + errno；除非另有明确说明，
 * 返回 int 的公开操作成功时返回 0，失败时返回 -1 并设置 errno。
 */
/* 普通文件是单一所有者的目标（<basename>.logger.lock）。第二个独立
 * 所有者会收到 EBUSY：应共享同一个存活实例。目录解析在创建时绑定；
 * 拒绝最终路径组件为符号链接或硬链接的情况。
 * 内部轮转需要支持不覆盖目标的重命名。兼容性、资源开销及平台限制见
 * docs/FILE_BACKEND.md。 */
/* 显式调用会保护库拥有的资源，直到关键工作、va_end 和解锁完成，
 * 在此之前不受线程取消影响。这不意味着支持回滚或 I/O 时限。
 * 同一线程内嵌套的资源操作（包括另一实例、Console 及异步工作线程
 * 的回调）在加锁或产生副作用前以 EDEADLK 拒绝；返回状态的 sync_log
 * 保持 -errno 约定。这不保证信号安全或一般的异步取消安全。
 *
 * create 在交出指针前设有延迟取消检查点：待处理的取消请求会销毁
 * 尚未返回的候选实例。取消处于 ENABLED 且类型为 ASYNCHRONOUS 时，
 * 在分配实例/后端资源之前以 ENOTSUP 失败（可能已注册进程保护）；
 * 应在指针所有权转移期间禁用取消。调用者拥有所有成功返回的指针，
 * 并且必须为之后的取消安排清理。
 */
/* 显式初始化当前进程的运行域，不创建日志实例。
 * 初次调用或有有效 atfork 快照的子方重建成功返回0；重复为-EALREADY，
 * 并发为-EAGAIN，注册重入为-EDEADLK；失败返回负错误码并保留errno。
 * 子方必须在fork返回后的普通上下文、新业务线程创建前显式调用；
 * 不从任何atfork处理函数调用本接口，不复用继承的实例指针。
 * 无可信快照（原始系统调用、注册窗口等）仍-ECHILD，不能绕过检查。
 * Linux/glibc候选实现，不承诺多线程fork后的POSIX通用安全；宿主libc/
 * 区域设置等外部状态尚需专项验证，见docs/v2/FORK_RUNTIME.md。
 */
LOGGER_API int logger_process_init(void);

LOGGER_API logger_t *logger_create(const logger_config_t *);
LOGGER_API void logger_context_set(const logger_context_t *);
LOGGER_API void logger_context_clear(void);
LOGGER_API logger_context_t logger_context_get(void);

/* 显式的秘密信息处理辅助函数。日志器不会检查任意 printf 文本中的
 * 秘密信息。调用者必须先脱敏，再记录敏感数据。 */
LOGGER_API const char *logger_redact(void);
LOGGER_API int logger_mask_secret(const char *value, char *out, size_t out_size,
				  size_t visible_prefix, size_t visible_suffix);

LOGGER_API void logger_log_source(logger_t *, logger_level_t, logger_source_t,
				  const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
	;

/* 仅限独占所有者：先停止并等待所有借用者结束。返回 0 或 -1+errno。
 * 普通的最终 I/O/关闭失败仍会消耗并释放实例。
 * 生命周期安全性校验失败（工作线程等待结束、同步设施销毁或内部计数
 * 不变量）也会消耗公开所有权，但可能有意在内部保留已退役的分配，
 * 以免发生 UAF。除进入前被拒绝（如 ECHILD/EDEADLK）外，
 * 一旦销毁已实际开始，任何情况下都不得重试或复用该指针。
 * NULL 视为成功。销毁期间延迟取消。没有隐式引用计数。 */
LOGGER_API int logger_destroy_status(logger_t *);
/* 兼容包装函数，丢弃销毁状态。 */
/* 释放结束时待处理的取消可能在指针已释放后触发宿主清理。
 * 在有效的独占所有权边界调用 destroy 之前，必须先从宿主清理责任中
 * 移除此指针，或在清空指针并释放期间禁用取消。嵌套调用/子进程拒绝
 * 发生在释放之前。不得仅因最终 I/O 返回错误而重试已释放的指针。 */
LOGGER_API void logger_destroy(logger_t *);
LOGGER_API logger_state_t logger_get_state(const logger_t *);
/* 普通 void 日志操作最多保留 4095 字节的消息。格式化超出此上限时，
 * 保留的文本以 " [truncated]" 结尾。这是字节边界，不保证 UTF-8 码点
 * 完整；不会增加逐记录分配。文本以 NUL 结尾，不支持二进制安全。
 * 若元数据耗尽普通输出行的空间，该行也会在 LF 前以此标记结尾。
 * logger_log_sync_status() 则以 -EOVERFLOW 拒绝溢出，不输出记录。
 * 下文的元数据上限单独计算。
 * 输入字符串必须在整个调用期间有效。异步记录仅对 detail/include_*
 * 所需的元数据建立快照；需要 module/source 时，会复制模块名
 * （127 字节）、源文件基本名（255 字节）和函数名（127 字节）。
 * 仅当该详细级别能够输出 DEBUG 上下文时，才复制该上下文。
 * 较长的来源标签会用 '~' 明示缩短；后续格式化所需的任何排队指针，
 * 均不会保留指向可卸载业务 .so 中存储空间的引用。
 * 这不允许在未完成的调用及日志工作线程结束前卸载 liblogger 本身
 * 或回调提供方。 */
LOGGER_API void logger_log(logger_t *, logger_level_t, const char *,
			   const char *, int, const char *, const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 7, 8)))
#endif
	;
/* 高级同步状态路径。内部返回 0 或负值 errno；
 * 为兼容性保留；新应用代码通常应使用
 * logger_log()/LOG_* 和 logger_flush_instance()。 */
LOGGER_API int logger_log_sync_status(logger_t *, logger_level_t, const char *,
				      const char *, int, const char *,
				      const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 7, 8)))
#endif
	;
/* 返回状态的 flush 等待进入时捕获的队列预留水位，包括已经出队但
 * 尚未输出的批次，然后对文件后端执行 fsync。并发调用也可能被纳入；
 * 因队列满而丢弃的记录不属于已接受记录。flush 之前成功的同步日志
 * 调用也在保障范围内。返回 0，或 -1 + errno。此前的输出/同步失败
 * 仍保留在错误状态中，不会自动清除。
 * 执行器生命周期失败也会以该错误终止正在进行的等待；不会确认或
 * 重放未完成的预留。停止接纳后进入的调用者会收到 ESHUTDOWN。
 * 失败并不证明已静止：销毁实例前须停止并等待所有借用者结束。
 * 这不是远端 syslog 持久化确认，也不提供 I/O 超时。
 * 调用者必须始终保护显式实例的生命周期。 */
LOGGER_API int logger_flush_instance_status(logger_t *);
/* 兼容包装函数：执行相同等待，丢弃状态；NULL 时不执行操作。 */
LOGGER_API void logger_flush_instance(logger_t *);
LOGGER_API int logger_reopen_instance(logger_t *);
LOGGER_API void logger_set_instance_level(logger_t *, logger_level_t);
LOGGER_API uint64_t logger_dropped(const logger_t *);
LOGGER_API void logger_get_metrics(const logger_t *, logger_metrics_t *);
LOGGER_API void logger_get_io_metrics(const logger_t *, logger_io_metrics_t *);
/* 无锁且不获取后端锁的可观测性快照。无效、重入或子进程访问返回
 * 全零快照，并遵循现有指标接口的 errno 行为。调用者仍须保护
 * logger_t 的生命周期，避免并发销毁。 */
LOGGER_API void logger_get_diagnostics(const logger_t *, logger_diagnostics_t *);
/* 一致的 File 后端快照。返回 0 或 -1+errno；未选择文件输出时返回
 * ENOTSUP。不执行 flush/fsync/reopen，快照为只读。 */
LOGGER_API int logger_get_file_metrics(logger_t *, logger_file_metrics_t *);
/* 要求实例存活；与其他控制操作一样，由外部保护生命周期。
 * 返回 0 或 -1+errno。未选择 Syslog 时返回 ENOTSUP；出错时输出不变。
 * 不执行连接、重试、刷新，也不重置粘滞 I/O 错误。 */
LOGGER_API int logger_get_syslog_metrics(logger_t *, logger_syslog_metrics_t *);
/* 可选的应用全局门面。并发的数据/控制调用固定一个代次，绝不会
 * 静默操作更新的实例。启动期间返回 EAGAIN；关闭期间、关闭之后或
 * 固定的代次已失效时返回 ESHUTDOWN；没有初始实例时返回 ENODEV
 * （仅 LOG_* 保留旧版启动阶段的 stderr 回退）。
 * 同一线程内嵌套的全局调用以 EDEADLK 失败。不保证信号安全。
 * 全局调用将取消延迟到所有库锁释放及代次保护解除之后；取消不会
 * 回滚输出或已完成的初始化/关闭，也不会施加 I/O 超时。
 * 显式实例操作使用上述内部取消/重入作用域。
 */
LOGGER_API int logger_init(const logger_config_t *);
/* 将停止接纳、排空、等待线程结束、关闭和释放所有权串行化。
 * 对同一代次重复关闭会成功；延迟的关闭绝不会销毁更新的代次
 * （ESHUTDOWN）。I/O 失败时仍会释放旧实例；错误仅向控制者返回
 * 一次，不返回可重试的指针。与 void 包装函数不同，
 * 此接口允许检查最终同步/关闭结果。
 */
LOGGER_API int logger_shutdown_status(void);
LOGGER_API void logger_shutdown(void);
/* 全局门面提供相同保证 */
LOGGER_API int logger_flush_status(void);
LOGGER_API void logger_flush(void); /* 兼容包装函数；不返回错误 */
LOGGER_API int logger_reopen(void);
/* fork进程边界：父方继续使用原实例；有效prepare快照的子方先显式
 * logger_process_init，再按子方配置创建新实例。所有继承实例指针在子方
 * 失效，不能记录/刷新/普通销毁；不承诺诊断重初始化后的悬垂指针。
 * 子方处理函数只关闭库拥有的fd引用、失效根指针，不操作旧锁或队列。
 * 原始系统调用、_Fork/vfork、信号处理函数fork、库内回调重入及不可靠
 * 资源边界不属于重建入口。未初始化子方普通调用仍提前ECHILD拒绝。
 * 本功能的平台与外部libc调用链验收仍未完成，不能视作最终发布批准。
 */
/* 库绝不调用 fork，也不扫描宿主的线程列表。 */

LOGGER_API void logger_set_level(logger_level_t);
LOGGER_API uint64_t logger_global_dropped(void);
LOGGER_API void logger_get_global_metrics(logger_metrics_t *);
LOGGER_API void logger_get_global_io_metrics(logger_io_metrics_t *);
LOGGER_API void logger_get_global_diagnostics(logger_diagnostics_t *);
/* 通过当前全局代次获取相同快照。返回 0/-1+errno，
 * 并保持全局接纳/代次语义。 */
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
