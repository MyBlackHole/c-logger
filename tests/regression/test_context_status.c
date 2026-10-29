#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include "logger_scope.h"

static _Thread_local int active;
static _Thread_local int setup_error;
static _Thread_local int restore_error;
static _Thread_local unsigned int cancel_calls;

int __real_pthread_setcancelstate(int state, int *old);
int __wrap_pthread_setcancelstate(int state, int *old)
{
	if (active) {
		++cancel_calls;
		if (state == PTHREAD_CANCEL_DISABLE && setup_error) {
			int error = setup_error;
			setup_error = 0;
			errno = ERANGE;
			return error;
		}
	}
	int rc = __real_pthread_setcancelstate(state, old);
	/* 先恢复真实状态，再注入失败；不能污染测试宿主线程。 */
	if (active && !rc && state == PTHREAD_CANCEL_ENABLE && restore_error) {
		int error = restore_error;
		restore_error = 0;
		errno = ERANGE;
		return error;
	}
	return rc;
}

static void check_context(const char *request, const char *session,
			  const char *trace)
{
	logger_context_t got = { 0 };
	errno = EDOM;
	CHECK(logger_context_get(&got) == 0 && errno == EDOM);
	CHECK((!request && !got.request_id) ||
	      (request && got.request_id && !strcmp(request, got.request_id)));
	CHECK((!session && !got.session_id) ||
	      (session && got.session_id && !strcmp(session, got.session_id)));
	CHECK((!trace && !got.trace_id) ||
	      (trace && got.trace_id && !strcmp(trace, got.trace_id)));
}

static void check_failure_slot(int error)
{
	struct {
		unsigned char prefix[16];
		logger_context_t value;
		unsigned char suffix[16];
	} slot;
	unsigned char before[sizeof(slot)];
	memset(&slot, 0xa5, sizeof(slot));
	memcpy(before, &slot, sizeof(slot));
	errno = EDOM;
	active = 1;
	int rc = logger_context_get(&slot.value);
	active = 0;
	CHECK(rc == -error && errno == EDOM);
	CHECK(memcmp(&slot, before, sizeof(slot)) == 0);
	CHECK(!logger_scope_busy());
}

static void check_set_error(int error, int clear)
{
	errno = EDOM;
	active = 1;
	int rc;
	if (clear) {
		rc = logger_context_clear();
	} else {
		logger_context_t value = { .request_id = "committed",
					   .session_id = "new-session" };
		rc = logger_context_set(&value);
	}
	active = 0;
	CHECK(rc == -error && errno == EDOM);
	CHECK(!logger_scope_busy());
}

static void *thread_main(void *arg)
{
	const char *request = arg;
	logger_context_t ctx = { .request_id = request };
	errno = EDOM;
	CHECK(logger_context_set(&ctx) == 0 && errno == EDOM);
	check_context(request, NULL, NULL);
	CHECK(logger_context_clear() == 0);
	check_context(NULL, NULL, NULL);
	return NULL;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(logger_process_init() == 0);
	const char *scenario = argv[1];
	logger_context_t first = { .request_id = "original",
				   .session_id = "old-session",
				   .trace_id = "old-trace" };
	errno = EDOM;
	CHECK(logger_context_set(&first) == 0 && errno == EDOM);
	check_context("original", "old-session", "old-trace");
	if (!strcmp(scenario, "healthy")) {
		logger_context_t borrowed;
		CHECK(logger_context_get(&borrowed) == 0);
		const char *request = borrowed.request_id;
		borrowed.request_id = borrowed.session_id;
		borrowed.session_id = request;
		CHECK(logger_context_set(&borrowed) == 0);
		check_context("old-session", "original", "old-trace");
		char large[140];
		memset(large, 'X', sizeof(large) - 1);
		large[sizeof(large) - 1] = 0;
		logger_context_t oversized = { .request_id = large };
		CHECK(logger_context_set(&oversized) == 0);
		logger_context_t got;
		CHECK(logger_context_get(&got) == 0);
		CHECK(got.request_id && strlen(got.request_id) == 63);
		CHECK(!got.session_id && !got.trace_id);
	} else if (!strcmp(scenario, "get-null")) {
		errno = EDOM;
		CHECK(logger_context_get(NULL) == -EINVAL && errno == EDOM);
		check_context("original", "old-session", "old-trace");
	} else if (!strcmp(scenario, "setup-set")) {
		setup_error = EAGAIN;
		check_set_error(EAGAIN, 0);
		CHECK(cancel_calls == 1 && !setup_error);
		check_context("original", "old-session", "old-trace");
	} else if (!strcmp(scenario, "setup-clear")) {
		setup_error = EAGAIN;
		check_set_error(EAGAIN, 1);
		CHECK(cancel_calls == 1 && !setup_error);
		check_context("original", "old-session", "old-trace");
	} else if (!strcmp(scenario, "setup-get")) {
		setup_error = EAGAIN;
		check_failure_slot(EAGAIN);
		CHECK(cancel_calls == 1 && !setup_error);
		check_context("original", "old-session", "old-trace");
	} else if (!strcmp(scenario, "restore-set")) {
		restore_error = EIO;
		check_set_error(EIO, 0);
		CHECK(cancel_calls == 2 && !restore_error);
		check_context("committed", "new-session", NULL);
	} else if (!strcmp(scenario, "restore-clear")) {
		restore_error = EIO;
		check_set_error(EIO, 1);
		CHECK(cancel_calls == 2 && !restore_error);
		check_context(NULL, NULL, NULL);
	} else if (!strcmp(scenario, "restore-get")) {
		restore_error = EIO;
		check_failure_slot(EIO);
		CHECK(cancel_calls == 2 && !restore_error);
		check_context("original", "old-session", "old-trace");
	} else if (!strcmp(scenario, "disabled")) {
		int old_state, old_type, observed;
		CHECK(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					     &old_state) == 0);
		CHECK(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,
					    &old_type) == 0);
		active = 1;
		errno = EDOM;
		logger_context_t got;
		CHECK(logger_context_get(&got) == 0 && errno == EDOM);
		CHECK(logger_context_set(&got) == 0 && errno == EDOM);
		CHECK(logger_context_clear() == 0 && errno == EDOM);
		active = 0;
		CHECK(cancel_calls == 6);
		CHECK(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					     &observed) == 0);
		CHECK(observed == PTHREAD_CANCEL_DISABLE);
		CHECK(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,
					    &observed) == 0);
		CHECK(observed == PTHREAD_CANCEL_ASYNCHRONOUS);
		CHECK(pthread_setcanceltype(old_type, NULL) == 0);
		CHECK(pthread_setcancelstate(old_state, NULL) == 0);
		check_context(NULL, NULL, NULL);
	} else if (!strcmp(scenario, "threads")) {
		pthread_t a, b;
		CHECK(pthread_create(&a, NULL, thread_main, "worker-a") == 0);
		CHECK(pthread_create(&b, NULL, thread_main, "worker-b") == 0);
		CHECK(pthread_join(a, NULL) == 0);
		CHECK(pthread_join(b, NULL) == 0);
		check_context("original", "old-session", "old-trace");
	} else {
		CHECK(!"unknown context status scenario");
	}
	CHECK(logger_context_clear() == 0);
	check_context(NULL, NULL, NULL);
	printf("CONTEXT_STATUS_PASS=%s\n", scenario);
	return 0;
}
