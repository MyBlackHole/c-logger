#ifndef EXAMPLE_SDK_H
#define EXAMPLE_SDK_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 这是示例业务 ABI，不是另一个 Logger API。业务 .so
 * 对 liblogger 没有二进制依赖，也不拥有任何 logger、线程或 fd。
 */
typedef struct example_sdk example_sdk_t;
typedef enum {
	EXAMPLE_LOG_DEBUG,
	EXAMPLE_LOG_INFO,
	EXAMPLE_LOG_ERROR
} example_log_level_t;
typedef struct {
	example_log_level_t level;
	const char *module;
	const char *file;
	const char *function;
	int line;
	const char *message; /* 回调期间借用，UTF-8 文本，不含 NUL */
	size_t message_len;
} example_log_record_t;
typedef void (*example_log_fn)(void *user, const example_log_record_t *record);
typedef struct {
	const char *name; /* 由 example_sdk_create 复制 */
	example_log_fn log; /* NULL 禁用诊断日志，无回退 */
	void *log_user; /* 借用；example_sdk_destroy 绝不释放它 */
} example_sdk_options_t;
int example_sdk_create(const example_sdk_options_t *, example_sdk_t **out);
/* 允许并发 run 调用。日志相对于调用者同步，
 * 不持有业务锁。回调必须线程安全；C++ 异常或 longjmp 不得逃逸。
 * 它不得销毁活跃的 SDK 实例。
 */
int example_sdk_run(example_sdk_t *, unsigned task);
/* 所有者先停止并回收全部调用线程。销毁返回后不再有回调。
 * 真实异步 SDK 也需要在此停止并回收自己的工作。
 */
void example_sdk_destroy(example_sdk_t *);
#ifdef __cplusplus
}
#endif
#endif
