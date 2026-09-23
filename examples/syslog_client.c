#include "logger.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3 ||
	    (argc == 3 && strcmp(argv[2], "--deferred"))) {
		fprintf(stderr, "usage: %s SYSLOG_PATH [--deferred]\n",
			argv[0]);
		return 2;
	}
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE | LOGGER_OUT_SYSLOG;
	cfg.file_path = "./syslog_client.log";
	cfg.ident = "example-client";
	cfg.queue_capacity = 1024;
	cfg.syslog.path = argv[1];
	cfg.syslog.facility = LOGGER_SYSLOG_FACILITY_LOCAL0;
	cfg.syslog.reconnect_interval_ms = 1000;
	if (argc == 3)
		cfg.syslog.startup = LOGGER_SYSLOG_START_DEFERRED;
	logger_t *log = logger_create(&cfg);
	if (!log) {
		perror("logger_create");
		return 1;
	}

	LOGGER_INFO(log, "example", "host-owned file and Syslog output");
	int rc = logger_flush_instance_status(log);
	int error = rc ? errno : 0;
	logger_syslog_metrics_t metrics;
	if (logger_get_syslog_metrics(log, &metrics) == 0) {
		printf("local_connected=%d sent=%" PRIu64 " failed=%" PRIu64
		       " last_error=%d\n",
		       metrics.connected, metrics.sent_records,
		       metrics.failed_records, metrics.last_error);
	} else if (!error)
		error = errno;
	/* No other users exist. Even an I/O-error return frees the instance. */
	if (logger_destroy_status(log) != 0 && !error)
		error = errno;
	if (error) {
		fprintf(stderr, "Logger output had an error: %s\n",
			strerror(error));
		return 1;
	}
	return 0;
}
