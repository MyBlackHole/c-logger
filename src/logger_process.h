#ifndef LOGGER_PROCESS_H
#define LOGGER_PROCESS_H
#ifndef LOGGER_ENABLE_LEGACY_FORK_HELPER
#define LOGGER_ENABLE_LEGACY_FORK_HELPER 0
#endif

/* Private, process-wide ownership guard shared by Logger, Console and Audit.
 * ensure: parent-side registration only, 0 / -errno. Check child state before
 * pthread_once, locks, allocation or I/O. Identity is only rebound by a successfully prepared clean fork.
 * This is defensive rejection, not a general async-signal-safe logging API.
 */
int logger_process_ensure(void);
int logger_process_is_child(void);
void logger_process_invalidate_child(void);

/* Count every created/in-construction logger, including Audit's private logger.
 * Only create/destroy pay this cost; ordinary record submission is unchanged. */
int logger_process_object_acquire(void);
void logger_process_object_release(void);
unsigned logger_process_object_count(void);
#if LOGGER_ENABLE_LEGACY_FORK_HELPER
int logger_process_thread_count(unsigned *out); /* Linux /proc, 0 / -errno */
int logger_process_arm_clean_fork(void);
int logger_process_finish_clean_fork(void);

#endif /* legacy helper */
#endif
