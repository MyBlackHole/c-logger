#define _GNU_SOURCE
#include "logger_internal.h"
#include "support.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * 真正的生产静态库经 GNU ld --wrap 注入；生产实现无新钩子。
 * 每个失败案例独立进程，一次只拒绝一种资源获取调用。
 */
enum fault_kind {
	FAULT_NONE,
	FAULT_CALLOC,
	FAULT_MALLOC,
	FAULT_MUTEX,
	FAULT_COND
};

struct tracked_allocation {
	void *ptr;
	int released;
};

static enum fault_kind fault;
static unsigned int target, refused;
static unsigned int calloc_calls, malloc_calls, mutex_calls, cond_calls;
static unsigned int created_workers, destroyed_allocations;
static struct tracked_allocation allocations[64];
static unsigned int allocation_count;

static void note_allocation(void *p)
{
	if (!p)
		return;
	CHECK(allocation_count < sizeof(allocations) / sizeof(allocations[0]));
	allocations[allocation_count++].ptr = p;
}

void *__real_logger_runtime_calloc(size_t n, size_t size);
void *__wrap_logger_runtime_calloc(size_t n, size_t size)
{
	unsigned int call = ++calloc_calls;
	if (fault == FAULT_CALLOC && call == target) {
		++refused;
		errno = ENOMEM;
		return NULL;
	}
	void *p = __real_logger_runtime_calloc(n, size);
	note_allocation(p);
	return p;
}

void *__real_logger_runtime_malloc(size_t size);
void *__wrap_logger_runtime_malloc(size_t size)
{
	unsigned int call = ++malloc_calls;
	if (fault == FAULT_MALLOC && call == target) {
		++refused;
		errno = ENOMEM;
		return NULL;
	}
	void *p = __real_logger_runtime_malloc(size);
	note_allocation(p);
	return p;
}

int __real_logger_runtime_free(void *p);
int __wrap_logger_runtime_free(void *p)
{
	if (p) {
		int found = 0;
		for (unsigned int i = allocation_count; i > 0; --i) {
			struct tracked_allocation *item = &allocations[i - 1];
			if (item->ptr != p || item->released)
				continue;
			item->released = 1;
			++destroyed_allocations;
			found = 1;
			break;
		}
		if (!found) {
			/* 防止把两次 free 同一对象误解释为释放两个分配。 */
			for (unsigned int i = 0; i < allocation_count; ++i)
				CHECK(allocations[i].ptr != p);
		}
	}
	return __real_logger_runtime_free(p);
}

int __real_pthread_mutex_init(pthread_mutex_t *mu,
			      const pthread_mutexattr_t *attr);
int __wrap_pthread_mutex_init(pthread_mutex_t *mu,
			      const pthread_mutexattr_t *attr)
{
	unsigned int call = ++mutex_calls;
	if (fault == FAULT_MUTEX && call == target) {
		++refused;
		return EAGAIN;
	}
	return __real_pthread_mutex_init(mu, attr);
}

int __real_pthread_cond_init(pthread_cond_t *cv, const pthread_condattr_t *a);
int __wrap_pthread_cond_init(pthread_cond_t *cv, const pthread_condattr_t *a)
{
	unsigned int call = ++cond_calls;
	if (fault == FAULT_COND && call == target) {
		++refused;
		return EAGAIN;
	}
	return __real_pthread_cond_init(cv, a);
}

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*start)(void *arg), void *arg);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*start)(void *arg), void *arg)
{
	++created_workers;
	return __real_pthread_create(thread, attr, start, arg);
}

static void assert_no_leaks(void)
{
	CHECK(destroyed_allocations == allocation_count);
	for (unsigned int i = 0; i < allocation_count; ++i)
		CHECK(allocations[i].released);
	CHECK(logger_process_object_count() == 0);
}

static void choose_case(const char *kind, unsigned int index)
{
	if (!strcmp(kind, "calloc")) {
		CHECK(index >= 1 && index <= 9);
		fault = FAULT_CALLOC;
	} else if (!strcmp(kind, "malloc")) {
		CHECK(index == 1);
		fault = FAULT_MALLOC;
	} else if (!strcmp(kind, "mutex")) {
		CHECK(index >= 1 && index <= 3);
		fault = FAULT_MUTEX;
	} else if (!strcmp(kind, "cond")) {
		CHECK(index >= 1 && index <= 2);
		fault = FAULT_COND;
	} else
		CHECK(!"unknown resource acquisition type");
	target = index;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2 || argc == 3);
	/*
	 * 域构造的四个 mutex/rwlock 由独立运行域故障矩阵覆盖。
	 * 本用例只统计实例构造原有的取得点，先完成一次真实进程初始化。
	 */
	CHECK(logger_process_init() == 0);
	mutex_calls = 0;
	cond_calls = 0;
	if (argc == 3) {
		char *end = NULL;
		unsigned long chosen = strtoul(argv[2], &end, 10);
		CHECK(end && *end == 0 && chosen > 0 && chosen <= 9);
		choose_case(argv[1], (unsigned int)chosen);
	} else
		CHECK(!strcmp(argv[1], "healthy"));

	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_STDERR;
	cfg.async_mode = 1;
	cfg.queue_capacity = 64;

	if (fault != FAULT_NONE) {
		errno = 0;
		logger_t *candidate = NULL;
		int create_rc = logger_create(&cfg, &candidate);
		CHECK(candidate == NULL);
		CHECK(refused == 1);
		int expected_error = ENOMEM;
		if (fault == FAULT_MUTEX || fault == FAULT_COND)
			expected_error = EAGAIN;
		CHECK(create_rc == -expected_error);
		CHECK(created_workers == 0);
		assert_no_leaks(); /* 部分构造不得遗留已分配工作区/同步对象。 */
		fault = FAULT_NONE;
	}

	unsigned int before_workers = created_workers;
	logger_t *logger = NULL;
	CHECK(logger_create(&cfg, &logger) == 0);
	CHECK(logger != NULL);
	logger_state_t state;
	CHECK(logger_get_state(logger, &state) == 0 &&
	      state == LOGGER_STATE_RUNNING);
	CHECK(created_workers == before_workers + 1);
	CHECK(logger_flush(logger) == 0);
	CHECK(logger_destroy(&logger) == 0);
	assert_no_leaks();

	if (argc == 2) {
		/*
		 * 当前源码共有 9 次 calloc、1 次 malloc、3 次 mutex_init、
		 * 2 次 cond_init；改变获取点需要同步修改审查清单和用例。
		 */
		CHECK(calloc_calls == 9);
		CHECK(malloc_calls == 1);
		CHECK(mutex_calls == 3);
		CHECK(cond_calls == 2);
	}
	puts("constructor resource failure regression passed");
	return 0;
}
