#define _GNU_SOURCE
#include "host_support.h"
#include "logger_internal.h"
#include "logger_adapter.h"
#include "plugin_api.h"
#include <fcntl.h>

static _Atomic int pause_format, format_entered, allow_format;
size_t __real_logger_format_line(const logger_t *, const logger_message_t *,
				 char *, size_t);
size_t __wrap_logger_format_line(const logger_t *l, const logger_message_t *m,
				 char *out, size_t cap)
{
	if (atomic_load(&pause_format)) {
		atomic_store(&format_entered, 1);
		wait_flag(&allow_format);
	}
	return __real_logger_format_line(l, m, out, cap);
}

static int close_logger(logger_t *l)
{
#ifdef TEST_BASELINE
	logger_destroy(l);
	return 0;
#else
	return logger_destroy_status(l);
#endif
}

static logger_t *make_logger(const char *path, int async)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = path;
	c.async_mode = async;
	c.queue_capacity = 4096;
	c.detail = LOGGER_DETAIL_DEBUG;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *l = logger_create(&c);
	CHECK(l != NULL);
	return l;
}

static example_sdk_t *make_sdk(example_plugin_t *p, const char *name,
			       example_log_fn log, void *user)
{
	example_sdk_options_t options = { name, log, user };
	example_sdk_t *sdk = NULL;
	CHECK(p->create(&options, &sdk) == 0);
	CHECK(sdk != NULL);
	return sdk;
}

static unsigned count_lines(const char *path)
{
	FILE *f = fopen(path, "r");
	CHECK(f != NULL);
	unsigned count = 0;
	int c;
	while ((c = fgetc(f)) != EOF)
		count += c == '\n';
	CHECK(!ferror(f));
	CHECK(fclose(f) == 0);
	return count;
}

struct receiver {
	unsigned calls;
	char message[128];
};
static void custom_log(void *user, const example_log_record_t *r)
{
	struct receiver *state = user;
	CHECK(r->message_len < sizeof(state->message));
	memcpy(state->message, r->message, r->message_len);
	state->message[r->message_len] = 0;
	++state->calls;
}

