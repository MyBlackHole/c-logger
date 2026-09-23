#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { CONSOLE_COLOR_AUTO=0, CONSOLE_COLOR_ALWAYS, CONSOLE_COLOR_NEVER } console_color_mode_t;
typedef enum { CONSOLE_QUIET=0, CONSOLE_NORMAL=1, CONSOLE_VERBOSE=2, CONSOLE_DEBUG=3 } console_verbosity_t;
#define CONSOLE_CONFIG_VERSION 1u
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    console_verbosity_t verbosity;
    console_color_mode_t color;
} console_config_t;
#define CONSOLE_DEFAULT_CONFIG() ((console_config_t){.struct_size=sizeof(console_config_t),.version=CONSOLE_CONFIG_VERSION,.verbosity=CONSOLE_NORMAL,.color=CONSOLE_COLOR_AUTO})
/* Console shares the Logger/Audit process guard. Inherited child output returns
 * -1/ECHILD before stdio or mutex use; setters are no-ops with errno=ECHILD.
 * TTY predicates return 0 with errno=ECHILD. Exec before using a new runtime.
 * These are defensive errors, not a general async-signal-safe console API.
 * Console setters/init are process-global configuration operations.
 * Print functions return 0 on success, -1 on stdio failure with errno set by
 * the underlying libc operation where available. */
/* Console operations defer cancellation while formatting/holding internal
 * mutexes and FILE operations, restore the caller's policy after release, and
 * reject same-thread Logger/Console callback recursion with EDEADLK. Not a
 * signal-handler API or a bounded-time I/O interface. The host must not change
 * cancellation policy, pthread_exit/longjmp, or unload code inside a callback.
 * Whole init is an atomic config snapshot; invalid config/setter values set
 * EINVAL without changing it. debug_source returns EOVERFLOW instead of silently
 * truncating its bounded source/message buffers. NULL fmt is always EINVAL,
 * even when diagnostics would otherwise be filtered out.
 */
void console_init(const console_config_t *);
void console_set_verbosity(console_verbosity_t);
void console_set_color(console_color_mode_t);
int console_stdout_is_tty(void);
int console_stderr_is_tty(void);
/* Stable result/data channel: always stdout, no label/color. */
int console_print(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf,1,2)))
#endif
;
/* Human-facing diagnostics: stderr. INFO is hidden in QUIET; verbose/debug are gated. */
int console_info(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf,1,2)))
#endif
;
int console_warn(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf,1,2)))
#endif
;
int console_error(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf,1,2)))
#endif
;
int console_verbose(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf,1,2)))
#endif
;
int console_debug(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf,1,2)))
#endif
;
int console_debug_source(const char *file, int line, const char *func, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf,4,5)))
#endif
;
#define CONSOLE_DEBUG(...) console_debug_source(__FILE__,__LINE__,__func__,__VA_ARGS__)
#ifdef __cplusplus
}
#endif
#endif
