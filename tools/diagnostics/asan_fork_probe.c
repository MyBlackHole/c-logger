#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * 独立诊断探针：不链接 logger，不读取或修改检测器内部状态。
 * 多线程 fork 后创建线程是本次研究的 Linux 扩展场景，不声称可移植 POSIX。
 */
static _Atomic int stop;
static _Atomic unsigned int ready;
static void *allocator(void *unused)
{
	(void)unused;
	atomic_fetch_add(&ready, 1);
	while (!atomic_load(&stop)) {
		void *p[128];
		for (unsigned int i = 0; i < 128; ++i) {
			p[i] = calloc(1, 32 + (i % 4) * 16);
			if (!p[i])
				abort();
			*(volatile unsigned char *)p[i] = (unsigned char)i;
		}
		for (unsigned int i = 0; i < 128; ++i)
			free(p[i]);
	}
	return NULL;
}
static void *child_worker(void *unused)
{
	(void)unused;
	return NULL;
}
extern unsigned int sanitizer_atfork_registrations;
extern unsigned int sanitizer_atfork_dropped;
int main(void)
{
	printf("SANITIZER_ATFORK registrations=%u dropped=%u\n",
	       sanitizer_atfork_registrations, sanitizer_atfork_dropped);
	fflush(stdout);
	pthread_t parents[8];
	for (unsigned int i = 0; i < 8; ++i)
		if (pthread_create(&parents[i], NULL, allocator, NULL))
			return 2;
	while (atomic_load(&ready) != 8)
		usleep(10);
	unsigned int completed = 0;
	unsigned int timed_out = 0;
	unsigned int failed = 0;
	for (unsigned int i = 0; i < 96; ++i) {
		pid_t pid = fork();
		if (pid < 0) {
			++failed;
			break;
		}
		if (!pid) {
			alarm(2);
			pthread_t thread;
			int rc = pthread_create(&thread, NULL, child_worker,
						NULL);
			if (rc)
				_exit(3);
			_exit(pthread_join(thread, NULL) ? 4 : 0);
		}
		int status;
		pid_t got;
		do {
			got = waitpid(pid, &status, 0);
		} while (got < 0 && errno == EINTR);
		if (got != pid)
			return 5;
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			++completed;
		else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM)
			++timed_out;
		else
			++failed;
		printf("FORK_PROBE_ITERATION=%u status=%d\n", i, status);
		fflush(stdout);
	}
	atomic_store(&stop, 1);
	for (unsigned int i = 0; i < 8; ++i)
		if (pthread_join(parents[i], NULL))
			return 6;
	printf("FORK_PROBE completed=%u timeout=%u failed=%u\n", completed,
	       timed_out, failed);
	return timed_out || failed ? 1 : 0;
}
