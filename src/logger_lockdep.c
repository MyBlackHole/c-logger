#include <stdbool.h>
#include "logger_lockdep.h"

#if LOGGER_ENABLE_LOCKDEP

#include <stdio.h>
#include <stdlib.h>

#define LOGGER_LOCKDEP_MAX_HELD 16u

struct logger_lockdep_entry {
	enum logger_lock_class class_id;
	const void *lock;
};

struct logger_lockdep_state {
	struct logger_lockdep_entry held[LOGGER_LOCKDEP_MAX_HELD];
	unsigned int depth;
};

static _Thread_local struct logger_lockdep_state state;

static const char *const class_names[LOGGER_LOCK_CLASS_NR] = {
	[LOGGER_LOCK_GLOBAL_CONTROL] = "global-control",
	[LOGGER_LOCK_GLOBAL_LIFETIME] = "global-lifetime",
	[LOGGER_LOCK_INSTANCE_EMIT] = "instance-emit",
	[LOGGER_LOCK_INSTANCE_PROGRESS] = "instance-progress",
	[LOGGER_LOCK_QUEUE_WAIT] = "queue-wait",
	[LOGGER_LOCK_CONSOLE] = "console",
	[LOGGER_LOCK_RUNTIME_FORMAT] = "runtime-format",
	[LOGGER_LOCK_RUNTIME_RESOURCE] = "runtime-resource",
};

void logger_lockdep_reset_child(void)
{
	/* 只清当前线程记账，不访问不继承映射中的旧锁。 */
	state.depth = 0;
}

const char *logger_lockdep_class_name(enum logger_lock_class class_id)
{
	if ((unsigned int)class_id >= LOGGER_LOCK_CLASS_NR)
		return "invalid";
	return class_names[class_id];
}

static void violation(const char *what, enum logger_lock_class class_id,
		      const void *lock)
{
	fprintf(stderr, "LOGGER_LOCKDEP: %s: class=%s lock=%p depth=%u\n", what,
		logger_lockdep_class_name(class_id), lock, state.depth);
	abort();
}

static bool is_instance(enum logger_lock_class class_id)
{
	return class_id == LOGGER_LOCK_INSTANCE_EMIT ||
	       class_id == LOGGER_LOCK_INSTANCE_PROGRESS ||
	       class_id == LOGGER_LOCK_QUEUE_WAIT;
}

static bool allowed_after(enum logger_lock_class held,
			  enum logger_lock_class next)
{
	/* fork 取得格式锁后再固定资源；禁止资源锁反向等待格式锁。 */
	if (next == LOGGER_LOCK_RUNTIME_RESOURCE)
		return held == LOGGER_LOCK_GLOBAL_CONTROL ||
		       held == LOGGER_LOCK_GLOBAL_LIFETIME ||
		       held == LOGGER_LOCK_INSTANCE_EMIT ||
		       held == LOGGER_LOCK_RUNTIME_FORMAT;
	if (next == LOGGER_LOCK_RUNTIME_FORMAT)
		return held == LOGGER_LOCK_GLOBAL_CONTROL ||
		       held == LOGGER_LOCK_GLOBAL_LIFETIME ||
		       held == LOGGER_LOCK_INSTANCE_EMIT ||
		       held == LOGGER_LOCK_CONSOLE;
	if (held == LOGGER_LOCK_GLOBAL_CONTROL)
		return next == LOGGER_LOCK_GLOBAL_LIFETIME || is_instance(next);
	if (held == LOGGER_LOCK_GLOBAL_LIFETIME)
		return is_instance(next);
	return false;
}

void logger_lockdep_check_acquire(enum logger_lock_class class_id,
				  const void *lock)
{
	if ((unsigned int)class_id >= LOGGER_LOCK_CLASS_NR || !lock)
		violation("invalid acquire", class_id, lock);
	for (unsigned int i = 0; i < state.depth; ++i) {
		if (state.held[i].lock == lock)
			violation("recursive/reacquire", class_id, lock);
	}

	if (state.depth) {
		enum logger_lock_class held =
			state.held[state.depth - 1].class_id;
		if (!allowed_after(held, class_id))
			violation("forbidden lock order", class_id, lock);
	}
	if (state.depth == LOGGER_LOCKDEP_MAX_HELD)
		violation("held-lock stack overflow", class_id, lock);
}

void logger_lockdep_acquire(enum logger_lock_class class_id, const void *lock)
{
	logger_lockdep_check_acquire(class_id, lock);
	state.held[state.depth++] =
		(struct logger_lockdep_entry){ .class_id = class_id,
					       .lock = lock };
}

void logger_lockdep_check_release(enum logger_lock_class class_id,
				  const void *lock)
{
	if (!state.depth)
		violation("release without acquire", class_id, lock);

	struct logger_lockdep_entry *top = &state.held[state.depth - 1];

	if (top->class_id != class_id || top->lock != lock)
		violation("non-LIFO/wrong-owner release", class_id, lock);
}

void logger_lockdep_release(enum logger_lock_class class_id, const void *lock)
{
	logger_lockdep_check_release(class_id, lock);
	--state.depth;
}

void logger_lockdep_abandon(enum logger_lock_class class_id, const void *lock)
{
	/* 锁状态已不确定且调用方已封闭对象；移除线程记账，避免污染后续锁序。 */
	for (unsigned int i = 0; i < state.depth; ++i) {
		if (state.held[i].class_id == class_id &&
		    state.held[i].lock == lock) {
			for (unsigned int j = i + 1; j < state.depth; ++j)
				state.held[j - 1] = state.held[j];
			--state.depth;
			return;
		}
	}
}

void logger_lockdep_assert_held(enum logger_lock_class class_id,
				const void *lock)
{
	for (unsigned int i = 0; i < state.depth; ++i) {
		if (state.held[i].class_id == class_id &&
		    state.held[i].lock == lock)
			return;
	}
	violation("assert-held failed", class_id, lock);
}

void logger_lockdep_assert_not_held(enum logger_lock_class class_id,
				    const void *lock)
{
	for (unsigned int i = 0; i < state.depth; ++i) {
		if (state.held[i].class_id == class_id &&
		    state.held[i].lock == lock)
			violation("assert-not-held failed", class_id, lock);
	}
}

#endif /* LOGGER_ENABLE_LOCKDEP */
