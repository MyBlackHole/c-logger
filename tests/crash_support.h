#ifndef LOGGER_CRASH_SUPPORT_H
#define LOGGER_CRASH_SUPPORT_H

/* Test-only ordinary Logger contract, shared by process and VM harnesses. */
int crash_fixture_point_valid(const char *point);
void crash_fixture_write(const char *point);
void crash_fixture_verify(const char *point, unsigned recovered);
void crash_fixture_recover(const char *point);
void crash_fixture_cleanup(const char *directory);

/* Harness callback: notify the controller, then wait without flushing files. */
_Noreturn void crash_fixture_cut(const char *point);

#endif