struct job {
	example_plugin_t *plugin;
	example_sdk_t *sdk;
};
static void *run_jobs(void *ptr)
{
	struct job *j = ptr;
	for (unsigned i = 0; i < 100; ++i)
		CHECK(j->plugin->run(j->sdk, i) == 0);
	return NULL;
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	unlink("a.log");
	unlink("b.log");
	unlink("shared.log");
	unlink("global.log");
	const char *mode = argv[1];
	example_plugin_t p;
	CHECK(example_plugin_open(&p, argv[2]) == 0);
	if (!strcmp(mode, "quiet")) {
		FILE *capture = tmpfile();
		CHECK(capture != NULL);
		int stdout_copy = dup(STDOUT_FILENO),
		    stderr_copy = dup(STDERR_FILENO);
		CHECK(stdout_copy >= 0 && stderr_copy >= 0);
		CHECK(dup2(fileno(capture), STDOUT_FILENO) >= 0);
		CHECK(dup2(fileno(capture), STDERR_FILENO) >= 0);
		example_sdk_t *sdk = make_sdk(&p, "quiet", NULL, NULL);
		CHECK(p.run(sdk, 1) == 0);
		p.destroy(sdk);
		CHECK(fflush(stdout) == 0 && fflush(stderr) == 0);
		CHECK(lseek(fileno(capture), 0, SEEK_END) == 0);
		CHECK(dup2(stdout_copy, STDOUT_FILENO) >= 0 &&
		      dup2(stderr_copy, STDERR_FILENO) >= 0);
		close(stdout_copy);
		close(stderr_copy);
		fclose(capture);
		CHECK(logger_process_object_count() == 0);
	} else if (!strcmp(mode, "callback-only")) {
		struct receiver r = { 0 };
		example_sdk_t *sdk = make_sdk(&p, "external", custom_log, &r);
		CHECK(p.run(sdk, 123) == 0);
		p.destroy(sdk);
		CHECK(r.calls == 1 &&
		      !strcmp(r.message, "sdk=external task=123"));
		CHECK(logger_process_object_count() == 0);
	} else if (!strcmp(mode, "invalid-options")) {
		example_sdk_t *sdk = (example_sdk_t *)1;
		CHECK(p.create(NULL, &sdk) == -1 && errno == EINVAL &&
		      sdk == NULL);
		example_sdk_options_t options = { "valid", NULL, NULL };
		CHECK(p.create(&options, NULL) == -1 && errno == EINVAL);
		char name[80];
		memset(name, 'x', sizeof(name));
		name[79] = 0;
		options.name = name;
		CHECK(p.create(&options, &sdk) == -1 && errno == EINVAL &&
		      sdk == NULL);
		CHECK(logger_process_object_count() == 0);
	} else if (!strcmp(mode, "separate")) {
		logger_t *a = make_logger("a.log", 1),
			 *b = make_logger("b.log", 1);
		example_sdk_t *sa = make_sdk(&p, "alpha", example_to_logger, a);
		example_sdk_t *sb = make_sdk(&p, "beta", example_to_logger, b);
		CHECK(p.run(sa, 1) == 0 && p.run(sb, 2) == 0);
		p.destroy(sa);
		CHECK(close_logger(a) == 0);
		CHECK(p.run(sb, 3) ==
		      0); /* one SDK/logger lifecycle does not stop another */
		p.destroy(sb);
		CHECK(close_logger(b) == 0);
		CHECK(count_lines("a.log") == 1 && count_lines("b.log") == 2);
		CHECK(log_contains("a.log", "sdk=alpha task=1"));
		CHECK(!log_contains("a.log", "sdk=beta"));
		CHECK(log_contains("b.log", "sdk=beta task=3"));
		CHECK(!log_contains("b.log", "sdk=alpha"));
	} else {
		int async = strcmp(mode, "shared-sync") != 0;
		int pending = !strcmp(mode, "unload-pending");
		int concurrent = !strcmp(mode, "concurrent");
		int percent = !strcmp(mode, "message-data");
		int global = !strcmp(mode, "global-independent");
		if (global) {
			logger_config_t c = LOGGER_DEFAULT_CONFIG();
			c.outputs = LOGGER_OUT_FILE;
			c.file_path = "global.log";
			c.rotation.mode = LOGGER_ROTATE_NONE;
			c.async_mode = 0;
			CHECK(logger_init(&c) == 0);
			LOG_INFO("global-before");
		}
		logger_t *l = make_logger("shared.log", async);
		atomic_store(&pause_format, pending);
		example_sdk_t *a = make_sdk(&p, percent ? "%n%s%%" : "alpha",
					    example_to_logger, l);
		example_sdk_t *b = make_sdk(&p, "beta", example_to_logger, l);
		unsigned expected = 2;
		if (concurrent) {
			pthread_t threads[8];
			struct job jobs[8];
			for (unsigned i = 0; i < 8; ++i) {
				jobs[i] = (struct job){ &p, i % 2 ? b : a };
				CHECK(pthread_create(&threads[i], NULL,
						     run_jobs, &jobs[i]) == 0);
			}
			for (unsigned i = 0; i < 8; ++i)
				CHECK(pthread_join(threads[i], NULL) == 0);
			expected = 800;
		} else {
			CHECK(p.run(a, 1) == 0 && p.run(b, 2) == 0);
		}
		if (pending)
			wait_flag(&format_entered);
		p.destroy(a);
		p.destroy(b);
		CHECK(logger_get_state(l) == LOGGER_STATE_RUNNING);
		CHECK(dlclose(p.handle) == 0);
		p.handle = NULL;
		atomic_store(&allow_format, 1);
		LOGGER_INFO(l, "host", "owner-can-still-log");
		CHECK(logger_flush_instance_status(l) == 0);
		logger_io_metrics_t io;
		logger_get_io_metrics(l, &io);
		CHECK(io.emitted_records == expected + 1 &&
		      io.failed_records == 0);
		CHECK(close_logger(l) == 0);
		CHECK(count_lines("shared.log") == expected + 1);
		CHECK(log_contains("shared.log", "example_sdk.c:"));
		CHECK(log_contains("shared.log", "example_sdk_run()"));
		CHECK(log_contains("shared.log", "owner-can-still-log"));
		if (percent)
			CHECK(log_contains("shared.log", "sdk=%n%s%% task=1"));
		if (global) {
			LOG_INFO("global-after");
			CHECK(logger_flush_status() == 0);
			logger_shutdown();
			CHECK(count_lines("global.log") == 2);
			CHECK(!log_contains("global.log", "sdk="));
		}
	}
	if (p.handle)
		CHECK(dlclose(p.handle) == 0);
	CHECK(logger_process_object_count() == 0);
	return 0;
}
