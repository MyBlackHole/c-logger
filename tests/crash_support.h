#ifndef LOGGER_CRASH_SUPPORT_H
#define LOGGER_CRASH_SUPPORT_H

#include <stdbool.h>

/* 仅用于测试的普通 Logger 契约，供进程 SIGKILL 测试框架使用。 */
bool crash_fixture_point_valid(const char *point);
void crash_fixture_write(const char *point);
void crash_fixture_verify(const char *point, unsigned int recovered);
void crash_fixture_recover(const char *point);
void crash_fixture_cleanup(const char *directory);

/* 测试框架回调：通知控制方，然后等待，不刷新文件。 */
_Noreturn void crash_fixture_cut(const char *point);

#endif /* LOGGER_CRASH_SUPPORT_H */
