#define _GNU_SOURCE
#include <stdbool.h>
#include "logger.h" /* 仅有声明；此可执行文件不链接 logger */
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool check(bool condition, const char *expression, int line)
{
	if (!condition)
		fprintf(stderr, "%s:%d: %s\n", __FILE__, line, expression);
	return condition;
}

#define CHECK(x) check((x), #x, __LINE__)
int main(int argc, char **argv)
{
	if (!CHECK(argc == 2))
		return 1;
	unlink("dlclose.log");
	for (int n = 0; n < 20; ++n) {
		void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
		if (!CHECK(handle != NULL))
			return 1;
		int (*initialize_fn)(void);
		int (*create_fn)(const logger_config_t *config, logger_t **out);
		int (*log_fn)(logger_t *logger, logger_level_t level,
			      const char *module, const char *file, int line,
			      const char *function, const char *format, ...);
		int (*destroy_fn)(logger_t **owner);
		void *p;

		_Static_assert(sizeof(p) == sizeof(initialize_fn),
			       "Linux pointer representation");
		_Static_assert(sizeof(p) == sizeof(create_fn),
			       "Linux pointer representation");
		_Static_assert(sizeof(p) == sizeof(log_fn),
			       "Linux pointer representation");
		_Static_assert(sizeof(p) == sizeof(destroy_fn),
			       "Linux pointer representation");
		p = dlsym(handle, "logger_process_init");
		if (!CHECK(p != NULL))
			return 1;
		memcpy(&initialize_fn, &p, sizeof(p));
		if (!CHECK(initialize_fn() == 0))
			return 1;
		p = dlsym(handle, "logger_create");
		if (!CHECK(p != NULL))
			return 1;
		memcpy(&create_fn, &p, sizeof(p));
		p = dlsym(handle, "logger_log");
		if (!CHECK(p != NULL))
			return 1;
		memcpy(&log_fn, &p, sizeof(p));
		p = dlsym(handle, "logger_destroy");
		if (!CHECK(p != NULL))
			return 1;
		memcpy(&destroy_fn, &p, sizeof(p));
		if (!CHECK(dlsym(handle, "logger_destroy_status") == NULL))
			return 1;
		if (!CHECK(dlsym(handle, "logger_init") == NULL))
			return 1;
		if (!CHECK(dlsym(handle, "logger_shutdown") == NULL))
			return 1;
		if (!CHECK(dlsym(handle, "logger_shutdown_status") == NULL))
			return 1;
		if (!CHECK(dlsym(handle, "logger_flush_status") == NULL))
			return 1;
		if (!CHECK(dlsym(handle, "logger_flush_instance") == NULL))
			return 1;
		if (!CHECK(dlsym(handle, "logger_flush_instance_status") ==
			   NULL))
			return 1;
		if (!CHECK(dlsym(handle, "logger_reopen_instance") == NULL))
			return 1;
		logger_config_t c = LOGGER_DEFAULT_CONFIG();
		c.outputs = LOGGER_OUT_FILE;
		c.file_path = "dlclose.log";
		c.async_mode = 1;
		c.queue_capacity = 32;
		c.rotation.mode = LOGGER_ROTATE_NONE;
		logger_t *l = NULL;
		if (!CHECK(create_fn(&c, &l) == 0))
			return 1;
		if (!CHECK(l != NULL))
			return 1;
		for (int j = 0; j < 10; ++j) {
			errno = EDOM;
			if (!CHECK(log_fn(l, LOGGER_INFO, "dlclose", __FILE__,
					  __LINE__, __func__, "n=%d j=%d", n,
					  j) == 0))
				return 1;
			if (!CHECK(errno == EDOM))
				return 1;
		}
		if (!CHECK(destroy_fn(&l) == 0))
			return 1; /* dlclose 前不再有调用或工作线程 */
		if (!CHECK(l == NULL))
			return 1;
		if (!CHECK(dlclose(handle) == 0))
			return 1;
	}
	FILE *f = fopen("dlclose.log", "rb");
	if (!CHECK(f != NULL))
		return 1;
	int c;
	unsigned int records = 0;
	while ((c = fgetc(f)) != EOF)
		records += c == '\n';
	if (!CHECK(!ferror(f)))
		return 1;
	if (!CHECK(fclose(f) == 0))
		return 1;
	if (!CHECK(records == 200))
		return 1;
	return 0;
}
