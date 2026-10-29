#ifndef LOGGER_COMPILER_H
#define LOGGER_COMPILER_H

/*
 * c-logger 私有编译器约定。
 *
 * 这里只封装编译器能力，不定义所有权、生命周期或并发语义。
 * 公开头文件不得依赖本文件。
 */
#if defined(__clang__)
#if !__has_attribute(cleanup)
#error "c-logger requires compiler cleanup attribute support"
#endif
#elif !defined(__GNUC__)
#error "c-logger requires GCC or Clang"
#endif /* defined(__clang__) */

#define LOGGER_CLEANUP(_fn) __attribute__((cleanup(_fn)))
#define LOGGER_MUST_CHECK __attribute__((warn_unused_result))
#define LOGGER_MAYBE_UNUSED __attribute__((unused))
#define LOGGER_PRINTF(_fmt, _args) __attribute__((format(printf, _fmt, _args)))

#define LOGGER_STATIC_ASSERT(_expr, _msg) _Static_assert((_expr), _msg)
#define LOGGER_ARRAY_SIZE(_array) (sizeof(_array) / sizeof((_array)[0]))

#endif /* LOGGER_COMPILER_H */
