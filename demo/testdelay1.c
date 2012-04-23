#include <syslog.h>

#include <taskmn.h>

static void
taskmain(ltctx *lt, void *unused)
{
	taskdelay(lt, 1000);
}

int
main(int argc, char **argv)
{
	openlog("testdelay1", LOG_PERROR, LOG_USER);
	return libtaskmn(taskmain, 0/*arg*/, 32768/*stack*/);
}
