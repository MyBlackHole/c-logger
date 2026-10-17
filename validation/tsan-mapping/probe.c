#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* 不链接 Logger；不调用检测器注解、不设置抑制、不重置存活锁。 */
struct pair {
	pthread_mutex_t first;
	pthread_mutex_t second;
};
static struct pair *before;
static size_t page;
static int hold;
static int child_seen;
static volatile unsigned raced;
#define REQUIRE(expr)                                                      \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "PROBE_ASSERT line=%d errno=%d\n", \
				__LINE__, errno);                          \
			_exit(90);                                         \
		}                                                          \
	} while (0)

static struct pair *create(void *hint)
{
	struct pair *p = mmap(hint, page, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	REQUIRE(p != MAP_FAILED);
	REQUIRE(madvise(p, page, MADV_DONTFORK) == 0);
	REQUIRE(pthread_mutex_init(&p->first, NULL) == 0);
	REQUIRE(pthread_mutex_init(&p->second, NULL) == 0);
	return p;
}

static void use(struct pair *p, int reverse)
{
	pthread_mutex_t *a = reverse ? &p->second : &p->first;
	pthread_mutex_t *b = reverse ? &p->first : &p->second;
	REQUIRE(pthread_mutex_lock(a) == 0);
	REQUIRE(pthread_mutex_lock(b) == 0);
	REQUIRE(pthread_mutex_unlock(b) == 0);
	REQUIRE(pthread_mutex_unlock(a) == 0);
}

static void destroy(struct pair *p)
{
	REQUIRE(pthread_mutex_destroy(&p->second) == 0);
	REQUIRE(pthread_mutex_destroy(&p->first) == 0);
	REQUIRE(munmap(p, page) == 0);
}

static void prepare(void)
{
	if (hold)
		REQUIRE(pthread_mutex_lock(&before->second) == 0);
}
static void parent(void)
{
	if (hold)
		REQUIRE(pthread_mutex_unlock(&before->second) == 0);
}
static void child(void)
{
	child_seen = 1;
}

static void *race_writer(void *unused)
{
	(void)unused;
	for (unsigned i = 0; i != 10000; ++i)
		raced++;
	return NULL;
}

static void real_race(void)
{
	pthread_t a;
	pthread_t b;
	REQUIRE(pthread_create(&a, NULL, race_writer, NULL) == 0);
	REQUIRE(pthread_create(&b, NULL, race_writer, NULL) == 0);
	REQUIRE(pthread_join(a, NULL) == 0);
	REQUIRE(pthread_join(b, NULL) == 0);
}

int main(int argc, char **argv)
{
	REQUIRE(argc == 2);
	(void)alarm(20);
	long n = sysconf(_SC_PAGESIZE);
	REQUIRE(n > 0 && (size_t)n >= sizeof(struct pair));
	page = (size_t)n;
	const char *mode = argv[1];
	before = create(NULL);
	use(before, 0);
	if (!strcmp(mode, "race")) {
		real_race();
		destroy(before);
	} else if (!strcmp(mode, "retained-order")) {
		/* 新映射不能抹掉无关存活互斥锁上的真实逆序。 */
		struct pair *other = create(NULL);
		use(other, 0);
		destroy(other);
		use(before, 1);
		destroy(before);
	} else if (!strcmp(mode, "failed-map-order")) {
		REQUIRE(mmap(before, 0, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED);
		use(before, 1);
		destroy(before);
	} else if (!strcmp(mode, "no-fork")) {
		uintptr_t old = (uintptr_t)before;
		destroy(before);
		struct pair *next = create((void *)old);
		REQUIRE((uintptr_t)next == old);
		use(next, 0);
		destroy(next);
	} else {
		int reverse = !strcmp(mode, "child-order");
		int locked = !strcmp(mode, "child-destroy-locked");
		hold = strcmp(mode, "unlocked") != 0;
		REQUIRE(reverse || locked || !strcmp(mode, "held") || !hold);
		REQUIRE(pthread_atfork(prepare, parent, child) == 0);
		pid_t pid = fork();
		REQUIRE(pid >= 0);
		if (pid == 0) {
			unsigned char resident;
			REQUIRE(child_seen);
			REQUIRE(mincore(before, page, &resident) == -1 &&
				errno == ENOMEM);
			struct pair *next = create(before);
			REQUIRE(next == before);
			fprintf(stderr, "OLD_ABSENT=1 ADDRESS_REUSED=1\n");
			use(next, 0);
			if (reverse)
				use(next, 1);
			if (locked) {
				REQUIRE(pthread_mutex_lock(&next->second) == 0);
				/* 故意错误；不能把修复新寿命变成忽略真实锁误用。 */
				(void)pthread_mutex_destroy(&next->second);
			} else {
				destroy(next);
			}
			fprintf(stderr, "PROBE_CHILD_COMPLETE=%s\n", mode);
			return 0;
		}
		int status;
		pid_t got;
		do {
			got = waitpid(pid, &status, 0);
		} while (got < 0 && errno == EINTR);
		REQUIRE(got == pid && WIFEXITED(status));
		destroy(before);
		fprintf(stderr, "PROBE_CHILD_EXIT=%d\n", WEXITSTATUS(status));
		return WEXITSTATUS(status);
	}
	fprintf(stderr, "PROBE_COMPLETE=%s\n", mode);
	return 0;
}
