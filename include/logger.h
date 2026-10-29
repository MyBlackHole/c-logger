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
	unsigned int retention_days;
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
#endif /* __cplusplus */

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
/*
 * 独立扩展：logger_metrics_t 保留原有二进制布局。
 * 实时快照可能跨越并发更新。生产者停止产生日志后执行 flush，
 * 有 completed == emitted + failed。失败表示未能确认所有选定输出均成功
 * （部分完成的批次中可能已有字节写入）。
 */
typedef struct {
	uint64_t async_completed;
	uint64_t sync_completed;
	uint64_t emitted_records;
	uint64_t failed_records;
	int first_error; /* 正值 errno；在实例存续期间不会清除 */
} logger_io_metrics_t;

/*
 * 统一的低开销可观测性快照。
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
	unsigned int outputs;
	unsigned int observation_flags;
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

/*
 * 一致的 File 后端快照，在实例的输出互斥锁保护下获取。
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

/*
 * 本地 Unix 数据报 Syslog。没有每个输出端的暂存区或阻塞重试队列。
 * REQUIRED 保留创建时的连接失败；DEFERRED 仅允许端点暂时不可用，
 * 并通过指标和不会自动清除的错误状态暴露该情况。
 */
typedef enum {
	LOGGER_SYSLOG_START_REQUIRED = 0,
	LOGGER_SYSLOG_START_DEFERRED = 1
} logger_syslog_start_t;
/*
 * 带命名空间的 facility 值避免依赖 <syslog.h>，其中的 LOG_* 宏
 * 会与旧版全局便捷宏冲突。其他标准 facility 编号 0..23
 * 以 (number * 8) 的形式接受。
 */
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
	unsigned int facility; /* 带命名空间的 facility 或标准编号 * 8 */
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
#endif /* __cplusplus */

/*
 * 在输出互斥锁保护下获取快照。connected 表示本地关联，
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
	unsigned int outputs;
	const char *file_path;
	logger_rotation_config_t rotation;
	unsigned int file_mode;
	int async_mode;
	size_t queue_capacity;
	logger_level_t flush_level;
	int include_pid;
	int include_tid;
	int include_source;
	const char *ident;
	logger_overflow_policy_t overflow[6];
	/* 当前 ABI 2 的固定字段；未来扩展追加在当前结构之后。 */
	logger_syslog_config_t syslog;
} logger_config_t;
#define LOGGER_CONFIG_SIZE 144u
/*
 * 所有权与线程约定：
 * - logger_create() 成功时写出具有所有权的实例；logger_destroy() 释放它。
 * - logger_create 的 input 仅在创建期间借用；不得并发修改。
 * - 业务 .so 借用宿主的实例；仅所有者可以销毁。
 * - 每个异步实例拥有独立的工作线程和队列。仅加载 .so 不会启动工作
 *   线程、打开日志或初始化全局门面。
 * - 在实例存续期间，日志记录、级别设置、指标查询和刷新可以并发调用。
 * - 所有者必须先停止并等待所有显式实例使用者结束，再销毁实例。
 * 错误约定：进程初始化、create/destroy、默认槽、刷新/重开、记录、
 * 状态/指标查询、Console 与 TLS 上下文已统一 0/-errno，且保留入口 errno。
 * 脱敏辅助与级别设置也统一 0/-errno 并保留入口 errno。
 */
/*
 * 普通文件是单一所有者的目标（<basename>.logger.lock）。第二个独立
 * 所有者会收到 EBUSY：应共享同一个存活实例。目录解析在创建时绑定；
 * 拒绝最终路径组件为符号链接或硬链接的情况。
 * 内部轮转需要支持不覆盖目标的重命名。兼容性、资源开销及平台限制见
 * docs/FILE_BACKEND.md。
 */
/*
 * 显式调用会保护库拥有的资源，直到关键工作、va_end 和解锁完成，
 * 在此之前不受线程取消影响。这不意味着支持回滚或 I/O 时限。
 * 同一线程内嵌套的资源操作（包括另一实例、Console 及异步工作线程
 * 的回调）在加锁或产生副作用前以 EDEADLK 拒绝；返回状态的 sync_log
 * 保持 -errno 约定。这不保证信号安全或一般的异步取消安全。
 *
 * create 在交出指针前设有延迟取消检查点：待处理的取消请求会销毁
 * 尚未返回的候选实例。取消处于 ENABLED 且类型为 ASYNCHRONOUS 时，
 * 在分配实例/后端资源之前以 ENOTSUP 失败（进程域须已显式初始化）；
 * 应在指针所有权转移期间禁用取消。调用者拥有所有成功返回的指针，
 * 并且必须为之后的取消安排清理。
 */
