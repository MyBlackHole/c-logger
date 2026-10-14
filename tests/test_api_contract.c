#include "logger.h"
#include <errno.h>

/* #108：真实生产目标上的公开返回约定。
 * 普通状态入口为 0 / -1 + errno；旧同步状态写入是 0 / -errno。
 */
static int rejected(int rc, int error)
{
	return rc == -1 && errno == error;
}

int main(void)
{
	errno = 0;
	if (logger_create(NULL) != NULL || errno != EINVAL)
		return 1;

	errno = 0;
	if (!rejected(logger_flush_instance_status(NULL), EINVAL))
		return 2;
	errno = 0;
	if (!rejected(logger_reopen_instance(NULL), EINVAL))
		return 3;

	/* 此兼容状态入口不能被误改为 POSIX 风格 -1 + errno。 */
	errno = 0;
	/* 空元数据用 C11 零指针常量；唯一被测错误是无实例。 */
	int rc = logger_log_sync_status(NULL, LOGGER_INFO, 0, 0, 0, 0, "x");
	if (rc != -EINVAL)
		return 4;

	/* Global 的未初始化与无效配置同样使用可观察的 POSIX 错误。 */
	errno = 0;
	if (!rejected(logger_flush_status(), ENODEV))
		return 5;
	errno = 0;
	if (!rejected(logger_init(NULL), EINVAL))
		return 6;

	/* NULL 销毁不会消费资源，成功不清除调用方已有 errno。 */
	errno = ERANGE;
	if (logger_destroy_status(NULL) != 0 || errno != ERANGE)
		return 7;

	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_STDERR;
	cfg.async_mode = 0;
	logger_t *instance = logger_create(&cfg);
	if (!instance)
		return 8;
	if (logger_flush_instance_status(instance))
		return 9;
	if (logger_destroy_status(instance))
		return 10;
	return 0;
}
