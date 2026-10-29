#define _GNU_SOURCE
#include "logger_fork_fd.h"
#include "logger_process.h"
#include "logger_runtime.h"
#include "logger_scope.h"
#include "logger_libc.h"
#include "logger_cleanup.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

/* fd 是内核分配的非负整数。按 fd 索引的位图不包含候选对象地址，
 * 因而构造栈退出、后端整体移动和实例最终 free 不会留下悬垂登记项。
 * 初始映射覆盖 32768 个 fd；高 fd 才扩容，最后一个引用关闭后归还映射。
 * 映射失败必须在交出新 fd 前回滚，不能留下未登记的描述符。
 */
#define fd_mu (logger_runtime_current()->resource_mu)
#define owned (logger_runtime_current()->owned_fds)
#define owned_bytes (logger_runtime_current()->owned_bytes)
#define owned_count (logger_runtime_current()->owned_count)
#define gate_error (logger_runtime_current()->resource_error)
static _Thread_local int in_gate;
static _Thread_local int prepared;
static _Thread_local int prepare_error;
static int child_error;
static int child_valid;
static _Thread_local uint64_t *snapshot_fds;
static _Thread_local size_t snapshot_bytes;
static _Thread_local unsigned int fork_depth;
static _Thread_local DIR *directory_lease;
static uint64_t *child_bitmap;
static size_t child_bitmap_bytes;

void logger_fork_resource_poison(int error)
{
	/* 不确定的同步状态不可重用；不再遍历或释放旧登记映射。
	 * 已经等待未知锁的线程不具备活性保证，不能冒称错误后的自动恢复。
	 */
	atomic_store_explicit(&gate_error, error, memory_order_release);
}

int logger_fork_resource_enter(void)
{
	int rc = logger_process_require();
	if (rc)
		return -rc;
	if (in_gate)
		return EDEADLK;
	rc = atomic_load_explicit(&gate_error, memory_order_acquire);
	if (rc)
		return rc;
	rc = __cleanup_lockdep_mutex_lock(&fd_mu, LOGGER_LOCK_RUNTIME_RESOURCE);
	if (rc) {
		logger_fork_resource_poison(rc);
		return rc;
	}
	in_gate = 1;
	rc = atomic_load_explicit(&gate_error, memory_order_acquire);
	if (rc) {
		int unlocked = __cleanup_lockdep_mutex_unlock(
			&fd_mu, LOGGER_LOCK_RUNTIME_RESOURCE);
		in_gate = 0;
		if (unlocked)
			logger_fork_resource_poison(unlocked);
	}
	return rc;
}

int logger_fork_resource_leave(int error)
{
	int rc = __cleanup_lockdep_mutex_unlock(&fd_mu,
						LOGGER_LOCK_RUNTIME_RESOURCE);
	in_gate = 0;
	if (rc)
		logger_fork_resource_poison(rc);
	return error ? error : rc;
}

static int grow_for(int fd)
{
	size_t required = ((size_t)fd / 64u + 1u) * sizeof(*owned);
	if (required <= owned_bytes)
		return 0;
	size_t bytes = owned_bytes ? owned_bytes : 4096u;
	while (bytes < required) {
		if (bytes > SIZE_MAX / 2u)
			return EOVERFLOW;
		bytes *= 2u;
	}
	void *next;
	if (owned)
		next = mremap(owned, owned_bytes, bytes, MREMAP_MAYMOVE);
	else
		next = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (next == MAP_FAILED)
		return errno;
	owned = next;
	owned_bytes = bytes;
	return 0;
}

static int is_owned(int fd)
{
	return fd >= 0 && (size_t)fd / 64u < owned_bytes / sizeof(*owned) &&
	       (owned[(size_t)fd / 64u] &
		(UINT64_C(1) << ((unsigned int)fd % 64u)));
}

static int register_fd(int fd)
{
	int rc = grow_for(fd);
	if (rc)
		return rc;
	if (is_owned(fd)) {
		/* 宿主私自 close/dup2 内部 fd 会破坏所有权，不能冒用旧登记。 */
		logger_fork_resource_poison(EUCLEAN);
		return EUCLEAN;
	}
	owned[(size_t)fd / 64u] |= UINT64_C(1) << ((unsigned int)fd % 64u);
	++owned_count;
	return 0;
}

static void unregister_fd(int fd)
{
	owned[(size_t)fd / 64u] &= ~(UINT64_C(1) << ((unsigned int)fd % 64u));
	--owned_count;
}

static int release_empty(void)
{
	if (owned_count || !owned)
		return 0;
	if (munmap(owned, owned_bytes)) {
		int error = errno;
		logger_fork_resource_poison(error);
		return error;
	}
	owned = NULL;
	owned_bytes = 0;
	return 0;
}

static int acquired(int fd)
{
	int error = fd < 0 ? errno : 0;
	int saved = errno;
	if (fd >= 0) {
		error = register_fd(fd);
		if (error) {
			/* fd 还没有交出；Linux close 消耗引用，失败也不重试。
			 * 关闭回执不能覆盖登记失败这一主错误。
			 */
			(void)close(fd);
			fd = -1;
		}
	}
	error = logger_fork_resource_leave(error);
	errno = error ? error : saved;
	return error ? -1 : fd;
}

int logger_fork_open(const char *path, int flags, mode_t mode)
{
	int error = logger_fork_resource_enter();
	if (error) {
		errno = error;
		return -1;
	}
	return acquired(open(path, flags, mode));
}

int logger_fork_openat(int dir, const char *path, int flags, mode_t mode)
{
	int error = logger_fork_resource_enter();
	if (error) {
		errno = error;
		return -1;
	}
	return acquired(openat(dir, path, flags, mode));
}

