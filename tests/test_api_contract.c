#include "logger.h"
#include <errno.h>

int main(void)
{
	errno = 0;
	if (logger_create(NULL) != NULL || errno != EINVAL)
		return 1;
	return 0;
}
