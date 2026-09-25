#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include <sys/mman.h>
static _Atomic int entered, proceed;
size_t __real_logger_format_line(const logger_t *, const logger_message_t *,
				 char *, size_t);
size_t __wrap_logger_format_line(const logger_t *l, const logger_message_t *m,
				 char *out, size_t cap)
{
	atomic_store(&entered, 1);
	wait_flag(&proceed);
	return __real_logger_format_line(l, m, out, cap);
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	unlink("source.log");
	const char *mode = argv[1];
	if (!strcmp(mode, "queue-copy") || !strcmp(mode, "bounds") ||
	    !strcmp(mode, "null-source")) {
		logger_queue_t q;
		CHECK(logger_queue_init(&q, 2) == 0);
		char big[600];
		memset(big, 'M', sizeof(big));
		big[599] = 0;
		logger_message_t in = {
			.level = LOGGER_INFO,
			.metadata_mask = LOGGER_RECORD_META_MODULE |
					 LOGGER_RECORD_META_SOURCE,
			.module = "module",
			.file = "dir/test.c",
			.func = "call"
		};
		if (!strcmp(mode, "bounds"))
			in.module = in.file = in.func = big;
		if (!strcmp(mode, "null-source"))
			in.module = in.file = in.func = NULL;
		CHECK(logger_queue_push(&q, &in) == 1);
		logger_message_t out;
		CHECK(logger_queue_try_pop(&q, &out) == 1);
		CHECK(out.module != in.module && out.file != in.file &&
		      out.func != in.func);
		if (!strcmp(mode, "bounds")) {
			CHECK(strlen(out.module) == 127 &&
			      out.module[126] == '~');
			CHECK(strlen(out.file) == 255 && out.file[254] == '~');
			CHECK(strlen(out.func) == 127 && out.func[126] == '~');
		} else if (!strcmp(mode, "null-source")) {
			CHECK(!strcmp(out.module, "app") &&
			      !strcmp(out.file, "?") && !strcmp(out.func, "?"));
		} else {
			in.module = "overwrite";
			in.file = "other.c";
			in.func = "different";
			for (int i = 0; i < 8; ++i) {
				logger_message_t tmp;
				CHECK(logger_queue_push(&q, &in) == 1);
				CHECK(logger_queue_try_pop(&q, &tmp) == 1);
			}
			CHECK(!strcmp(out.module, "module") &&
			      !strcmp(out.file, "test.c") &&
			      !strcmp(out.func, "call"));
		}
		logger_queue_destroy(&q);
		return 0;
	}
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "source.log";
	c.async_mode = 1;
	c.detail = LOGGER_DETAIL_DEBUG;
	c.queue_capacity = 8;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *l = logger_create(&c);
	CHECK(l != NULL);
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	char *page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(page != MAP_FAILED);
	strcpy(page, "ephemeral-module");
	strcpy(page + 128, "/plugin/ephemeral.c");
	strcpy(page + 256, "ephemeral_function");
	if (!strcmp(mode, "log-source")) {
		logger_source_t src = { page, page + 128, page + 256, 77 };
		logger_log_source(l, LOGGER_INFO, src, "copied payload");
	} else {
		logger_log(l, LOGGER_INFO, page, page + 128, 77, page + 256,
			   "copied payload");
	}
	wait_flag(&entered);
	CHECK(munmap(page, page_size) ==
	      0); /* no source storage remains accessible */
	atomic_store(&proceed, 1);
	CHECK(logger_flush_instance_status(l) == 0);
	logger_destroy(l);
	CHECK(file_contains("source.log", "[ephemeral-module]"));
	CHECK(file_contains(
		"source.log",
		"ephemeral.c:77 ephemeral_function() copied payload"));
	return 0;
}
