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

struct logger {
	_Atomic logger_level_t level;
	_Atomic logger_state_t state;
	logger_detail_t detail;
	unsigned outputs;
	logger_file_t file_backend;
	logger_syslog_t syslog_backend;
	mode_t file_mode;
	int async_mode, include_pid, include_tid, include_source;
	logger_level_t flush_level;
	char ident[128];
	pthread_mutex_t emit_mu;
	/* Completion is independent of queue consumption and of the emit lock.
     * No thread waits on progress_cv while holding emit_mu. */
	pthread_mutex_t progress_mu;
	pthread_cond_t progress_cv;
	size_t completed_pos; /* exclusive queue watermark, under progress_mu */
	_Atomic uint64_t async_completed, sync_completed;
	_Atomic uint64_t emitted_records, failed_records;
	_Atomic int first_error; /* positive errno, sticky for this instance */
	logger_queue_t q;
	pthread_t worker;
	_Atomic int running;
	logger_overflow_policy_t overflow[6];
	_Atomic uint64_t dropped_by_level[6];
	_Atomic uint64_t enqueued, sync_fallbacks, queue_high_watermark;
	_Atomic uint64_t consumer_batches, consumer_records;
};
/* Private complete teardown: requires exclusive ownership. 0 / -errno.
 * Like logger_destroy, frees the object; failure is not a retryable pointer. */
/* Audit reserves file ownership before it is allowed to inspect/repair the
 * destination, then moves the reservation into its synchronous logger. */
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
void *logger_worker_main(void *);
void logger_note_io_error(logger_t *, int);
/* Private: wait for queued backend output, without fsync. 0 / -errno. */
int logger_wait_for_output(logger_t *);
/* Called with emit_mu held. */
int logger_sync_outputs_locked(logger_t *);

#endif
