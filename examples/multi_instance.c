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

	logger_t *network = NULL;
	int rc = logger_create(&net_cfg, &network);
	if (rc) {
		errno = -rc;
		perror("logger_create network");
		return 1;
	}
	logger_t *storage = NULL;
	rc = logger_create(&storage_cfg, &storage);
	if (rc) {
		errno = -rc;
		perror("logger_create storage");
		logger_destroy(&network);
		return 1;
	}

	LOGGER_INFO(network, "network", "connected fd=%d", 42);
	LOGGER_DEBUG(network, "network", "received bytes=%zu", (size_t)4096);
	LOGGER_INFO(storage, "storage", "volume opened id=%s", "vol-1");
	LOGGER_ERROR(storage, "storage", "write failed errno=%d", EIO);

	/* 销毁前停止并回收使用这些指针的每个线程。 */
	int storage_rc = logger_destroy(&storage);
	int network_rc = logger_destroy(&network);
	if (storage_rc || network_rc) {
		errno = storage_rc ? -storage_rc : -network_rc;
		perror("logger_destroy");
		return 1;
	}
	return 0;
}
