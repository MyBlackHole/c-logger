#include "example_sdk.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct example_sdk {
	char name[64];
	void (*log)(void *user, const struct example_log_record *record);
	void *log_user;
};

int example_sdk_create(const struct example_sdk_options *options,
		       example_sdk_t **out)
{
	if (!out) {
		errno = EINVAL;
		return -1;
	}
	*out = NULL;
	if (!options || !options->name) {
		errno = EINVAL;
		return -1;
	}
	size_t n = strlen(options->name);

	if (!n || n >= sizeof(((example_sdk_t *)0)->name)) {
		errno = EINVAL;
		return -1;
	}
	example_sdk_t *sdk = calloc(1, sizeof(*sdk));

	if (!sdk)
		return -1;
	memcpy(sdk->name, options->name, n + 1);
	sdk->log = options->log;
	sdk->log_user = options->log_user;
	*out = sdk;
	return 0;
}

int example_sdk_run(example_sdk_t *sdk, unsigned int task)
{
	if (!sdk) {
		errno = EINVAL;
		return -1;
	}
	/* 不初始化 logger、不注册全局写者，也不隐式创建文件。 */
	if (sdk->log) {
		char message[128];
		int n = snprintf(message, sizeof(message), "sdk=%s task=%u",
				 sdk->name, task);
		if (n < 0 || (size_t)n >= sizeof(message)) {
			errno = EOVERFLOW;
			return -1;
		}
		const struct example_log_record record = {
			EXAMPLE_LOG_INFO, sdk->name, __FILE__, __func__,
			__LINE__,	  message,   (size_t)n
		};
		sdk->log(sdk->log_user, &record);
	}
	/* 真实业务错误独立于诊断日志返回。 */
	return 0;
}

void example_sdk_destroy(example_sdk_t *sdk)
{
	free(sdk);
}
