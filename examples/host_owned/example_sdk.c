#include "example_sdk.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct example_sdk {
	char name[64];
	example_log_fn log;
	void *log_user;
};

int example_sdk_create(const example_sdk_options_t *options,
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

int example_sdk_run(example_sdk_t *sdk, unsigned task)
{
	if (!sdk) {
		errno = EINVAL;
		return -1;
	}
	/* No logger init, global writer registration or implicit file creation. */
	if (sdk->log) {
		char message[128];
		int n = snprintf(message, sizeof(message), "sdk=%s task=%u",
				 sdk->name, task);
		if (n < 0 || (size_t)n >= sizeof(message)) {
			errno = EOVERFLOW;
			return -1;
		}
		const example_log_record_t record = {
			EXAMPLE_LOG_INFO, sdk->name, __FILE__, __func__,
			__LINE__,	  message,   (size_t)n
		};
		sdk->log(sdk->log_user, &record);
	}
	/* Real business errors are returned independently of diagnostic logging. */
	return 0;
}

void example_sdk_destroy(example_sdk_t *sdk)
{
	free(sdk);
}
