#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>

#include <taskmn.h>

enum
{
	STACK = 32768
};

char *server;
char *url;

static void fetchtask(ltctx *, void*);

int argc;
char **argv;

static void
taskmain(ltctx *lt, void *unused)
{
	int i, n;
	
	if(argc != 4){
		fprintf(stderr, "usage: httpload n server url\n");
		fprintf(stderr, "   ex: httpload 4 127.0.0.1 /\n");
		taskexit(lt, 1);
	}
	n = atoi(argv[1]);
	server = argv[2];
	url = argv[3];

	for(i=0; i<n; i++){
		taskcreate(lt, fetchtask, 0, STACK);
		while(taskyield(lt) > 1)
			;
		//taskdelay(lt, 1/*ms*/);
	}
}

void
fetchtask(ltctx *lt, void *v)
{
	int fd, n;
	char buf[512];
	
	fprintf(stderr, "starting...\n");
	for(;;){
		if((fd = netdial(lt, TCP, server, 80)) < 0){
			fprintf(stderr, "dial %s: %s (%s)\n", server, strerror(errno), taskgetstate(lt));
			continue;
		}
		snprintf(buf, sizeof buf, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", url, server);
		fdwrite(lt, fd, buf, strlen(buf));
		while((n = fdread(lt, fd, buf, sizeof buf)) > 0)
			;
		close(fd);
		write(1, ".", 1);
	}
}

int
main(int argc_, char **argv_)
{
	openlog("httpload", LOG_PERROR, LOG_USER);
	argc = argc_;
	argv = argv_;
	return libtaskmn(taskmain, 0/*arg*/, STACK);
}
