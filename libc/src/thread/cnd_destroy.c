#include <threads.h>

void cnd_destroy(cnd_t *c)
{
	/* Private condition variables do not need destruction work. */
}
