#define _POSIX_C_SOURCE 200809L
#include "logger_queue.h"
#include "logger_runtime.h"
#include "support.h"
#include <stdint.h>
#include <string.h>

/* 包装仅存在于测试程序；正常调用仍使用真实运行域映射与释放。
 * 本测试不启动工作线程，运行域计数的直接读取不存在并发修改。
 */
struct allocation_attempt {
	size_t count;
	size_t size;
	void *pointer;
	unsigned int allocated;
	unsigned int released;
};

static struct allocation_attempt attempts[2];
static struct logger_runtime *runtime;
static size_t baseline_allocations;
static unsigned int allocation_calls;
static unsigned int successful_allocations;
static unsigned int actual_frees;
static unsigned int fail_at;
static unsigned int refused;
static int watch;

void *__real_logger_runtime_calloc(size_t count, size_t size);
void *__wrap_logger_runtime_calloc(size_t count, size_t size)
{
	if (!watch)
		return __real_logger_runtime_calloc(count, size);
	CHECK(allocation_calls < sizeof(attempts) / sizeof(attempts[0]));
	struct allocation_attempt *attempt = &attempts[allocation_calls++];
	attempt->count = count;
	attempt->size = size;
	if (allocation_calls == fail_at) {
		++refused;
		errno = ENOMEM;
		return NULL;
	}
	size_t before = runtime->allocation_count;
	void *pointer = __real_logger_runtime_calloc(count, size);
	int error = errno;
	attempt->pointer = pointer;
	if (pointer) {
		attempt->allocated = 1;
		++successful_allocations;
		CHECK(runtime->allocation_count == before + 1);
	} else {
		CHECK(runtime->allocation_count == before);
	}
	errno = error;
	return pointer;
}

int __real_logger_runtime_free(void *pointer);
int __wrap_logger_runtime_free(void *pointer)
{
	if (!watch || !pointer)
		return __real_logger_runtime_free(pointer);
	struct allocation_attempt *attempt = NULL;
	for (unsigned int i = 0; i < allocation_calls; ++i) {
		if (attempts[i].pointer == pointer) {
			attempt = &attempts[i];
			break;
		}
	}
	CHECK(attempt && attempt->allocated && !attempt->released);
	size_t before = runtime->allocation_count;
	int result = __real_logger_runtime_free(pointer);
	int error = errno;
	if (!result) {
		/* 真正释放成功后才记账，并使测试侧的借用指针失效。 */
		attempt->pointer = NULL;
		++attempt->released;
		++actual_frees;
		CHECK(before == runtime->allocation_count + 1);
	}
	errno = error;
	return result;
}

static void no_allocations_left(void)
{
	CHECK(runtime->allocation_count == baseline_allocations);
	CHECK(successful_allocations == actual_frees);
	for (unsigned int i = 0; i < allocation_calls; ++i) {
		CHECK(!attempts[i].pointer);
		CHECK(attempts[i].allocated == attempts[i].released);
	}
}

static void begin_tracking(unsigned int failure)
{
	no_allocations_left();
	memset(attempts, 0, sizeof(attempts));
	allocation_calls = 0;
	successful_allocations = 0;
	actual_frees = 0;
	refused = 0;
	fail_at = failure;
	watch = 1;
}

static void check_geometry(const struct logger_queue *q, size_t capacity,
			   size_t spill_capacity)
{
	CHECK(spill_capacity <= SIZE_MAX / sizeof(struct logger_queue_spill));
	size_t spill_bytes = spill_capacity * sizeof(struct logger_queue_spill);
	CHECK(capacity <=
	      (SIZE_MAX - spill_bytes) / sizeof(struct logger_queue_slot));
	size_t total =
		capacity * sizeof(struct logger_queue_slot) + spill_bytes;
	CHECK(q->cap == capacity && q->mask == capacity - 1);
	CHECK(q->spill_cap == spill_capacity && q->storage_bytes == total);
	CHECK(logger_queue_storage_bytes(q) == total);
	CHECK(logger_queue_spill_capacity(q) == spill_capacity);
	CHECK(logger_queue_spill_storage_bytes(q) == spill_bytes);
	CHECK(!logger_queue_wait_error(q));
}

static void check_attempt(unsigned int index, size_t count, size_t size)
{
	CHECK(index < allocation_calls);
	CHECK(attempts[index].count == count && attempts[index].size == size);
}

static void check_records(struct logger_queue *q)
{
	static const char short_text[] = "queue-size-inline";
	struct logger_message input = { 0 };
	struct logger_message output = { 0 };
	input.level = LOGGER_INFO;
	input.line = 1;
	input.text_len = (uint16_t)(sizeof(short_text) - 1u);
	memcpy(input.text, short_text, sizeof(short_text));
	CHECK(logger_queue_push(q, &input) == 1);

	size_t long_length = LOGGER_QUEUE_INLINE_TEXT + 1u;
	CHECK(long_length < sizeof(input.text) && long_length <= UINT16_MAX);
	input.line = 2;
	input.text_len = (uint16_t)long_length;
	for (size_t i = 0; i < long_length; ++i)
		input.text[i] = (char)('a' + i % 26u);
	input.text[long_length] = '\0';
	CHECK(logger_queue_push(q, &input) == 1);
	CHECK(logger_queue_depth(q) == 2 && !logger_queue_empty(q));
	CHECK(logger_queue_spill_in_use(q) == 1);
	CHECK(runtime->allocation_count == baseline_allocations + 2);

	CHECK(logger_queue_try_pop(q, &output) == 1);
	CHECK(output.level == LOGGER_INFO && output.line == 1);
	CHECK(output.text_len == sizeof(short_text) - 1u);
	CHECK(!memcmp(output.text, short_text, sizeof(short_text)));
	CHECK(logger_queue_depth(q) == 1 && logger_queue_spill_in_use(q) == 1);
	CHECK(logger_queue_try_pop(q, &output) == 1);
	CHECK(output.level == LOGGER_INFO && output.line == 2);
	CHECK(output.text_len == long_length);
	CHECK(!memcmp(output.text, input.text, long_length + 1u));
	CHECK(logger_queue_try_pop(q, &output) == 0);
	CHECK(logger_queue_empty(q) && !logger_queue_depth(q));
	CHECK(!logger_queue_spill_in_use(q));
	CHECK(!logger_queue_spill_exhaustions(q));
	CHECK(allocation_calls == 2 && !actual_frees);
}