int logger_fork_socket(int domain, int type, int protocol)
{
	int error = logger_fork_resource_enter();
	if (error) {
		errno = error;
		return -1;
	}
	return acquired(socket(domain, type, protocol));
}

int logger_fork_close(int fd)
{
	int error = logger_fork_resource_enter();
	if (error) {
		errno = error;
		return -1;
	}
	if (!is_owned(fd)) {
		error = EBADF;
	} else {
		/* 先消费登记再 close，fork 不可能看见已关闭、被宿主复用的 fd。 */
		unregister_fd(fd);
		if (close(fd))
			error = errno;
		int released = release_empty();
		if (!error)
			error = released;
	}
	error = logger_fork_resource_leave(error);
	if (error)
		errno = error;
	return error ? -1 : 0;
}

DIR *logger_fork_fdopendir(int fd)
{
	int error = logger_fork_resource_enter();
	if (error) {
		errno = error;
		return NULL;
	}
	DIR *dir = NULL;
	if (!is_owned(fd))
		error = EBADF;
	else {
		dir = fdopendir(fd);
		if (!dir)
			error = errno;
	}
	if (dir) {
		/* DIR 内含 libc 锁/堆缓冲。整个遍历保持资源租约，fork 等待
		 * closedir 完成，避免子方拥有无法安全析构的半使用 DIR。
		 */
		directory_lease = dir;
		return dir;
	}
	error = logger_fork_resource_leave(error);
	errno = error;
	return NULL;
}

int logger_fork_closedir(DIR *dir)
{
	/* 仅同线程能够消费 fdopendir 成功取得的租约。 */
	if (!directory_lease || directory_lease != dir || !in_gate) {
		errno = EINVAL;
		return -1;
	}
	int fd = dirfd(dir);
	int error = 0;
	if (!is_owned(fd)) {
		error = EUCLEAN;
		logger_fork_resource_poison(error);
	} else {
		unregister_fd(fd);
		if (closedir(dir))
			error = errno;
		int released = release_empty();
		if (!error)
			error = released;
	}
	directory_lease = NULL;
	error = logger_fork_resource_leave(error);
	if (error)
		errno = error;
	return error ? -1 : 0;
}

void logger_fork_fd_prepare(void)
{
	int saved = errno;
	int busy = logger_scope_busy() || logger_global_scope_busy() ||
		   logger_libc_busy();
	if (fork_depth++) {
		errno = saved;
		return;
	}
	prepared = 0;
	prepare_error = 0;
	snapshot_fds = NULL;
	snapshot_bytes = 0;
	if (logger_process_is_child() || !logger_process_ready()) {
		prepare_error = ECHILD;
	} else if (in_gate || busy) {
		prepare_error = EDEADLK;
	} else {
		prepare_error =
			atomic_load_explicit(&gate_error, memory_order_acquire);
		if (!prepare_error)
			prepare_error = logger_libc_fork_prepare();
		if (!prepare_error) {
			prepare_error = __cleanup_lockdep_mutex_lock(
				&fd_mu, LOGGER_LOCK_RUNTIME_RESOURCE);
			if (!prepare_error) {
				prepared = 1;
				in_gate = 1;
				prepare_error = atomic_load_explicit(
					&gate_error, memory_order_acquire);
				snapshot_fds = owned;
				snapshot_bytes = owned_bytes;
			}
		}
	}
	errno = saved;
}

void logger_fork_fd_parent(void)
{
	int saved = errno;
	if (fork_depth > 1) {
		--fork_depth;
		errno = saved;
		return;
	}
	if (prepared) {
		int rc = __cleanup_lockdep_mutex_unlock(
			&fd_mu, LOGGER_LOCK_RUNTIME_RESOURCE);
		if (rc)
			logger_fork_resource_poison(rc);
		in_gate = 0;
	}
	logger_libc_fork_parent();
	prepared = 0;
	fork_depth = 0;
	errno = saved;
}

void logger_fork_fd_child(void)
{
	int saved = errno;
	child_valid = prepared && !prepare_error && fork_depth == 1;
	child_error = child_valid ? 0 :
				    (prepare_error ? prepare_error : ECHILD);
	if (child_valid) {
		/* 旧进程域已不在子方地址空间；这里只能使用 prepare 的 TLS
		 * 数值快照，不借用宏指向的旧域或任何 DIR/实例内存。
		 */
		child_bitmap = snapshot_fds;
		child_bitmap_bytes = snapshot_bytes;
		for (size_t word = 0;
		     word < snapshot_bytes / sizeof(*snapshot_fds); ++word) {
			uint64_t bits = snapshot_fds[word];
			snapshot_fds[word] = 0;
			while (bits) {
				unsigned int bit =
					(unsigned int)__builtin_ctzll(bits);
				bits &= bits - 1u;
				int fd = (int)(word * 64u + bit);
				if (close(fd) && !child_error)
					child_error = errno;
			}
		}
	}
	prepared = 0;
	fork_depth = 0;
	in_gate = 0;
	directory_lease = NULL;
	logger_libc_fork_child();
	logger_lockdep_reset_child();
	errno = saved;
}

int logger_fork_fd_child_reinit(void)
{
	if (!child_valid)
		return -ECHILD;
	if (child_error)
		return -child_error;
	/* 普通显式初始化中释放已消费位图；失败可以重试，但不再 close。 */
	if (child_bitmap && munmap(child_bitmap, child_bitmap_bytes))
		return -errno;
	child_bitmap = NULL;
	child_bitmap_bytes = 0;
	return 0;
}

int logger_fork_fd_child_error(void)
{
	return child_error;
}

int logger_fork_fd_child_valid(void)
{
	return child_valid;
}

int logger_fork_fd_preparing(void)
{
	return fork_depth != 0;
}
