#include "logger_lockdep.h"

#if LOGGER_ENABLE_LOCKDEP

#include <stdio.h>
#include <stdlib.h>

#define LOGGER_LOCKDEP_MAX_HELD 16u

typedef struct {
	logger_lock_class_t class_id;
	const void *lock;
} logger_lockdep_entry_t;

typedef struct {
	logger_lockdep_entry_t held[LOGGER_LOCKDEP_MAX_HELD];
	unsigned depth;
} logger_lockdep_state_t;

static _Thread_local logger_lockdep_state_t state;

static const char *const class_names[LOGGER_LOCK_CLASS_NR] = {
	[LOGGER_LOCK_GLOBAL_CONTROL] = "global-control",
	[LOGGER_LOCK_GLOBAL_LIFETIME] = "global-lifetime",
	[LOGGER_LOCK_INSTANCE_EMIT] = "instance-emit",
	[LOGGER_LOCK_INSTANCE_PROGRESS] = "instance-progress",
	[LOGGER_LOCK_QUEUE_WAIT] = "queue-wait",
	[LOGGER_LOCK_CONSOLE] = "console",
	[LOGGER_LOCK_AUDIT_CONTROL] = "audit-control",
	[LOGGER_LOCK_AUDIT_OPERATION] = "audit-operation",
};

const char *logger_lockdep_class_name(logger_lock_class_t class_id)
{
	if ((unsigned)class_id >= LOGGER_LOCK_CLASS_NR)
		return "invalid";
	return class_names[class_id];
}

static void violation(const char *what, logger_lock_class_t class_id,
		      const void *lock)
{
	fprintf(stderr, "LOGGER_LOCKDEP: %s: class=%s lock=%p depth=%u\n",
		what, logger_lockdep_class_name(class_id), lock, state.depth);
	abort();
}

static int is_instance(logger_lock_class_t class_id)
{
	return class_id == LOGGER_LOCK_INSTANCE_EMIT ||
	       class_id == LOGGER_LOCK_INSTANCE_PROGRESS ||
	       class_id == LOGGER_LOCK_QUEUE_WAIT;
}

static int allowed_after(logger_lock_class_t held, logger_lock_class_t next)
{
	if (held == LOGGER_LOCK_GLOBAL_CONTROL)
		return next == LOGGER_LOCK_GLOBAL_LIFETIME;
	if (held == LOGGER_LOCK_GLOBAL_LIFETIME)
		return is_instance(next);
	if (held == LOGGER_LOCK_AUDIT_CONTROL)
		return next == LOGGER_LOCK_AUDIT_OPERATION;
	if (held == LOGGER_LOCK_AUDIT_OPERATION)
		return is_instance(next);
	return 0;
}

void logger_lockdep_acquire(logger_lock_class_t class_id, const void *lock)
{
	if ((unsigned)class_id >= LOGGER_LOCK_CLASS_NR || !lock)
		violation("invalid acquire", class_id, lock);
	for (unsigned i = 0; i < state.depth; ++i) {
		if (state.held[i].lock == lock)
			violation("recursive/reacquire", class_id, lock);
	}

	if (state.depth) {
		logger_lock_class_t held = state.held[state.depth - 1].class_id;
		if (!allowed_after(held, class_id))
			violation("forbidden lock order", class_id, lock);
	}
	if (state.depth == LOGGER_LOCKDEP_MAX_HELD)
		violation("held-lock stack overflow", class_id, lock);

	state.held[state.depth++] =
		(logger_lockdep_entry_t){ .class_id = class_id, .lock = lock };
}

void logger_lockdep_release(logger_lock_class_t class_id, const void *lock)
{
	if (!state.depth)
		violation("release without acquire", class_id, lock);

	logger_lockdep_entry_t *top = &state.held[state.depth - 1];
	if (top->class_id != class_id || top->lock != lock)
		violation("non-LIFO/wrong-owner release", class_id, lock);
	--state.depth;
}

void logger_lockdep_assert_held(logger_lock_class_t class_id, const void *lock)
{
	for (unsigned i = 0; i < state.depth; ++i) {
		if (state.held[i].class_id == class_id &&
		    state.held[i].lock == lock)
			return;
	}
	violation("assert-held failed", class_id, lock);
}

void logger_lockdep_assert_not_held(logger_lock_class_t class_id,
				    const void *lock)
{
	for (unsigned i = 0; i < state.depth; ++i) {
		if (state.held[i].class_id == class_id &&
		    state.held[i].lock == lock)
			violation("assert-not-held failed", class_id, lock);
	}
}

#endif