static void healthy(size_t requested, size_t capacity, size_t spill_capacity)
{
	begin_tracking(0);
	struct logger_queue q;
	CHECK(logger_queue_init(&q, requested) == 0);
	check_geometry(&q, capacity, spill_capacity);
	CHECK(allocation_calls == 2 && successful_allocations == 2 && !refused);
	check_attempt(0, capacity, sizeof(struct logger_queue_slot));
	check_attempt(1, spill_capacity, sizeof(struct logger_queue_spill));
	CHECK(q.slots == attempts[0].pointer &&
	      q.spills == attempts[1].pointer);
	CHECK(runtime->allocation_count == baseline_allocations + 2);
	check_records(&q);
	CHECK(logger_queue_destroy(&q) == 0);
	CHECK(!q.slots && !q.spills && actual_frees == 2);
	no_allocations_left();
}

static size_t largest_capacity(void)
{
	/* 从存储字节上界向下取 2 的幂，与请求容量向上取整独立。
	 * 不包含运行域的私有分配头，更不代表宿主能映射如此大的对象。
	 */
	size_t spill_bytes =
		LOGGER_QUEUE_SPILL_LIMIT * sizeof(struct logger_queue_spill);
	size_t limit =
		(SIZE_MAX - spill_bytes) / sizeof(struct logger_queue_slot);
	size_t capacity = (SIZE_MAX / 2u + 1u) / 2u;
	while (capacity > limit)
		capacity /= 2u;
	CHECK(capacity >= LOGGER_QUEUE_SPILL_LIMIT);
	return capacity;
}

static void allocation_failure(size_t requested, size_t capacity,
			       size_t spill_capacity, unsigned int failure)
{
	begin_tracking(failure);
	struct logger_queue q;
	errno = EDOM;
	CHECK(logger_queue_init(&q, requested) == -1 && errno == ENOMEM);
	check_geometry(&q, capacity, spill_capacity);
	CHECK(!q.slots && !q.spills);
	CHECK(allocation_calls == failure && refused == 1);
	CHECK(successful_allocations == failure - 1u);
	CHECK(actual_frees == failure - 1u);
	check_attempt(0, capacity, sizeof(struct logger_queue_slot));
	if (failure == 2)
		check_attempt(1, spill_capacity,
			      sizeof(struct logger_queue_spill));
	no_allocations_left();
	/* 同进程恢复必须重新获得真实存储并完成短／长记录及真实释放。 */
	healthy(3, 4, 4);
}

static void capacity_overflow(void)
{
	size_t requests[] = {
		largest_capacity() + 1u,
		SIZE_MAX / 2u + 1u,
		SIZE_MAX,
	};
	for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
		begin_tracking(0);
		struct logger_queue q;
		errno = EDOM;
		CHECK(logger_queue_init(&q, requests[i]) == -1 &&
		      errno == EOVERFLOW);
		CHECK(!q.slots && !q.spills && !logger_queue_wait_error(&q));
		CHECK(!q.cap && !q.mask && !q.spill_cap && !q.storage_bytes);
		CHECK(!allocation_calls && !successful_allocations &&
		      !actual_frees && !refused);
		no_allocations_left();
		healthy(3, 4, 4);
	}
}

static void run(const char *mode)
{
	if (!strcmp(mode, "minimum")) {
		healthy(0, 2, 2);
		healthy(1, 2, 2);
		healthy(2, 2, 2);
	} else if (!strcmp(mode, "round-up")) {
		healthy(3, 4, 4);
		healthy(4, 4, 4);
		healthy(5, 8, 8);
		healthy(7, 8, 8);
		healthy(8, 8, 8);
		healthy(9, 16, 16);
	} else if (!strcmp(mode, "spill-limit")) {
		healthy(1023, 1024, 1024);
		healthy(1024, 1024, 1024);
	} else if (!strcmp(mode, "spill-capped")) {
		healthy(1025, 2048, 1024);
	} else if (!strcmp(mode, "max-capacity")) {
		size_t capacity = largest_capacity();
		/* 必须在第一次调用主动拒绝，不分配假小块或触碰巨额映射。 */
		allocation_failure(capacity, capacity, LOGGER_QUEUE_SPILL_LIMIT,
				   1);
	} else if (!strcmp(mode, "capacity-overflow")) {
		capacity_overflow();
	} else if (!strcmp(mode, "first-allocation-failure")) {
		allocation_failure(3, 4, 4, 1);
	} else if (!strcmp(mode, "second-allocation-failure")) {
		allocation_failure(1025, 2048, 1024, 2);
	} else {
		CHECK(!"unknown queue size scenario");
	}
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(logger_process_init() == 0);
	runtime = logger_runtime_current();
	CHECK(runtime);
	baseline_allocations = runtime->allocation_count;
	CHECK(!baseline_allocations);
	run(argv[1]);
	no_allocations_left();
	watch = 0;
	printf("QUEUE_SIZE_PASS=%s\n", argv[1]);
	return 0;
}
