#define _GNU_SOURCE
#include "audit_support.h"

static _Atomic int run = 1, successes;
static int transient_error(int e)
{
	return e == EAGAIN || e == ENODEV || e == ESHUTDOWN;
}
static void *producer(void *unused)
{
	(void)unused;
	audit_event_t e = audit_test_event("UPDATE");
	while (atomic_load(&run)) {
		if (audit_write(&e) == 0)
			atomic_fetch_add(&successes, 1);
		else {
			CHECK(transient_error(errno));
			nap_ms();
		}
	}
	return NULL;
}
static void *reader(void *unused)
{
	(void)unused;
	while (atomic_load(&run)) {
		char id[33];
		if (audit_instance_id_copy(id) == 0)
			CHECK(strlen(id) == 32);
		else
			CHECK(transient_error(errno));
		audit_status_t s;
		CHECK(audit_get_status(&s) == 0);
		CHECK(s.state == AUDIT_STATE_IDLE ||
		      s.state == AUDIT_STATE_STARTING ||
		      s.state == AUDIT_STATE_STOPPING ||
		      s.state == AUDIT_STATE_RUNNING);
		const char *snapshot = audit_instance_id();
		CHECK(strlen(snapshot) == 0 || strlen(snapshot) == 32);
		if (audit_flush() != 0)
			CHECK(transient_error(errno));
		nap_ms();
	}
	return NULL;
}
static void verify_instances(unsigned expected)
{
	FILE *f = fopen("app.audit.log", "r");
	CHECK(f != NULL);
	char line[8192], previous[33] = { 0 };
	unsigned starts = 0, stops = 0;
	int open_session = 0;
	unsigned long long sequence = 0;
	while (fgets(line, sizeof(line), f)) {
		char id[33];
		unsigned long long seq;
		char *p = strstr(line, "instance=");
		CHECK(p && sscanf(p, "instance=%32s seq=%llu", id, &seq) == 2);
		if (strcmp(id, previous)) {
			CHECK(!open_session && seq == 1 &&
			      strstr(line, "event=\"AUDIT_START\""));
			memcpy(previous, id, 33);
			sequence = 0;
			open_session = 1;
			++starts;
		}
		CHECK(open_session && seq == sequence + 1);
		sequence = seq;
		if (strstr(line, "event=\"AUDIT_STOP\"")) {
			++stops;
			open_session = 0;
		}
	}
	CHECK(!ferror(f) && fclose(f) == 0);
	CHECK(!open_session && starts == expected && stops == expected);
	CHECK(audit_verify_file("app.audit.log") == 0);
}
static void *transactions(void *unused)
{
	(void)unused;
	for (int i = 0; i < 40; ++i) {
		audit_event_t e = audit_test_event("TRANSACTION");
		CHECK(audit_begin(&e) == 0 && e.transaction_id != 0);
		CHECK(audit_end(&e, AUDIT_SUCCESS, 0) == 0);
	}
	return NULL;
}
static void transaction_check(void)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	pthread_t threads[8];
	for (unsigned i = 0; i < 8; ++i)
		CHECK(!pthread_create(&threads[i], NULL, transactions, NULL));
	for (unsigned i = 0; i < 8; ++i)
		CHECK(!pthread_join(threads[i], NULL));
	CHECK(audit_shutdown_status() == 0);
	unsigned char attempts[321] = { 0 }, results[321] = { 0 };
	FILE *f = fopen("app.audit.log", "r");
	CHECK(f != NULL);
	char line[8192];
	while (fgets(line, sizeof(line), f)) {
		if (!strstr(line, "event=\"TRANSACTION\""))
			continue;
		char *p = strstr(line, " txn=");
		unsigned long long txn;
		CHECK(p && sscanf(p, " txn=%llu", &txn) == 1 && txn > 0 &&
		      txn <= 320);
		if (strstr(line, "phase=ATTEMPT"))
			++attempts[txn];
		else
			++results[txn];
	}
	CHECK(!ferror(f) && !fclose(f));
	for (unsigned i = 1; i <= 320; ++i)
		CHECK(attempts[i] == 1 && results[i] == 1);
	verify_instances(1);
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char dir[] = "/tmp/audit-concurrency-XXXXXX";
	enter_temp(dir);
	if (!strcmp(argv[1], "transactions")) {
		transaction_check();
		audit_test_cleanup(dir);
		return 0;
	}
	CHECK(!strcmp(argv[1], "lifetimes"));
	pthread_t writers[6], readers[2];
	audit_config_t c = audit_test_config();
	for (unsigned i = 0; i < 6; ++i)
		CHECK(!pthread_create(&writers[i], NULL, producer, NULL));
	for (unsigned i = 0; i < 2; ++i)
		CHECK(!pthread_create(&readers[i], NULL, reader, NULL));
	for (unsigned i = 0; i < 16; ++i) {
		CHECK(audit_init(&c) == 0);
		int previous = atomic_load(&successes);
		for (int n = 0; n < 5000 && atomic_load(&successes) == previous;
		     ++n)
			nap_ms();
		CHECK(atomic_load(&successes) > previous);
		CHECK(audit_shutdown_status() == 0);
	}
	atomic_store(&run, 0);
	for (unsigned i = 0; i < 6; ++i)
		CHECK(!pthread_join(writers[i], NULL));
	for (unsigned i = 0; i < 2; ++i)
		CHECK(!pthread_join(readers[i], NULL));
	verify_instances(16);
	audit_test_cleanup(dir);
	return 0;
}
