#ifndef CONSOLE_H
#define CONSOLE_H
#include "logger_export.h"
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
	CONSOLE_COLOR_AUTO = 0,
	CONSOLE_COLOR_ALWAYS,
	CONSOLE_COLOR_NEVER
} console_color_mode_t;
typedef enum {
	CONSOLE_QUIET = 0,
	CONSOLE_NORMAL = 1,
	CONSOLE_VERBOSE = 2,
	CONSOLE_DEBUG = 3
} console_verbosity_t;
#define CONSOLE_CONFIG_VERSION 1u
#define CONSOLE_CONFIG_SIZE 16u
typedef struct {
	uint32_t struct_size;
	uint32_t version;
	console_verbosity_t verbosity;
	console_color_mode_t color;
} console_config_t;
#ifdef __cplusplus
static inline console_config_t console_defaults_cpp(void)
{
	console_config_t c = {};
	c.struct_size = sizeof(c);
	c.version = CONSOLE_CONFIG_VERSION;
	c.verbosity = CONSOLE_NORMAL;
	c.color = CONSOLE_COLOR_AUTO;
	return c;
}
#define CONSOLE_DEFAULT_CONFIG() console_defaults_cpp()
#else
#define CONSOLE_DEFAULT_CONFIG()                                      \
	((console_config_t){ .struct_size = sizeof(console_config_t), \
			     .version = CONSOLE_CONFIG_VERSION,       \
			     .verbosity = CONSOLE_NORMAL,             \
			     .color = CONSOLE_COLOR_AUTO })
#endif
/* Console 与 Logger 共用进程保护。子进程继承的输出接口在使用 stdio
 * 或互斥锁前返回 -1/ECHILD；设置接口不执行操作，并设置 errno=ECHILD。
 * TTY 判定返回 0，并设置 errno=ECHILD。使用新运行时前须执行 exec。
 * 这些是防御性错误，不代表通用的异步信号安全控制台 API。
 * Console 的设置/初始化是进程全局配置操作。
 * 打印函数成功时返回 0，stdio 失败时返回 -1，并在底层 libc 操作
 * 提供 errno 时保留该值。已关闭管道上的输出/刷新失败返回 -1/EPIPE，
 * 不会因 SIGPIPE 终止宿主。这仅使用调用线程的信号屏蔽字，随后恢复
 * 原屏蔽字，保留已待处理的 SIGPIPE，不改变进程的信号处理函数。
 * 保留 FILE 缓冲/错误状态，以及对宿主缓冲数据的正常刷新；
 * 较早的 stdio 错误优先于后续 EPIPE。
 * 宿主在 Console 之外直接执行的写入/刷新不受保护。 */
/* Console 操作在格式化、持有内部互斥锁及执行 FILE 操作期间延迟取消，
 * 释放后恢复调用者的策略，并以 EDEADLK 拒绝同一线程内递归的
 * Logger/Console 回调。这不是信号处理函数 API，也不是有时限的 I/O
 * 接口。宿主不得在回调内更改取消策略、调用 pthread_exit/longjmp
 * 或卸载代码。整个 init 以原子方式应用配置快照；无效的配置/设置值
 * 会设置 EINVAL，不改变配置。debug_source 返回 EOVERFLOW，
 * 而不会静默截断其有界的来源/消息缓冲区。fmt 为 NULL 时始终报
 * EINVAL，即使该诊断信息本来会被过滤。
 */
LOGGER_API void console_init(const console_config_t *);
LOGGER_API void console_set_verbosity(console_verbosity_t);
LOGGER_API void console_set_color(console_color_mode_t);
LOGGER_API int console_stdout_is_tty(void);
LOGGER_API int console_stderr_is_tty(void);
/* 稳定的结果/数据通道：始终使用 stdout，无标签或颜色。 */
LOGGER_API int console_print(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;
/* 面向人的诊断信息：stderr。QUIET 隐藏 INFO；verbose/debug 受级别控制。 */
LOGGER_API int console_info(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;
LOGGER_API int console_warn(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;
LOGGER_API int console_error(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;
LOGGER_API int console_verbose(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;
LOGGER_API int console_debug(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;
LOGGER_API int console_debug_source(const char *file, int line,
				    const char *func, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
	;
#define CONSOLE_DEBUG(...) \
	console_debug_source(__FILE__, __LINE__, __func__, __VA_ARGS__)
#ifdef __cplusplus
}
#endif
#endif
