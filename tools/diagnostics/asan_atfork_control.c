#include <pthread.h>

/* 仅供独立诊断的负向对照；不得链接进 Logger 或正常测试。
 * 探针本身不注册 atfork。记录真实注册数，删除回调的构建必须标为反例。
 * 此文件不插桩，避免在检测器初始化回调内递归初始化检测器。
 */
#ifndef DROP_SANITIZER_ATFORK
#define DROP_SANITIZER_ATFORK 0
#endif
unsigned sanitizer_atfork_registrations;
unsigned sanitizer_atfork_dropped;
int __real_pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
__attribute__((no_sanitize("address", "undefined"))) int
__wrap_pthread_atfork(void (*prepare)(void), void (*parent)(void),
		      void (*child)(void))
{
	++sanitizer_atfork_registrations;
	if (DROP_SANITIZER_ATFORK) {
		++sanitizer_atfork_dropped;
		return 0;
	}
	return __real_pthread_atfork(prepare, parent, child);
}
