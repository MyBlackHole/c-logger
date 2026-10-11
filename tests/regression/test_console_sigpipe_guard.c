#define _GNU_SOURCE
#include "console.h"
#include "support.h"
#include <signal.h>

/* Rare guard setup/unlock failures are injected only into the white-box
 * archive. Production closed-pipe behavior has its own unwrapped test. */
static int armed;
static int fail_mask;
static int fail_pending;
static int fail_unlock;
static unsigned locks;
static unsigned unlocks;
static pthread_mutex_t *console_mutex;

int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	int rc = __real_pthread_mutex_lock(mutex);
	if (armed && !rc) {
		if (!console_mutex)
			console_mutex = mutex;
		CHECK(mutex == console_mutex);
		++locks;
	}
	return rc;
}

int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	int rc = __real_pthread_mutex_unlock(mutex);
	if (armed && mutex == console_mutex) {
		++unlocks;
		if (fail_unlock) {
			fail_unlock = 0;
			return EPERM;
		}
	}
	return rc;
}

int __real_pthread_sigmask(int, const sigset_t *, sigset_t *);
int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
	if (armed && how == SIG_BLOCK && fail_mask) {
		fail_mask = 0;
		return EAGAIN;
	}
	return __real_pthread_sigmask(how, set, old);
}

int __real_sigpending(sigset_t *);
int __wrap_sigpending(sigset_t *pending)
{
	if (armed && fail_pending) {
		fail_pending = 0;
		errno = EIO;
		return -1;
	}
	return __real_sigpending(pending);
}

static int output(int source)
{
	return source ? console_debug_source(__FILE__, __LINE__, __func__,
					     "after-setup") :
			console_print("after-setup");
}

int main(int argc, char **argv)
{
	CHECK(argc == 4);
	CHECK(!strcmp(argv[1], "print") || !strcmp(argv[1], "source"));
	CHECK(!strcmp(argv[2], "mask") || !strcmp(argv[2], "pending"));
	CHECK(!strcmp(argv[3], "unlock") || !strcmp(argv[3], "recover"));
	int source = !strcmp(argv[1], "source");
	int poison = !strcmp(argv[3], "unlock");
	fail_mask = !strcmp(argv[2], "mask");
	fail_pending = !fail_mask;
	fail_unlock = poison;
	int first_error = fail_mask ? EAGAIN : EIO;
	console_config_t config = CONSOLE_DEFAULT_CONFIG();
	config.verbosity = CONSOLE_DEBUG;
	config.color = CONSOLE_COLOR_NEVER;
	console_init(&config);
	sigset_t before;
	sigset_t after;
	CHECK(!pthread_sigmask(SIG_SETMASK, NULL, &before));
	FILE *stream = tmpfile();
	CHECK(stream);
	FILE *saved = source ? stderr : stdout;
	if (source)
		stderr = stream;
	else
		stdout = stream;
	armed = 1;
	int rc = output(source);
	int error = errno;
	if (source)
		stderr = saved;
	else
		stdout = saved;
	CHECK(rc == -1 && error == first_error);
	CHECK(locks == 1 && unlocks == 1);
	CHECK(!fail_mask && !fail_pending && !fail_unlock);
	CHECK(ftell(stream) == 0);
	CHECK(!pthread_sigmask(SIG_SETMASK, NULL, &after));
	for (int signum = 1; signum < NSIG; ++signum)
		CHECK(sigismember(&before, signum) ==
		      sigismember(&after, signum));
	if (poison) {
		CHECK(output(source) == -1 && errno == EPERM);
		CHECK(locks == 1 && unlocks == 1);
	} else {
		CHECK(!output(source));
		CHECK(locks == 2 && unlocks == 2);
	}
	armed = 0;
	CHECK(!fclose(stream));
	return 0;
}
