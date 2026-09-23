#ifndef EXAMPLE_PLUGIN_API_H
#define EXAMPLE_PLUGIN_API_H
#include "example_sdk.h"
#include <dlfcn.h>
#include <string.h>

typedef struct {
	void *handle;
	int (*create)(const example_sdk_options_t *, example_sdk_t **);
	int (*run)(example_sdk_t *, unsigned);
	void (*destroy)(example_sdk_t *);
} example_plugin_t;
/* POSIX dlsym supplies function addresses. memcpy avoids ISO C pedantic casts
 * between function pointers and void *. Linux target asserts representation. */
static inline int example_plugin_open(example_plugin_t *p, const char *path)
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
