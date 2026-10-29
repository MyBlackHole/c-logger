#ifndef LOGGER_INTERNAL_H
#define LOGGER_INTERNAL_H
#include "logger.h"
#include "logger_process.h"
#include "logger_scope.h"
#include "logger_queue.h"
#include "logger_file.h"
#include "logger_syslog.h"
#include <stdatomic.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include "logger_record.h"
const char *logger_level_name(logger_level_t level);
const char *logger_basename(const char *path);
size_t logger_format_line(const logger_t *l, const struct logger_message *m,
			  char *out, size_t cap);
/* 带确认的输出：不回退或截断；出错时长度为零。
 * 返回 0/-errno。调用者不得在失败后输出部分缓冲区。
 */
int logger_format_line_checked(const logger_t *l,
			       const struct logger_message *m, char *out,
			       size_t cap, size_t *length);
logger_detail_t logger_internal_detail(const logger_t *l);
int logger_internal_include_pid(const logger_t *l);
int logger_internal_include_tid(const logger_t *l);
int logger_internal_include_source(const logger_t *l);

struct logger_worker_workspace;

enum {
	LOGGER_CAPTURE_MODULE = LOGGER_RECORD_META_MODULE,
	LOGGER_CAPTURE_CONTEXT = LOGGER_RECORD_META_CONTEXT,
	LOGGER_CAPTURE_SOURCE = LOGGER_RECORD_META_SOURCE,
	LOGGER_CAPTURE_PID = 1u << 3,
	LOGGER_CAPTURE_TID = 1u << 4,
	LOGGER_CAPTURE_RECORD_MASK = LOGGER_RECORD_META_MODULE |
				     LOGGER_RECORD_META_CONTEXT |
				     LOGGER_RECORD_META_SOURCE
};

struct logger {
	/* 原子控制/状态字段，可在不持有 emit_mu 时读取。 */
	_Atomic logger_level_t level;
	_Atomic logger_state_t state;
	/* 默认槽的管理性借用标记，不是引用计数或在途调用计数。 */
	_Atomic int default_borrowed;

	/* 构造并发布成功后保持不变。 */
	logger_detail_t detail;
	unsigned int outputs;
	mode_t file_mode;
	int async_mode;
	int include_pid;
	int include_tid;
	int include_source;
	logger_level_t flush_level;
	/* 构造时按 detail/include_* 冻结；生产者不再每条日志重复推导。 */
	unsigned int capture_mask;
	pid_t pid;
	char ident[128];

	/* 后端可变状态统一由 emit_mu 串行化。 */
	struct logger_file file_backend;
	struct logger_syslog syslog_backend;
	pthread_mutex_t emit_mu;
	/* 完成事件与队列消费、emit_mu 相互独立。
	 * 任何线程都不能在持有 emit_mu 时等待 progress_cv。
	 */
	pthread_mutex_t progress_mu;
	pthread_cond_t progress_cv;
	size_t completed_pos; /* 队列的不包含上界的水位，由 progress_mu 保护 */
	_Atomic uint64_t async_completed;
	_Atomic uint64_t sync_completed;
	_Atomic uint64_t emitted_records;
	_Atomic uint64_t failed_records;
	_Atomic int first_error;
	/* 内部同步或生命周期失败；正 errno，错误状态不会自动清除。
	 * 与后端 first_error 不同，因为它控制能否安全拆除。
	 */
	_Atomic int
		lifecycle_error; /* 正 errno；实例生命周期内不会被后续成功清除 */
	/* 解锁失败后所有权不确定；销毁必须保留对象，不能再次操作该锁。 */
	_Atomic int synchronization_error;
	/* 异步队列：q 拥有紧凑槽位与预分配溢出池。
	 * 生产者与消费者的顺序由 slot.seq 原子发布协议保证；
	 * q.wait_mu 只负责睡眠与唤醒，不是负载发布锁。
	 */
	struct logger_queue q;
	/* 创建成功后由 logger_t 拥有；必须在工作线程退出并回收后才能释放。 */
	struct logger_worker_workspace *worker_workspace;
	/* 工作线程生命周期由 logger_t 拥有；必须协作停止并等待退出、回收。 */
	pthread_t worker;
	_Atomic int running;
	logger_overflow_policy_t overflow[6];
	_Atomic uint64_t dropped_by_level[6];
	_Atomic uint64_t enqueued;
	_Atomic uint64_t sync_fallbacks;
	_Atomic uint64_t queue_high_watermark;
	_Atomic uint64_t consumer_batches;
	_Atomic uint64_t consumer_records;
};

