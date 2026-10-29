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
/*
 * POSIX dlsym 提供函数地址。memcpy 避免 ISO C 严格检查下
 * 函数指针与 void * 之间的强制转换。Linux 目标断言其表示形式。
 */
static inline int example_plugin_open(struct example_plugin *p,
				      const char *path)
{
	void *address;

	_Static_assert(sizeof(p->create) == sizeof(address),
		       "Linux pointer size");
	_Static_assert(sizeof(p->run) == sizeof(address), "Linux pointer size");
	_Static_assert(sizeof(p->destroy) == sizeof(address),
		       "Linux pointer size");
	memset(p, 0, sizeof(*p));
	p->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!p->handle)
		return -1;
	address = dlsym(p->handle, "example_sdk_create");
	if (!address)
		goto close_handle;
	memcpy(&p->create, &address, sizeof(address));
	address = dlsym(p->handle, "example_sdk_run");
	if (!address)
		goto close_handle;
	memcpy(&p->run, &address, sizeof(address));
	address = dlsym(p->handle, "example_sdk_destroy");
	if (!address)
		goto close_handle;
	memcpy(&p->destroy, &address, sizeof(address));
	return 0;
close_handle:
	dlclose(p->handle);
	p->handle = NULL;
	return -1;
}
#endif /* EXAMPLE_PLUGIN_API_H */
