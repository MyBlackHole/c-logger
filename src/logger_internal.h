#ifndef LOGGER_INTERNAL_H
#define LOGGER_INTERNAL_H
#include "logger.h"
#include "logger_process.h"
#include "logger_scope.h"
#include "logger_queue.h"
#include "logger_file.h"
#include "logger_syslog.h"
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include "logger_record.h"
const char *logger_level_name(logger_level_t);
const char *logger_basename(const char *);
size_t logger_format_line(const logger_t *, const logger_message_t *, char *,
			  size_t);
logger_detail_t logger_internal_detail(const logger_t *);
int logger_internal_include_pid(const logger_t *);
int logger_internal_include_tid(const logger_t *);
int logger_internal_include_source(const logger_t *);

typedef struct logger_worker_workspace logger_worker_workspace_t;

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

	/* 构造并 publish 成功后保持不变。 */
	logger_detail_t detail;
	unsigned outputs;
	mode_t file_mode;
	int async_mode, include_pid, include_tid, include_source;
	logger_level_t flush_level;
	/* 构造时按 detail/include_* 冻结；producer 不再每条日志重复推导。 */
	unsigned capture_mask;
	pid_t pid;
	char ident[128];

	/* backend 可变状态统一由 emit_mu 串行化。 */
	logger_file_t file_backend;
	logger_syslog_t syslog_backend;
	pthread_mutex_t emit_mu;
	/* completion 与 queue 消费、emit_mu 相互独立。
	 * 任何线程都不能在持有 emit_mu 时等待 progress_cv。 */
	pthread_mutex_t progress_mu;
	pthread_cond_t progress_cv;
	size_t completed_pos; /* queue 的 exclusive watermark，由 progress_mu 保护 */
	_Atomic uint64_t async_completed, sync_completed;
	_Atomic uint64_t emitted_records, failed_records;
	_Atomic int first_error; /* 正 errno；实例生命周期内 sticky，不被后续成功清除 */
	/* 异步 queue：q 拥有 compact slots 与预分配 spill pool。
	 * producer/consumer 顺序由 slot.seq atomic publication 协议保证；
	 * q.wait_mu 只负责 sleep/wakeup，不是 payload publication 锁。 */
	logger_queue_t q;
	/* create 成功后由 logger_t 拥有；必须在 worker join 后才能释放。 */
	logger_worker_workspace_t *worker_workspace;
	/* worker 生命周期由 logger_t 拥有；必须 cooperative stop + join。 */
	pthread_t worker;
	_Atomic int running;
	logger_overflow_policy_t overflow[6];
	_Atomic uint64_t dropped_by_level[6];
	_Atomic uint64_t enqueued, sync_fallbacks, queue_high_watermark;
	_Atomic uint64_t consumer_batches, consumer_records;
};
/* 内部完整 teardown：调用者必须拥有独占 ownership，返回 0 / -errno。
 * 与 logger_destroy 一样会释放对象；失败也不能把旧指针当作可重试对象。 */
/* Audit 在检查/修复目标前先取得 file ownership，随后把该 reservation
 * move 给同步 logger。 */
logger_t *logger_create_reserved_file(const logger_config_t *, logger_file_t *);
int logger_dispose_internal(logger_t *);
#if LOGGER_ENABLE_LEGACY_FORK_HELPER
int logger_global_stop_for_clean_fork(void);
#endif
int logger_file_offset(logger_t *, uint64_t *);
int logger_emit_status(logger_t *, const logger_message_t *, int);
void logger_vlog_internal(logger_t *, logger_level_t, const char *,
			  const char *, int, const char *, const char *,
			  va_list);
logger_worker_workspace_t *logger_worker_workspace_create(size_t);
void logger_worker_workspace_destroy(logger_worker_workspace_t *);
void *logger_worker_main(void *);
void logger_note_io_error(logger_t *, int);
/* 内部：等待已排队的 backend 输出完成，但不执行 fsync。返回 0 / -errno。 */
int logger_wait_for_output(logger_t *);
/* 调用时必须已经持有 emit_mu。 */
int logger_sync_outputs_locked(logger_t *);

#endif
