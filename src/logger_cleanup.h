#ifndef LOGGER_CLEANUP_H
#define LOGGER_CLEANUP_H

/*
 * Internal lexical resource helpers, intentionally not part of the public API.
 *
 * These helpers use the GCC/Clang cleanup attribute in the same spirit as
 * Linux's scoped cleanup helpers. They cover ordinary C scope exit (including
 * return/goto out of scope), not pthread cancellation, longjmp, _exit, signal
 * handlers, or cross-thread ownership.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__clang__)
#if !__has_attribute(cleanup)
#error "c-logger internal cleanup helpers require __attribute__((cleanup))"
#endif
#elif !defined(__GNUC__)
#error "c-logger internal cleanup helpers require GCC/Clang cleanup support"
#endif

#define LOGGER_CLEANUP(function) __attribute__((cleanup(function)))

static inline void *logger_cleanup_load_ptr(const void *slot)
{
	void *value = NULL;
	memcpy(&value, slot, sizeof(value));
	return value;
}

static inline void logger_cleanup_store_ptr(void *slot, void *value)
{
	memcpy(slot, &value, sizeof(value));
}

static inline void logger_cleanup_freep(void *slot)
{
	void *value = logger_cleanup_load_ptr(slot);
	logger_cleanup_store_ptr(slot, NULL);
	free(value);
}

static inline void logger_cleanup_close_fd(int *slot)
{
	int fd = *slot;
	*slot = -1;
	if (fd < 0)
		return;
	int saved = errno;
	(void)close(fd);
	errno = saved;
}

static inline void logger_cleanup_fclosep(FILE **slot)
{
	FILE *file = *slot;
	*slot = NULL;
	if (!file)
		return;
	int saved = errno;
	(void)fclose(file);
	errno = saved;
}

static inline void *logger_cleanup_take_ptr(void *slot)
{
	void *value = logger_cleanup_load_ptr(slot);
	logger_cleanup_store_ptr(slot, NULL);
	return value;
}

static inline int logger_cleanup_take_fd(int *slot)
{
	int fd = *slot;
	*slot = -1;
	return fd;
}

#define LOGGER_AUTO_FREE LOGGER_CLEANUP(logger_cleanup_freep)
#define LOGGER_AUTO_FD LOGGER_CLEANUP(logger_cleanup_close_fd)
#define LOGGER_AUTO_FILE LOGGER_CLEANUP(logger_cleanup_fclosep)

#define LOGGER_TAKE_PTR(variable) logger_cleanup_take_ptr(&(variable))
#define LOGGER_TAKE_FD(variable) logger_cleanup_take_fd(&(variable))

#endif
