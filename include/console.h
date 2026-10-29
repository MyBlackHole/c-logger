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
#endif /* __cplusplus */
/*
 * 所有 Console 接口成功返回 0，失败返回负 errno；正常返回均保留调用方
 * errno。输出成功也包括按 verbosity 过滤的诊断，但 fmt==NULL 始终 EINVAL。
 * 不再提供 void 配置接口或返回布尔值的无参 TTY 查询。
 *
 * 宿主先调用 logger_process_init()；未就绪为 ENODEV，初始化进行中为 EAGAIN，
 * fork 子方重新初始化前为 ECHILD。同线程重入为 EDEADLK。准入失败先于配置/
 * 输出槽访问、stdio、取消策略或锁操作；这些接口不是异步信号安全 API。
 *
 * Console 是独立的终端展示通道，不创建、借用或销毁默认 Logger。init 只配置
 * 已就绪进程域。NULL 配置和非法字段为 EINVAL，原配置不变；需恢复默认值时
 * 显式传入 CONSOLE_DEFAULT_CONFIG()。成功的配置提交和输出不会因之后的
 * 取消策略恢复失败而撤销；主操作错误优先于收尾错误。
 *
 * 操作期间延迟取消，va_end 和资源释放后才恢复调用方策略。宿主不得在回调中
 * 更改取消策略、pthread_exit/longjmp 或卸载库。debug_source 拒绝超限缓冲，
 * 返回 EOVERFLOW。输出不是有时限 I/O，不提供持久化保证。
 *
 * FILE 缓冲及正常刷新语义不变。闭管道为 EPIPE，调用线程局部 SIGPIPE 保护
 * 保留原屏蔽字和待处理信号，不改变宿主信号处理函数；较早的 stdio 主错误优先。
 * 宿主在 Console 之外直接写入/刷新不受保护。printf 的 %m 使用入口 errno。
 */
LOGGER_API int console_init(const console_config_t *c);
LOGGER_API int console_set_verbosity(console_verbosity_t value);
LOGGER_API int console_set_color(console_color_mode_t value);
/*
 * 查询成功写入 0/1（非终端/终端），ENOTTY 是成功的非终端结果。
 * EBADF 等错误仍返回负错误码；包括收尾失败在内的任意失败不修改 *out。
 * out==NULL 为 EINVAL；调用方保证非空输出槽独占、存活且可写。
 */
LOGGER_API int console_stdout_is_tty(int *out);
LOGGER_API int console_stderr_is_tty(int *out);
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
#endif /* CONSOLE_H */
