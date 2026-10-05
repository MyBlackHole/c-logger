#ifndef LOGGER_SCOPE_H
#define LOGGER_SCOPE_H

/* 私有的线程本地重入/取消作用域。不做引用计数，也不拥有调用方的 logger_t。
 * 绝不能在信号处理器中使用。 */
typedef struct {
	int old_cancel;
	int saved_errno;
} logger_scope_t;

int logger_scope_busy(void);
int logger_scope_begin(logger_scope_t *scope); /* 返回 0 / -errno。 */
int logger_scope_end(logger_scope_t *scope, int rc); /* 恢复 errno/状态。 */
/* 工作线程生命周期标记：工作线程通过协作方式停止，绝不使用取消。 */
void logger_scope_worker_enter(void);
void logger_scope_worker_leave(void);
#endif
