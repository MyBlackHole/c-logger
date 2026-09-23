#ifndef LOGGER_FORK_COMPAT_H
#define LOGGER_FORK_COMPAT_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Application helper only. Not part of the default library. Existing users
 * must configure LOGGER_ENABLE_LEGACY_FORK_HELPER=ON (same old semantics).
 * Prefer host-owned process creation and explicit instance lifetimes. */
/* Controlled fork without exec, for a quiescent Linux application.
 * Before entry: stop/join every application thread that can use the library;
 * destroy ALL explicit logger_t instances and shut down Audit. Only the optional
 * default logger and its worker may remain. Do not create threads/use this
 * library in signal/atfork handlers during this call. Other libraries must also
 * be quiescent and permit single-threaded fork/continue.
 *
 * This function drains/syncs/destroys the default logger BEFORE fork, checks
 * /proc/self/task for a single-threaded boundary, then calls fork(). Both
 * return branches may call logger_init/create, console_init and audit_init.
 * Parent and child must reinitialize explicitly; old instances are not reused.
 * Runtime counters/locks are never reset in the child. Child request context
 * is cleared; parent request context and console settings are retained.
 *
 * Return: >0 child PID in parent, 0 in child, -1 + errno on failure. EBUSY
 * means another thread/instance remains. /proc must be available. Failure
 * after teardown (including fork failure) leaves the default logger shut down;
 * it is NOT automatically restored. Raw fork keeps the inherited-state guard.
 */
pid_t logger_fork_reinit(void);

#ifdef __cplusplus
}
#endif
#endif
