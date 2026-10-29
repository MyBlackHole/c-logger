#define _GNU_SOURCE
#include "logger.h"
#include "console.h"
#include "support.h"
#include "logger_runtime.h"
#include <signal.h>

/*
 * 罕见的守卫设置或解锁失败只注入白盒静态库。
 * 生产闭管道行为有自己的无包装测试。
 */
static int armed;
static int fail_mask;
static int fail_pending;
static int fail_unlock;
static unsigned int locks;
static unsigned int unlocks;
static pthread_mutex_t *console_mutex;

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	int rc = __real_pthread_mutex_lock(mutex);

	if (armed && !rc && mutex == console_mutex)
		++locks;

	return rc;
}

int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);

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

int __real_pthread_sigmask(int how, const sigset_t *set, sigset_t *old);

int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
	if (armed && how == SIG_BLOCK && fail_mask) {
		fail_mask = 0;
		return EAGAIN;
	}
	return __real_pthread_sigmask(how, set, old);
}

int __real_sigpending(sigset_t *pending);

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
	CHECK(logger_process_init() == 0);
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
	CHECK(console_init(&config) == 0);
	/* 只注入 Console 锁；格式协调锁不属于本用例的释放对象。 */
	console_mutex = &logger_runtime_current()->console_mu;
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
	errno = EDOM;
	int rc = output(source);
	int error = errno;

	if (source)
		stderr = saved;
	else
		stdout = saved;
	CHECK(rc == -first_error && error == EDOM);
	CHECK(locks == 1 && unlocks == 1);
	CHECK(!fail_mask && !fail_pending && !fail_unlock);
	CHECK(ftell(stream) == 0);
	CHECK(!pthread_sigmask(SIG_SETMASK, NULL, &after));
	for (int signum = 1; signum < NSIG; ++signum)
		CHECK(sigismember(&before, signum) ==
		      sigismember(&after, signum));
	if (poison) {
		errno = EDOM;
		CHECK(output(source) == -EPERM && errno == EDOM);
		CHECK(locks == 1 && unlocks == 1);
	} else {
		CHECK(!output(source));
		CHECK(locks == 2 && unlocks == 2);
	}
	armed = 0;
	CHECK(!fclose(stream));
	return 0;
}
