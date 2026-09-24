#ifndef LOGGER_CLEANUP_H
#define LOGGER_CLEANUP_H

/*
 * Internal Linux-style lexical resource management for c-logger.
 *
 * This is an independent user-space implementation of the same ownership
 * model used by Linux cleanup helpers. It is private to this project: no
 * installed header, public symbol, struct layout, or ABI depends on it.
 *
 * Scope cleanup covers ordinary C scope exit. It is not a replacement for
 * pthread cancellation cleanup, longjmp handling, signal safety, refcounting,
 * worker lifetime, or cross-thread ownership protocols.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__clang__)
#if !__has_attribute(cleanup)
#error "c-logger cleanup helpers require __attribute__((cleanup))"
#endif
#elif !defined(__GNUC__)
#error "c-logger cleanup helpers require GCC/Clang"
#endif

#define CLEANUP_ATTR(function) __attribute__((cleanup(function)))
#define CLEANUP_MUST_CHECK __attribute__((warn_unused_result))
#define CLEANUP_ALWAYS_INLINE inline __attribute__((always_inline))

#define __cleanup_concat_1(a, b) a##b
#define __cleanup_concat(a, b) __cleanup_concat_1(a, b)
#define __cleanup_unique(prefix) __cleanup_concat(prefix, __COUNTER__)

#define DEFINE_FREE(_name, _type, _release)                                \
	static CLEANUP_ALWAYS_INLINE void __free_##_name(void *__slot)            \
	{                                                                     \
		_type _T = *(_type *)__slot;                                   \
		_release;                                                        \
	}

#define __free(_name) CLEANUP_ATTR(__free_##_name)

#define __get_and_null(_value, _null_value)                                 \
	__extension__({                                                       \
		__typeof__(_value) *__cleanup_ptr = &(_value);                 \
		__typeof__(_value) __cleanup_value = *__cleanup_ptr;           \
		*__cleanup_ptr = (_null_value);                                 \
		__cleanup_value;                                                 \
	})

static CLEANUP_ALWAYS_INLINE CLEANUP_MUST_CHECK const volatile void *
__cleanup_must_check_ptr(const volatile void *value)
{
	return value;
}

#define no_free_ptr(_ptr)                                                    \
	((__typeof__(_ptr))__cleanup_must_check_ptr(                          \
		(const volatile void *)__get_and_null((_ptr), NULL)))

#define return_ptr(_ptr) return no_free_ptr(_ptr)
#define retain_and_null_ptr(_ptr) ((void)__get_and_null((_ptr), NULL))

/*
 * User-space fd ownership uses -1 as the released/invalid sentinel. Linux
 * uses -EBADF for its get_unused_fd class because that class carries errno
 * values in-band; ordinary POSIX descriptors in c-logger use -1.
 */
#define take_fd(_fd) __get_and_null((_fd), -1)

#define DEFINE_CLASS(_name, _type, _exit, _init, ...)                        \
	typedef _type class_##_name##_t;                                       \
	static CLEANUP_ALWAYS_INLINE void class_##_name##_destructor(_type *__slot)  \
	{                                                                     \
		_type _T = *__slot;                                             \
		_exit;                                                           \
	}                                                                     \
	static CLEANUP_ALWAYS_INLINE _type class_##_name##_constructor(__VA_ARGS__)  \
	{                                                                     \
		return (_init);                                                  \
	}

#define EXTEND_CLASS_COND(_name, _ext, _cond, _init, ...)                   \
	typedef class_##_name##_t class_##_name##_ext##_t;                    \
	static CLEANUP_ALWAYS_INLINE void                                             \
	class_##_name##_ext##_destructor(class_##_name##_t *__slot)            \
	{                                                                      \
		class_##_name##_t _T = *__slot;                                  \
		if (_cond)                                                         \
			return;                                                      \
		class_##_name##_destructor(__slot);                               \
	}                                                                      \
	static CLEANUP_ALWAYS_INLINE class_##_name##_t                                \
	class_##_name##_ext##_constructor(__VA_ARGS__)                          \
	{                                                                      \
		return (_init);                                                   \
	}

#define EXTEND_CLASS(_name, _ext, _init, ...)                               \
	EXTEND_CLASS_COND(_name, _ext, false, _init, __VA_ARGS__)

#define CLASS(_name, _var)                                                  \
	class_##_name##_t _var CLEANUP_ATTR(class_##_name##_destructor) =       \
		class_##_name##_constructor

#define CLASS_INIT(_name, _var, _init_expr)                                 \
	class_##_name##_t _var CLEANUP_ATTR(class_##_name##_destructor) =       \
		(_init_expr)

#define __scoped_class(_name, _var, _once, ...)                             \
	for (int _once = 1; _once; _once = 0)                                \
		for (CLASS(_name, _var)(__VA_ARGS__); _once; _once = 0)

#define scoped_class(_name, _var, ...)                                      \
	__scoped_class(_name, _var, __cleanup_unique(__cleanup_once_),        \
		       __VA_ARGS__)

/*
 * Guard classes are independent wrappers rather than encoded error pointers.
 * This preserves Linux's source-level ownership model without importing
 * kernel ERR_PTR conventions into user space.
 */
