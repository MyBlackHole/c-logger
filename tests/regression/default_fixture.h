#ifndef LOGGER_DEFAULT_FIXTURE_H
#define LOGGER_DEFAULT_FIXTURE_H
#include "logger.h"
#include <errno.h>

/* 仅测试使用的宿主动作组合；拥有者始终显式传入，不属于生产兼容 API。
 * 默认层只借用，资源创建/最终错误回执由测试宿主承担。
 */
static inline int fixture_default_create(const logger_config_t *config,
					 logger_t **owner)
{
	int rc = logger_create(config, owner);
	if (rc)
		return rc;
	rc = logger_default_set(NULL, *owner);
	if (rc)
		(void)logger_destroy(owner);
	return rc;
}

static inline int fixture_default_destroy(logger_t **owner)
{
	if (!owner)
		return -EINVAL;
	if (!*owner)
		return logger_default_set(NULL, NULL);
	int rc = logger_default_set(*owner, NULL);
	return rc ? rc : logger_destroy(owner);
}
#endif