/*
 * 显式初始化当前进程的运行域，不创建日志实例。
 * 所有使用进程域的 Logger、Global、Console 与上下文操作都以前置成功为条件。
 * 普通调用不隐式初始化、不等待、不重试：未初始化 ENODEV，进行中 EAGAIN，
 * 失败后保留上次初始化错误；各入口仍使用各自的返回/errno 形式。
 * 纯默认配置与脱敏辅助函数不依赖进程域。
 * 初次调用或有有效 atfork 快照的子方重建成功返回0；重复为-EALREADY，
 * 并发为-EAGAIN，注册重入为-EDEADLK；失败返回负错误码并保留errno。
 * 子方必须在fork返回后的普通上下文、新业务线程创建前显式调用；
 * 不从任何atfork处理函数调用本接口，不复用继承的实例指针。
 * 无可信快照（原始系统调用、注册窗口等）仍-ECHILD，不能绕过检查。
 * 仅限docs/v2/SUPPORT_MATRIX.md中的Linux/glibc基线，不承诺多线程fork后
 * 的POSIX通用安全。宿主须自行协调外部FILE、locale/时区、动态装载、
 * 自定义格式化与替换分配器等状态；不保证它们与fork并发时子方可立即
 * 重建。见docs/v2/FORK_RUNTIME.md。
 */
LOGGER_API int logger_process_init(void);

/*
 * 创建一个当前进程拥有的实例。返回 0/-errno，始终保留调用方 errno。
 * input 必须指向有效的配置，NULL 为 -EINVAL；不会隐式采用默认配置。
 * out 必须指向独占的、已初始化为 NULL 的可写拥有者槽；非空返回 -EBUSY，
 * 失败保持槽为 NULL，成功才发布完整实例。不得与 input 存储重叠。
 * 调用前必须显式 process_init；进程准入失败先于读取配置或拥有者槽。
 */
LOGGER_API int logger_create(const logger_config_t *input, logger_t **out);
/*
 * 当前线程的请求上下文；不分配堆存储，不拥有调用方字符串。
 * set 先把三个字段分别截断复制到线程局部的 63 字节缓冲，再发布指针，
 * 允许使用先前 get 的指针或在当前线程中交换字段作为 set 输入。
 * NULL set 等价于 clear；字段 NULL 或空串表示没有对应字段。
 * 返回 0/-errno，保留入口 errno。准入失败不修改上下文；
 * 取消恢复失败时 set/clear 的变更可能已经生效，不能据负值回滚。
 * get 只在完整成功后写输出槽；NULL 槽为 -EINVAL，其他失败保持全槽不变。
 * get 返回本线程存储的借用指针，不得跨线程或在线程退出后使用，
 * 下一次本线程 set/clear 将使先前快照中的字符串内容失效。
 */
LOGGER_API int logger_context_set(const logger_context_t *c);
LOGGER_API int logger_context_clear(void);
LOGGER_API int logger_context_get(logger_context_t *out);

/*
 * 显式的秘密信息处理辅助函数。日志器不会检查任意 printf 文本中的
 * 秘密信息。调用者必须先脱敏，再记录敏感数据。
 */
/* 不会失败，返回静态只读标记，不需要进程初始化或调用方释放。 */
LOGGER_API const char *logger_redact(void);
/*
 * 返回 0/-errno，保留入口 errno；此纯辅助函数不依赖进程域。
 * value 须为可读的 NUL 结尾字符串，NULL 表示输出 "[REDACTED]"。
 * out 须有 out_size 字节可写空间；NULL/零容量 -EINVAL，容量不足
 * -ENOSPC，结果长度溢出 -EOVERFLOW。所有失败保持整个输出缓冲原值。
 * 首尾长度按字节裁剪；若组合会完整披露原值，则只输出 "***"。
 * 支持原地及部分重叠，成功只写结果和终止符，不清除缓冲区剩余秘密。
 * 调用方必须检查状态，不能在失败后记录仍含原值的缓冲区。
 */
LOGGER_API int logger_mask_secret(const char *value, char *out, size_t out_size,
				  size_t visible_prefix, size_t visible_suffix);

