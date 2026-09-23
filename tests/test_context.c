#define LOG_MODULE "rpc"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void emit(void)
{
	LOG_INFO("request accepted");
}
int main(void)
{
	unlink("./context.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./context.log";
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.detail = LOGGER_DETAIL_DEBUG;
	if (logger_init(&c))
		return 1;
	logger_context_t ctx = { .request_id = "req-42",
				 .session_id = "sess-7",
				 .trace_id = "trace-abcd" };
	logger_context_set(&ctx);
	emit();
	logger_context_clear();
	logger_shutdown();
	FILE *f = fopen("./context.log", "r");
	if (!f)
		return 2;
	char b[8192];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[n] = 0;
	if (!strstr(b, "request=req-42"))
		return 3;
	if (!strstr(b, "session=sess-7"))
		return 4;
	if (!strstr(b, "trace=trace-abcd"))
		return 5;
	if (!strstr(b, "test_context.c:"))
		return 6;
	return 0;
}
