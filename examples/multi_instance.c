#include "logger.h"
#include <errno.h>
#include <stdio.h>
int main(void)
{
	logger_config_t net_cfg = LOGGER_DEFAULT_CONFIG();
	net_cfg.outputs = LOGGER_OUT_FILE;
	net_cfg.file_path = "./network.log";
	net_cfg.ident = "network";
	logger_config_t storage_cfg = LOGGER_DEFAULT_CONFIG();
	storage_cfg.outputs = LOGGER_OUT_FILE;
	storage_cfg.file_path = "./storage.log";
	storage_cfg.ident = "storage";

	logger_t *network = logger_create(&net_cfg);
	if (!network) {
		perror("logger_create network");
		return 1;
	}
	logger_t *storage = logger_create(&storage_cfg);
	if (!storage) {
		perror("logger_create storage");
		logger_destroy(network);
		return 1;
	}

	LOGGER_INFO(network, "network", "connected fd=%d", 42);
	LOGGER_DEBUG(network, "network", "received bytes=%zu", (size_t)4096);
	LOGGER_INFO(storage, "storage", "volume opened id=%s", "vol-1");
	LOGGER_ERROR(storage, "storage", "write failed errno=%d", EIO);

	/* Stop/join every thread using these pointers before destroy. */
	logger_destroy(storage);
	logger_destroy(network);
	return 0;
}
