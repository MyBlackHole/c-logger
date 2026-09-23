#include "logger_process.h"

#include <errno.h>
#include <limits.h>
#if LOGGER_ENABLE_LEGACY_FORK_HELPER
#include <dirent.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

/* Linux pid_t fits in int. Both handler state and identity must be lock-free;
 * no libatomic lock acquisition is permitted in the child rejection path. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "fork guard requires lock-free int");
static _Atomic int owner_pid;
static _Atomic int forked_child;
static _Atomic unsigned live_objects;
/* Nonzero only inside logger_fork_reinit after all objects have been destroyed.
 * The copied token is consumed independently in the two processes. */
#if LOGGER_ENABLE_LEGACY_FORK_HELPER
static _Atomic int clean_fork_owner;
#endif
static pthread_once_t register_once = PTHREAD_ONCE_INIT;
static int registration_error; /* published by pthread_once */

void logger_process_invalidate_child(void)
{
	/* Never overwrite/destroy pthread objects or abandon/free descriptors here.
     * The inherited runtime is left untouched until exec or _exit. */
	atomic_store_explicit(&forked_child, 1, memory_order_relaxed);
}

int logger_process_is_child(void)
{
	if (atomic_load_explicit(&forked_child, memory_order_relaxed))
		return 1;
	int owner = atomic_load_explicit(&owner_pid, memory_order_acquire);
	/* Also covers an application child handler registered before ours and the
     * registration window. This does not promise support for vfork/clone. */
	return owner != 0 && owner != (int)getpid();
}

static void register_guard(void)
{
	registration_error =
		pthread_atfork(NULL, NULL, logger_process_invalidate_child);
}

int logger_process_ensure(void)
{
	if (atomic_load_explicit(&forked_child, memory_order_relaxed))
		return -ECHILD;
	int owner = atomic_load_explicit(&owner_pid, memory_order_acquire);
	int current = (int)getpid();
	if (owner != 0 && owner != current)
		return -ECHILD;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(&owner_pid, &expected,
						      current,
						      memory_order_release,
						      memory_order_relaxed);
	if (expected != 0 && expected != current)
		return -ECHILD;
#if LOGGER_ENABLE_LEGACY_FORK_HELPER
	if (atomic_load_explicit(&clean_fork_owner, memory_order_acquire))
		return -EBUSY; /* legacy wrapper only */
#endif
	/* Publish identity before registration: never enter an inherited once lock
     * in a child that forked while another thread was installing the handler. */
	int rc = pthread_once(&register_once, register_guard);
	return rc ? -rc : -registration_error;
}

int logger_process_object_acquire(void)
{
	unsigned n = atomic_load_explicit(&live_objects, memory_order_relaxed);
	do {
		if (n == UINT_MAX)
			return -EOVERFLOW;
	} while (!atomic_compare_exchange_weak_explicit(
		&live_objects, &n, n + 1, memory_order_acq_rel,
		memory_order_relaxed));
	return 0;
}

void logger_process_object_release(void)
{
	(void)atomic_fetch_sub_explicit(&live_objects, 1, memory_order_acq_rel);
}

unsigned logger_process_object_count(void)
{
	return atomic_load_explicit(&live_objects, memory_order_acquire);
}

#if LOGGER_ENABLE_LEGACY_FORK_HELPER
int logger_process_thread_count(unsigned *out)
{
	if (!out)
		return -EINVAL;
	DIR *dir = opendir("/proc/self/task");
	if (!dir)
		return -errno;
	unsigned count = 0;
	int error = 0;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			error = errno;
			break;
		}
		const char *p = entry->d_name;
		if (!*p)
			continue;
		while (*p >= '0' && *p <= '9')
			++p;
		if (*p)
			continue;
		if (count == UINT_MAX) {
			error = EOVERFLOW;
			break;
		}
		++count;
	}
	if (closedir(dir) != 0 && !error)
		error = errno;
	if (error)
		return -error;
	if (!count)
		return -EIO;
	*out = count;
	return 0;
}

int logger_process_arm_clean_fork(void)
{
	if (logger_process_is_child())
		return -ECHILD;
	if (logger_process_object_count())
		return -EBUSY;
	int expected = 0;
	if (!atomic_compare_exchange_strong_explicit(
		    &clean_fork_owner, &expected, (int)getpid(),
		    memory_order_release, memory_order_relaxed))
		return -EALREADY;
	return 0;
}

int logger_process_finish_clean_fork(void)
{
	int prepared =
		atomic_load_explicit(&clean_fork_owner, memory_order_acquire);
	int owner = atomic_load_explicit(&owner_pid, memory_order_acquire);
	if (!prepared || prepared != owner || logger_process_object_count())
		return -ECHILD;
	/* The wrapper established a single-threaded, zero-object boundary. The
     * once object is already completed; every library mutex is unlocked. Do
     * not assign pthread initializers, destroy inherited locks, or reuse fds.
     * Do not call this from an application handler or outside the wrapper. */
	atomic_store_explicit(&owner_pid, (int)getpid(), memory_order_release);
	atomic_store_explicit(&forked_child, 0, memory_order_relaxed);
	atomic_store_explicit(&clean_fork_owner, 0, memory_order_release);
	return 0;
}

#endif