#define DEFINE_GUARD(_name, _type, _lock, _unlock)                          \
	typedef _type lock_##_name##_t;                                        \
	typedef struct {                                                        \
		_type resource;                                                   \
		int err;                                                          \
	} class_##_name##_t;                                                   \
	static CLEANUP_ALWAYS_INLINE class_##_name##_t                               \
	class_##_name##_constructor(_type _T)                                  \
	{                                                                      \
		_lock;                                                            \
		return (class_##_name##_t){ .resource = _T, .err = 0 };          \
	}                                                                      \
	static CLEANUP_ALWAYS_INLINE void                                             \
	class_##_name##_destructor(class_##_name##_t *__guard)                  \
	{                                                                      \
		if (__guard->err)                                                  \
			return;                                                      \
		_type _T = __guard->resource;                                     \
		_unlock;                                                          \
	}                                                                      \
	static CLEANUP_ALWAYS_INLINE int                                              \
	class_##_name##_lock_err(class_##_name##_t *__guard)                    \
	{                                                                      \
		return __guard->err;                                               \
	}                                                                      \
	static CLEANUP_ALWAYS_INLINE bool                                             \
	class_##_name##_lock_acquired(class_##_name##_t *__guard)               \
	{                                                                      \
		return __guard->err == 0;                                          \
	}

static CLEANUP_ALWAYS_INLINE int __cleanup_normalize_lock_error(int rc)
{
	if (rc > 0)
		return -rc;
	if (rc < 0)
		return rc;
	return -EBUSY;
}

#define DEFINE_GUARD_COND_4(_name, _ext, _lock, _success)                   \
	typedef class_##_name##_t class_##_name##_ext##_t;                    \
	static CLEANUP_ALWAYS_INLINE class_##_name##_t                                \
	class_##_name##_ext##_constructor(lock_##_name##_t _T)                 \
	{                                                                      \
		int _RET = (_lock);                                                \
		int __err = (_success) ? 0 : __cleanup_normalize_lock_error(_RET);\
		return (class_##_name##_t){ .resource = _T, .err = __err };       \
	}                                                                      \
	static CLEANUP_ALWAYS_INLINE void                                             \
	class_##_name##_ext##_destructor(class_##_name##_t *__guard)            \
	{                                                                      \
		if (!__guard->err)                                                 \
			class_##_name##_destructor(__guard);                         \
	}                                                                      \
	static CLEANUP_ALWAYS_INLINE int                                              \
	class_##_name##_ext##_lock_err(class_##_name##_t *__guard)              \
	{                                                                      \
		return __guard->err;                                               \
	}                                                                      \
	static CLEANUP_ALWAYS_INLINE bool                                             \
	class_##_name##_ext##_lock_acquired(class_##_name##_t *__guard)         \
	{                                                                      \
		return __guard->err == 0;                                          \
	}

#define DEFINE_GUARD_COND_3(_name, _ext, _lock)                            \
	DEFINE_GUARD_COND_4(_name, _ext, _lock, _RET == 0)

#define __cleanup_get_5th(_1, _2, _3, _4, _name, ...) _name
#define DEFINE_GUARD_COND(...)                                              \
	__cleanup_get_5th(__VA_ARGS__, DEFINE_GUARD_COND_4,                  \
		          DEFINE_GUARD_COND_3)(__VA_ARGS__)

#define guard(_name) CLASS(_name, __cleanup_unique(__cleanup_guard_))
#define ACQUIRE(_name, _var) CLASS(_name, _var)
#define ACQUIRE_ERR(_name, _var) class_##_name##_lock_err((_var))

#define __scoped_guard(_name, _once, ...)                                   \
	for (int _once = 1; _once; _once = 0)                                \
		for (CLASS(_name, scope)(__VA_ARGS__);                         \
		     _once && class_##_name##_lock_acquired(&scope);           \
		     _once = 0)

#define scoped_guard(_name, ...)                                            \
	__scoped_guard(_name, __cleanup_unique(__cleanup_guard_once_),        \
		       __VA_ARGS__)

#define __scoped_cond_guard(_name, _fail, _once, ...)                       \
	for (int _once = 1; _once; _once = 0)                                \
		for (CLASS(_name, scope)(__VA_ARGS__); _once; _once = 0)       \
			if (!class_##_name##_lock_acquired(&scope)) {            \
				_fail;                                               \
			} else

#define scoped_cond_guard(_name, _fail, ...)                                \
	__scoped_cond_guard(_name, _fail,                                    \
			    __cleanup_unique(__cleanup_guard_once_),            \
			    __VA_ARGS__)

/* Common user-space resource definitions. */
static CLEANUP_ALWAYS_INLINE void __cleanup_release_free(void *ptr)
{
	int saved = errno;
	free(ptr);
	errno = saved;
}

static CLEANUP_ALWAYS_INLINE void __cleanup_release_fd(int fd)
{
	int saved = errno;
	(void)close(fd);
	errno = saved;
}

static CLEANUP_ALWAYS_INLINE void __cleanup_release_file(FILE *file)
{
	int saved = errno;
	(void)fclose(file);
	errno = saved;
}

DEFINE_FREE(free, void *, if (_T) __cleanup_release_free(_T))
DEFINE_FREE(close_fd, int, if (_T >= 0) __cleanup_release_fd(_T))
DEFINE_FREE(fclose, FILE *, if (_T) __cleanup_release_file(_T))

DEFINE_CLASS(fd, int, if (_T >= 0) __cleanup_release_fd(_T), _fd, int _fd)
DEFINE_CLASS(file, FILE *, if (_T) __cleanup_release_file(_T), _file,
	     FILE *_file)

#endif
