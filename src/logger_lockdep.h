#ifndef LOGGER_LOCKDEP_H
#define LOGGER_LOCKDEP_H

#include <stddef.h>

#ifndef LOGGER_ENABLE_LOCKDEP
#define LOGGER_ENABLE_LOCKDEP 0
#endif

typedef enum {
	LOGGER_LOCK_GLOBAL_CONTROL,
	LOGGER_LOCK_GLOBAL_LIFETIME,
	LOGGER_LOCK_INSTANCE_EMIT,
	LOGGER_LOCK_INSTANCE_PROGRESS,
	LOGGER_LOCK_QUEUE_WAIT,
	LOGGER_LOCK_CONSOLE,
	LOGGER_LOCK_AUDIT_CONTROL,
	LOGGER_LOCK_AUDIT_OPERATION,
	LOGGER_LOCK_CLASS_NR
} logger_lock_class_t;

#if LOGGER_ENABLE_LOCKDEP

void logger_lockdep_acquire(logger_lock_class_t, const void *);
void logger_lockdep_release(logger_lock_class_t, const void *);
void logger_lockdep_assert_held(logger_lock_class_t, const void *);
void logger_lockdep_assert_not_held(logger_lock_class_t, const void *);
const char *logger_lockdep_class_name(logger_lock_class_t);

#else

static inline void logger_lockdep_acquire(logger_lock_class_t class_id,
					   const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline void logger_lockdep_release(logger_lock_class_t class_id,
					   const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline void logger_lockdep_assert_held(logger_lock_class_t class_id,
					       const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline void logger_lockdep_assert_not_held(logger_lock_class_t class_id,
						   const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline const char *logger_lockdep_class_name(logger_lock_class_t class_id)
{
	(void)class_id;
	return "disabled";
}

#endif

#endif