/* 与 logger_log 相同的普通记录合同，来源参数使用值结构。 */
LOGGER_API int logger_log_source(logger_t *l, logger_level_t level,
				 logger_source_t source, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
	;

/*
 * 消费当前进程的实例拥有者，返回 0/-errno，始终保留调用方 errno。
 * owner == NULL 为 -EINVAL；*owner == NULL 是不获取资源的成功操作。
 * 调用前先停止并等待全部借用者。准入失败（如 ECHILD/EDEADLK/取消保护
 * 设置失败）不改变槽；准入成功后先清空 *owner，再排空、join、关闭和释放。
 * 后续 I/O 错误仍消费所有权；释放证明失败时内部保守保留对象，但公开
 * 指针同样已失效，不可重试。NULL 槽不是“物理释放已完成”的证明。
 * 清槽在取消恢复之前；其他别名也同时失效，不支持共享拥有者槽或并发销毁。
 */
LOGGER_API int logger_destroy(logger_t **owner);
/*
 * 状态和指标查询均返回 0/-errno，正常返回保留 errno；只有完整成功才
 * 写输出槽，所有失败保持原值。准入检查先于实例访问，NULL 参数 -EINVAL。
 * 允许观察仍然存活的 STOPPING/STOPPED 实例；调用者须保护其生命周期。
 */
LOGGER_API int logger_get_state(const logger_t *l, logger_state_t *out);
/*
 * 普通记录返回 0/-errno，所有正常返回保留调用方 errno，GNU %m 使用入口值。
 * 级别过滤是成功的无操作；非法参数和停止状态先于过滤检查。
 * 同步调用返回本次输出结果；异步成功表示入队，不表示已输出或持久化。
 * 容量不足且选择 DROP 时返回 -EAGAIN，计入丢弃；同步回退返回实际输出结果。
 * 负返回仍可能已发布或输出，如通知/解锁/取消恢复失败，不可据此直接重试。
 * 普通日志最多保留 4095 字节的消息。格式化超出此上限时，
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
 * 或回调提供方。
 */
LOGGER_API int logger_log(logger_t *l, logger_level_t level, const char *module,
			  const char *file, int line, const char *function,
			  const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 7, 8)))
#endif
	;
/*
 * 严格同步路径返回 0/-errno，正常返回保留 errno，GNU %m 使用入口值。
 * 正文或整行溢出返回 -EOVERFLOW，在任何后端输出前拒绝；成功记录直接输出，
 * 并对文件强制同步。它不等待先前异步记录的完成水位，也不确认远端持久化。
 * 普通记录与本路径的截断差异见 docs/v2/RECORD_API.md。
 */
LOGGER_API int logger_log_sync_status(logger_t *l, logger_level_t level,
				      const char *module, const char *file,
				      int line, const char *function,
				      const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 7, 8)))
#endif
	;
/*
 * 返回状态的 flush 等待进入时捕获的队列预留水位，包括已经出队但
 * 尚未输出的批次，然后对文件后端执行 fsync。并发调用也可能被纳入；
 * 因队列满而丢弃的记录不属于已接受记录。flush 之前成功的同步日志
 * 调用也在保障范围内。返回 0/-errno，所有正常返回保留调用方 errno。此前的输出/
 * 同步失败
 * 仍保留在错误状态中，不会自动清除。
 * 执行器生命周期失败也会以该错误终止正在进行的等待；不会确认或
 * 重放未完成的预留。停止接纳后进入的调用者会收到 ESHUTDOWN。
 * 失败并不证明已静止：销毁实例前须停止并等待所有借用者结束。
 * 这不是远端 syslog 持久化确认，也不提供 I/O 超时。
 * 调用者必须始终保护显式实例的生命周期。
 */
LOGGER_API int logger_flush(logger_t *l);
/*
 * 重新打开当前实例的文件后端。0/-errno，所有正常返回保留 errno。
 * 就绪后 NULL 或未选择文件输出返回 -EINVAL；不改变实例所有权。
 * 返回错误可能发生于文件切换之后，不保证没有 I/O 副作用或可安全重试。
 */
LOGGER_API int logger_reopen(logger_t *l);
/*
 * 显式级别设置：0/-errno，保留入口 errno；先做进程准入，
 * 再验证非空实例和 TRACE..OFF 范围。只执行 release-free 的 relaxed
 * 原子更新，无锁、不访问后端、不改变取消策略。允许在调用方证明仍
 * 存活的 STOPPING/STOPPED 实例上更新；不允许与销毁并发。
 * 失败不修改级别。
 */
LOGGER_API int logger_set_instance_level(logger_t *l, logger_level_t v);
LOGGER_API int logger_dropped(const logger_t *l, uint64_t *out);
LOGGER_API int logger_get_metrics(const logger_t *l, logger_metrics_t *out);
LOGGER_API int logger_get_io_metrics(const logger_t *l,
				     logger_io_metrics_t *out);
/*
 * 无锁且不获取后端锁的近似快照，不执行 I/O 或切换取消策略。
 * 同其他查询：0/-errno、保留 errno，完整成功才写输出，不保证跨字段一致。
 * 调用者仍须保护 logger_t 的生命周期，避免并发销毁。
 */
LOGGER_API int logger_get_diagnostics(const logger_t *l,
				      logger_diagnostics_t *out);
/*
 * 一致的 File 后端快照。0/-errno、保留 errno、完整成功才写输出。
 * 未选择文件输出时返回 -ENOTSUP。不执行 flush/fsync/reopen，快照为只读。
 */
