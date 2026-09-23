#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
int main(void)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "format-bound.log";
	c.async_mode = 0;
	c.detail = LOGGER_DETAIL_DEBUG;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *l = logger_create(&c);
	CHECK(l != NULL);
	logger_message_t m = { .level = LOGGER_INFO,
			       .ts = { 1700000000, 123456000 },
			       .pid = 123,
			       .tid = 456,
			       .line = 77 };
	memset(m.text, 'X', sizeof(m.text) - 1);
	memset(m.module_storage, 'M', sizeof(m.module_storage) - 1);
	memset(m.file_storage, 'F', sizeof(m.file_storage) - 1);
	memset(m.function_storage, 'U', sizeof(m.function_storage) - 1);
	memset(m.request_id, 'R', sizeof(m.request_id) - 1);
	memset(m.trace_id, 'T', sizeof(m.trace_id) - 1);
	memset(m.session_id, 'S', sizeof(m.session_id) - 1);
	m.module = m.module_storage;
	m.file = m.file_storage;
	m.func = m.function_storage;
	CHECK(logger_format_line(l, &m, NULL, 0) == 0);
	unsigned char b[LOGGER_LINE_MAX + 2];
	for (size_t cap = 0; cap <= LOGGER_LINE_MAX; ++cap) {
		memset(b, 0xa5, sizeof(b));
		size_t n = logger_format_line(l, &m, (char *)b + 1, cap);
		CHECK(b[0] == 0xa5 && b[cap + 1] == 0xa5);
		if (cap)
			CHECK(n < cap && b[n + 1] == 0);
		if (cap > 1)
			CHECK(n > 0 && b[n] == '\n');
		if (cap == LOGGER_LINE_MAX) {
			CHECK(n <
			      cap - 1); /* all bounded source/context/message fit */
			CHECK(b[n - 1] == 'X');
		}
	}
	CHECK(logger_destroy_status(l) == 0);
	return 0;
}
