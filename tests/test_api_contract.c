#include "logger.h"
#include <errno.h>

/*
 * 生命周期新接口使用 0/-errno 并保留 errno。尚未迁移的其他状态入口
 * 保持自己的既定约定；不能用创建/销毁改动默默替换全局接口。
 */
static int rejected(int rc, int error)
{
	return rc == -error && errno == 0;
}

int main(void)
{
	if (logger_process_init() != 0)
		return 1;
	errno = ERANGE;
	if (logger_create(NULL, &(logger_t *){ NULL }) != -EINVAL ||
	    errno != ERANGE)
		return 1;

	errno = 0;
	if (!rejected(logger_flush(NULL), EINVAL))
		return 2;
	errno = 0;
	if (!rejected(logger_reopen(NULL), EINVAL))
		return 3;

	/* 同步严格记录同样保留调用方 errno，不能只检查负错误码。 */
	errno = EDOM;
	/* 空元数据用 C11 零指针常量；唯一被测错误是无实例。 */
	int rc = logger_log_sync_status(NULL, LOGGER_INFO, 0, 0, 0, 0, "x");
	if (rc != -EINVAL || errno != EDOM)
		return 4;

	/* 默认刷新与绑定均使用负错误码，并保留调用方 errno。 */
	errno = 0;
	if (!rejected(logger_default_flush(), ENODEV))
		return 5;
	errno = 0;
	if (logger_default_set(NULL, NULL) != 0 || errno != 0)
		return 6;

	/* NULL 销毁不会消费资源，成功不清除调用方已有 errno。 */
	errno = ERANGE;
	if (logger_destroy(&(logger_t *){ NULL }) != 0 || errno != ERANGE)
		return 7;

	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_STDERR;
	cfg.async_mode = 0;
	logger_t *instance = NULL;
	(void)logger_create(&cfg, &instance);
	if (!instance)
		return 8;
	if (logger_flush(instance))
		return 9;
	errno = EDOM;
	if (logger_destroy(&instance) || instance != NULL || errno != EDOM)
		return 10;
	if (logger_destroy(&instance) || instance != NULL || errno != EDOM)
		return 11;
	if (logger_destroy(NULL) != -EINVAL || errno != EDOM)
		return 12;
	if (logger_create(&cfg, NULL) != -EINVAL || errno != EDOM)
		return 13;
	if (logger_create(&cfg, &instance) != 0 || !instance || errno != EDOM)
		return 14;
	logger_t *original = instance;
	/* 已有拥有者不能被新的创建请求覆盖，即使配置本身非法。 */
	if (logger_create(NULL, &instance) != -EBUSY || instance != original ||
	    errno != EDOM)
		return 15;
	if (logger_destroy(&instance) || instance != NULL)
		return 16;
	cfg.outputs = 1u << 31;
	if (logger_create(&cfg, &instance) != -EINVAL || instance ||
	    errno != EDOM)
		return 17;
	return 0;
}
