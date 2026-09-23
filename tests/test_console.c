#include "console.h"
#include <stdio.h>
int main(void)
{
	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	c.color = CONSOLE_COLOR_NEVER;
	c.verbosity = CONSOLE_NORMAL;
	console_init(&c);
	if (console_print("id=%d\n", 42))
		return 1; /* stdout result channel */
	if (console_info("operation completed"))
		return 2;
	if (console_warn("certificate expires soon"))
		return 3;
	if (console_error("example error"))
		return 4;
	if (console_verbose("must be hidden at normal verbosity"))
		return 5;
	console_set_verbosity(CONSOLE_DEBUG);
	if (console_debug("debug enabled"))
		return 6;
	return 0;
}
