#define _GNU_SOURCE
#include "logger.h" /* declarations only; this executable does NOT link logger */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                           \
	do {                                                               \
		if (!(x)) {                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#x);                                       \
			return 1;                                          \
		}                                                          \
	} while (0)
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	unlink("dlclose.log");
	for (int n = 0; n < 20; ++n) {
		void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
		CHECK(handle != NULL);
		logger_t *(*create_fn)(const logger_config_t *);
		void (*log_fn)(logger_t *, logger_level_t, const char *,
			       const char *, int, const char *, const char *,
			       ...);
		int (*destroy_fn)(logger_t *);
#define GET(function, symbol)                                   \
	do {                                                    \
		void *p = dlsym(handle, symbol);                \
		_Static_assert(sizeof(p) == sizeof(function),   \
			       "Linux pointer representation"); \
		CHECK(p != NULL);                               \
		memcpy(&function, &p, sizeof(p));               \
	} while (0)
		GET(create_fn, "logger_create");
		GET(log_fn, "logger_log");
		GET(destroy_fn, "logger_destroy_status");
#undef GET
		logger_config_t c = LOGGER_DEFAULT_CONFIG();
		c.outputs = LOGGER_OUT_FILE;
		c.file_path = "dlclose.log";
		c.async_mode = 1;
		c.queue_capacity = 32;
		c.rotation.mode = LOGGER_ROTATE_NONE;
		logger_t *l = create_fn(&c);
		CHECK(l != NULL);
		for (int j = 0; j < 10; ++j)
			log_fn(l, LOGGER_INFO, "dlclose", __FILE__, __LINE__,
			       __func__, "n=%d j=%d", n, j);
		CHECK(destroy_fn(l) ==
		      0); /* no calls/workers left before dlclose */
		l = NULL;
		CHECK(dlclose(handle) == 0);
	}
	FILE *f = fopen("dlclose.log", "rb");
	CHECK(f != NULL);
	int c;
	unsigned records = 0;
	while ((c = fgetc(f)) != EOF)
		records += c == '\n';
	CHECK(!ferror(f));
	CHECK(fclose(f) == 0);
	CHECK(records == 200);
	return 0;
}
