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
/* Console 与 Logger/Audit 共享进程防护。继承子进程中的输出会在使用 stdio 或 mutex 之前
 * 返回 -1/ECHILD；设置接口直接空操作并设置 errno=ECHILD。TTY 判断返回 0 并设置 errno=ECHILD。
 * 必须先 exec，才能使用新的运行时。这些是防御性错误，并不代表提供通用异步信号安全 Console API。
 * Console 设置/初始化属于进程全局配置操作。打印函数成功返回 0；stdio 失败返回 -1，
 * 在底层 libc 能提供错误信息时沿用其 errno。 */
/* Console 在格式化、持有内部 mutex 以及执行 FILE 操作期间会延迟取消，
 * 释放资源后恢复调用方策略；同一线程 Logger/Console 回调递归会以 EDEADLK 拒绝。
 * 这不是信号处理器 API，也不是有界时延 I/O 接口。宿主不得在回调内修改取消策略、
 * 调用 pthread_exit/longjmp 或卸载代码。整个初始化视为原子配置快照；非法配置/设置值返回 EINVAL，
 * 并保持原配置不变。debug_source 在有界源信息/消息缓冲区容量不足时返回 EOVERFLOW，
 * 而不是静默截断。fmt 为 NULL 时始终返回 EINVAL，即使该诊断本来会因为过滤规则而不输出。 */
LOGGER_API void console_init(const console_config_t *);
LOGGER_API void console_set_verbosity(console_verbosity_t);
LOGGER_API void console_set_color(console_color_mode_t);
LOGGER_API int console_stdout_is_tty(void);
LOGGER_API int console_stderr_is_tty(void);
/* 稳定结果/数据通道：始终写 stdout，不添加标签/颜色。 */
LOGGER_API int console_print(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;
/* 面向人的诊断：写 stderr。QUIET 下隐藏 INFO；详细/调试输出受级别门控。 */
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
