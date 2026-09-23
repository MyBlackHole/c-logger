#include "audit.h"
#include "logger.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
static logger_config_t lc;
static _Atomic int lok, lalready;
static void *li(void *x)
{
	(void)x;
	if (logger_init(&lc) == 0)
		atomic_fetch_add(&lok, 1);
	else if (errno == EALREADY)
		atomic_fetch_add(&lalready, 1);
	return 0;
}
static audit_config_t ac;
static _Atomic int aok, aalready;
static void *ai(void *x)
{
	(void)x;
	if (audit_init(&ac) == 0)
		atomic_fetch_add(&aok, 1);
	else if (errno == EALREADY)
		atomic_fetch_add(&aalready, 1);
	return 0;
}
int main(void)
{
	unlink("./reinit.log");
	lc = LOGGER_DEFAULT_CONFIG();
	lc.outputs = LOGGER_OUT_FILE;
	lc.file_path = "./reinit.log";
	lc.rotation.mode = LOGGER_ROTATE_NONE;
	if (logger_init(&lc))
		return 1;
	errno = 0;
	if (logger_init(&lc) == 0 || errno != EALREADY)
		return 2;
	logger_shutdown();
	if (logger_init(&lc))
		return 3;
	logger_shutdown();
	pthread_t lt[8];
	for (int i = 0; i < 8; i++)
		pthread_create(&lt[i], 0, li, 0);
	for (int i = 0; i < 8; i++)
		pthread_join(lt[i], 0);
	if (atomic_load(&lok) != 1 || atomic_load(&lalready) != 7)
		return 4;
	logger_shutdown();

	unlink("./reinit.audit.log");
	unlink("./reinit.audit.state");
	unlink("./reinit.audit.lock");
	ac = AUDIT_DEFAULT_CONFIG();
	ac.log_dir = ".";
	ac.name = "reinit";
	ac.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&ac))
		return 5;
	errno = 0;
	if (audit_init(&ac) == 0 || errno != EALREADY)
		return 6;
	audit_shutdown();
	if (audit_init(&ac))
		return 7;
	audit_shutdown();
	pthread_t at[8];
	for (int i = 0; i < 8; i++)
		pthread_create(&at[i], 0, ai, 0);
	for (int i = 0; i < 8; i++)
		pthread_join(at[i], 0);
	if (atomic_load(&aok) != 1 || atomic_load(&aalready) != 7)
		return 8;
	audit_shutdown();
	return 0;
}