LOGGER_API int logger_get_file_metrics(logger_t *l, logger_file_metrics_t *out);
/*
 * 要求实例存活；与其他控制操作一样，由外部保护生命周期。
 * 0/-errno、保留 errno。未选择 Syslog 时返回 -ENOTSUP；出错时输出不变。
 * 不执行连接、重试、刷新，也不重置粘滞 I/O 错误。
 */
LOGGER_API int logger_get_syslog_metrics(logger_t *l,
					 logger_syslog_metrics_t *out);
/*
 * 默认槽只借用普通实例，不创建、刷新或销毁实例。
 * logger_default_set(NULL, next) 绑定；(old, next) 原子替换；(old, NULL) 解绑。
 * 0/-errno，保留 errno。expected 与当前绑定不符或排队期间代次变化：-ESTALE。
 * 更换期间新便利调用返回 EAGAIN；解绑成功时旧的默认借用已经全部退出。
 * 没有默认实例时便利入口 ENODEV，不隐式输出 stderr。
 * 宿主必须保护 expected/replacement 的寿命并串行化其绑定与显式销毁；
 * 被借用期间 logger_destroy 拒绝 EBUSY，不消费拥有者。读者不取得所有权。
 * 解锁证明失败使默认槽失败关闭，相关实例可能永久保持借用，不能强行释放。
 * 取消恢复可能发生在绑定变更后；清理须按 expected 解绑后再销毁，不能仅依据
 * 函数是否正常返回来判断有无绑定。该接口不是取消点或信号安全接口。
 */
LOGGER_API int logger_default_set(logger_t *expected, logger_t *replacement);
/*
 * 默认槽借用一个代次执行同样的刷新/重开，均为 0/-errno 并保留 errno。
 * 空槽 -ENODEV，关闭准入 -EAGAIN，延迟调用代次失效 -ESHUTDOWN。
 * 返回前释放借用；失败不解绑、不销毁。错误不表示操作没有发生。
 */
LOGGER_API int logger_default_flush(void);
LOGGER_API int logger_default_reopen(void);
/*
 * fork进程边界：父方继续使用原实例；有效prepare快照的子方先显式
 * logger_process_init，再按子方配置创建新实例。所有继承实例指针在子方
 * 失效，不能记录/刷新/普通销毁；不承诺诊断重初始化后的悬垂指针。
 * 子方处理函数只关闭库拥有的fd引用、失效根指针，不操作旧锁或队列。
 * 原始系统调用、_Fork/vfork、信号处理函数fork、库内回调重入及不可靠
 * 资源边界不属于重建入口。未初始化子方普通调用仍提前ECHILD拒绝。
 * 有限宿主范围及其限制见docs/v2/FORK_RUNTIME.md；重建不会扩大该范围。
 */
/* 库绝不调用 fork，也不扫描宿主的线程列表。 */

/*
 * 当前默认实例的级别设置：0/-errno，保留入口 errno。
 * 先完成 Global 准入及参数校验，再固定当前代次并取得生命周期读锁；
 * 空默认槽 -ENODEV，代次转换中 -EAGAIN，失效代次 -ESHUTDOWN。
 * 无效级别 -EINVAL，未提交前的失败不改变默认实例或其他实例。
 * 成功仅修改当前实例的原子级别，不触发任何输出、刷新或持久化。
 * 已提交后解锁证明或取消恢复失败仍可能返回负值；不保证回滚。
 * 全局锁解锁证明失败将毒化绑定，不可据此销毁可能仍被借用的实例。
 */
LOGGER_API int logger_set_level(logger_level_t level);
/*
 * 默认查询在固定代次的借用完全归还后才写输出。0/-errno、保留 errno，
 * 所有失败保持输出槽原值；空槽 -ENODEV、转换中 -EAGAIN、代次失效 -ESHUTDOWN。
 */
LOGGER_API int logger_global_dropped(uint64_t *out);
LOGGER_API int logger_get_global_metrics(logger_metrics_t *out);
LOGGER_API int logger_get_global_io_metrics(logger_io_metrics_t *out);
LOGGER_API int logger_get_global_diagnostics(logger_diagnostics_t *out);
/* 通过当前默认代次获取相同的只读文件快照，遵循上述输出与借用规则。 */
LOGGER_API int logger_get_global_file_metrics(logger_file_metrics_t *out);
/*
 * 固定一个默认代次执行 logger_log 的普通记录合同。宏 LOG_* 传回该状态。
 * 默认生命周期锁释放证明失败优先返回安全错误，并保守保留借用。
 */
LOGGER_API int logger_global_write(logger_level_t level, const char *module,
				   const char *file, int line, const char *func,
				   const char *fmt, ...)
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
#endif /* __cplusplus */
#ifdef __cplusplus
}
#endif
#endif /* LOGGER_H */
