#ifndef LOGGER_LOCKDEP_H
#define LOGGER_LOCKDEP_H

#include <stddef.h>

#ifndef LOGGER_ENABLE_LOCKDEP
#define LOGGER_ENABLE_LOCKDEP 0
#endif

enum logger_lock_class {
	LOGGER_LOCK_GLOBAL_CONTROL,
	LOGGER_LOCK_GLOBAL_LIFETIME,
	LOGGER_LOCK_INSTANCE_EMIT,
	LOGGER_LOCK_INSTANCE_PROGRESS,
	LOGGER_LOCK_QUEUE_WAIT,
	LOGGER_LOCK_CONSOLE,
	LOGGER_LOCK_RUNTIME_FORMAT,
	LOGGER_LOCK_RUNTIME_RESOURCE,
	LOGGER_LOCK_CLASS_NR
};

#if LOGGER_ENABLE_LOCKDEP

void logger_lockdep_reset_child(void);
void logger_lockdep_check_acquire(enum logger_lock_class class_id,
				  const void *lock);
void logger_lockdep_check_release(enum logger_lock_class class_id,
				  const void *lock);
void logger_lockdep_acquire(enum logger_lock_class class_id, const void *lock);
void logger_lockdep_release(enum logger_lock_class class_id, const void *lock);
void logger_lockdep_abandon(enum logger_lock_class class_id, const void *lock);
void logger_lockdep_assert_held(enum logger_lock_class class_id,
				const void *lock);
void logger_lockdep_assert_not_held(enum logger_lock_class class_id,
				    const void *lock);
const char *logger_lockdep_class_name(enum logger_lock_class class_id);

#else

static inline void logger_lockdep_reset_child(void)
{
}

static inline void logger_lockdep_check_acquire(enum logger_lock_class class_id,
						const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline void logger_lockdep_check_release(enum logger_lock_class class_id,
						const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline void logger_lockdep_acquire(enum logger_lock_class class_id,
					  const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline void logger_lockdep_release(enum logger_lock_class class_id,
					  const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline void logger_lockdep_abandon(enum logger_lock_class class_id,
					  const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline void logger_lockdep_assert_held(enum logger_lock_class class_id,
					      const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline void
logger_lockdep_assert_not_held(enum logger_lock_class class_id,
			       const void *lock)
{
	(void)class_id;
	(void)lock;
}

static inline const char *
logger_lockdep_class_name(enum logger_lock_class class_id)
{
	(void)class_id;
	return "disabled";
}

#endif

#endif
