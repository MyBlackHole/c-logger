#include "logger.h"
#include "audit.h"
#include "console.h"
int main()
{
	auto l = LOGGER_DEFAULT_CONFIG();
	auto a = AUDIT_DEFAULT_CONFIG();
	auto c = CONSOLE_DEFAULT_CONFIG();
	auto s = LOGGER_SOURCE();
	return l.version != 1 || a.version != 1 || c.version != 1 ||
	       s.line <= 0;
}