/* 原子内建可能在诊断前转换枚举实参；普通参数边界保留跨枚举警告。
 * 这些窄接口固定当前实例协议的内存序；值域和状态转换前提仍由调用方保证。
 */
static inline void logger_level_init(_Atomic logger_level_t *level,
				     logger_level_t value)
{
	atomic_init(level, value);
}

static inline void logger_state_init(_Atomic logger_state_t *state,
				     logger_state_t value)
{
	atomic_init(state, value);
}

static inline void logger_level_store_relaxed(_Atomic logger_level_t *level,
					      logger_level_t value)
{
	atomic_store_explicit(level, value, memory_order_relaxed);
}

static inline void logger_state_store_release(_Atomic logger_state_t *state,
					      logger_state_t value)
{
	atomic_store_explicit(state, value, memory_order_release);
}

static inline int logger_state_try_publish(_Atomic logger_state_t *state,
					   logger_state_t *expected,
					   logger_state_t desired)
{
	/* 直接转传期望值指针，失败时必须写回调用方观察到的实际状态。 */
	return atomic_compare_exchange_strong_explicit(state, expected, desired,
						       memory_order_acq_rel,
						       memory_order_acquire);
}

/* GCC 9 尚无枚举转换诊断，使用其已有的枚举比较诊断保留调用点类别。
 * 比较结果恒为 int，sizeof 不执行比较或实参；实际值只向内联函数传一次。
 * 检查放在调用前，保留旧 Clang 对直接枚举常量实参的转换诊断。
 * 括号函数名避免再次展开同名宏，指针约束仍由普通函数参数检查。
 */
#define logger_enum_check(type, value) ((void)sizeof((value) == (type)0))
#define logger_level_init(level, value)            \
	(logger_enum_check(logger_level_t, value), \
	 (logger_level_init)((level), (value)))
#define logger_state_init(state, value)            \
	(logger_enum_check(logger_state_t, value), \
	 (logger_state_init)((state), (value)))
#define logger_level_store_relaxed(level, value)   \
	(logger_enum_check(logger_level_t, value), \
	 (logger_level_store_relaxed)((level), (value)))
#define logger_state_store_release(state, value)   \
	(logger_enum_check(logger_state_t, value), \
	 (logger_state_store_release)((state), (value)))
#define logger_state_try_publish(state, expected, desired) \
	(logger_enum_check(logger_state_t, desired),       \
	 (logger_state_try_publish)((state), (expected), (desired)))

/* 内部完整拆除：调用者必须拥有独占所有权，返回 0 / -errno。
 * 普通 I/O 拆除错误仍完成最终释放；若线程回收、同步对象或存活对象计数
 * 无法证明已静止，则故意保留退役分配，绝不继续释放而造成释放后使用。
 * 拆除一旦开始，旧指针都不能被调用者当作可重试对象。
 */
int logger_dispose_internal(logger_t *l);
/* 供控制方调用正常 logger_destroy() 入口（含链接时包装函数）
 * 使用的当前线程最终释放回执。调用前重置；只有完成最终存活对象计数释放
 * 和 free 后才设置回执。
 */
void logger_destroy_receipt_reset(void);
int logger_destroy_receipt_read(void);
int logger_emit_status(logger_t *l, const struct logger_message *m,
		       int force_sync);
/* 消耗并推进 iovecs，同时保留调用者的 SIGPIPE 策略。 */
struct iovec;
int logger_stderr_writev_all(struct iovec *v, int count);
int logger_vlog_internal(logger_t *l, logger_level_t level, const char *module,
			 const char *file, int line, const char *function,
			 const char *fmt, va_list args);
struct logger_worker_workspace *
logger_worker_workspace_create(size_t queue_capacity);
void logger_worker_workspace_destroy(struct logger_worker_workspace *workspace);
void *logger_worker_main(void *p);
void logger_note_io_error(logger_t *l, int error);
void logger_note_lifecycle_error(logger_t *l, int error);
void logger_note_synchronization_error(logger_t *l, int error);
/* 调用者已拥有显式实例或全局的取消与生命周期作用域。
 * 返回 0/-errno，且只在获取 emit_mu 后写入 out。
 */
int logger_file_metrics_snapshot(logger_t *l, logger_file_metrics_t *out);
/* 内部：等待已排队的后端输出完成，但不执行 fsync。返回 0 / -errno。 */
int logger_wait_for_output(logger_t *l);
/* 调用时必须已经持有 emit_mu。 */
int logger_sync_outputs_locked(logger_t *l);

#endif
