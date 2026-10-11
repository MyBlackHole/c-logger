#ifndef LOGGER_SCOPE_H
#define LOGGER_SCOPE_H

/* 私有、线程局部的重入与取消作用域。不进行引用计数，
 * 也不拥有调用者的 logger_t。绝不能从信号处理函数使用。
 */
typedef struct {
	int old_cancel;
	int saved_errno;
} logger_scope_t;

int logger_scope_busy(void);
int logger_scope_begin(logger_scope_t *scope); /* 0 / -errno */
int logger_scope_end(logger_scope_t *scope, int rc); /* 恢复 errno/状态 */
/* 工作线程生命周期标记：工作线程协作停止，绝不被取消。 */
void logger_scope_worker_enter(void);
void logger_scope_worker_leave(void);
#endif
