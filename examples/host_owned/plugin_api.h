#ifndef EXAMPLE_PLUGIN_API_H
#define EXAMPLE_PLUGIN_API_H
#include "example_sdk.h"
#include <dlfcn.h>
#include <string.h>

struct example_plugin {
	void *handle;
	int (*create)(const struct example_sdk_options *options,
		      example_sdk_t **out);
	int (*run)(example_sdk_t *sdk, unsigned int task);
	void (*destroy)(example_sdk_t *sdk);
};
/* POSIX dlsym 提供函数地址。memcpy 避免 ISO C 严格检查下
 * 函数指针与 void * 之间的强制转换。Linux 目标断言其表示形式。
 */
static inline int example_plugin_open(struct example_plugin *p,
				      const char *path)
{
	memset(p, 0, sizeof(*p));
	p->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!p->handle)
		return -1;
#define EXAMPLE_SYMBOL(member, symbol)                               \
	do {                                                         \
		void *address = dlsym(p->handle, symbol);            \
		_Static_assert(sizeof(p->member) == sizeof(address), \
			       "Linux pointer size");                \
		if (!address) {                                      \
			dlclose(p->handle);                          \
			p->handle = NULL;                            \
			return -1;                                   \
		}                                                    \
		memcpy(&p->member, &address, sizeof(address));       \
	} while (0)
	EXAMPLE_SYMBOL(create, "example_sdk_create");
	EXAMPLE_SYMBOL(run, "example_sdk_run");
	EXAMPLE_SYMBOL(destroy, "example_sdk_destroy");
#undef EXAMPLE_SYMBOL
	return 0;
}
#endif
