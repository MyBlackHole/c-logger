#ifndef LOGGER_SCOPE_H
#define LOGGER_SCOPE_H

/* Private, thread-local reentry/cancellation scope. No reference counting and
 * no ownership of a caller's logger_t. Never use from a signal handler. */
typedef struct {
	int old_cancel;
	int saved_errno;
} logger_scope_t;

int logger_scope_busy(void);
int logger_scope_begin(logger_scope_t *scope); /* 0 / -errno */
int logger_scope_end(logger_scope_t *scope, int rc); /* restore errno/state */
/* Worker lifetime marker: workers are stopped cooperatively, never canceled. */
void logger_scope_worker_enter(void);
void logger_scope_worker_leave(void);
#endif
