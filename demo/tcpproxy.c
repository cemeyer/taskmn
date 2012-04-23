#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <syslog.h>

#include <taskmn.h>

enum
{
	STACK = 32768
};

char *server;
int port;
void proxytask(ltctx *, void*);
void rwtask(ltctx *, void*);

static int*
mkfd2(int fd1, int fd2)
{
	int *a;
	
	a = malloc(2*sizeof a[0]);
	if(a == 0){
		fprintf(stderr, "out of memory\n");
		abort();
	}
	a[0] = fd1;
	a[1] = fd2;
	return a;
}

static int argc;
static char **argv;

static void
taskmain(ltctx *lt, void *unused)
{
	int cfd, fd;
	int rport;
	char remote[16];
	
	if(argc != 4){
		fprintf(stderr, "usage: tcpproxy localport server remoteport\n");
		taskexit(lt, 1);
	}
	server = argv[2];
	port = atoi(argv[3]);

	if((fd = netannounce(lt, TCP, 0, atoi(argv[1]))) < 0){
		fprintf(stderr, "cannot announce on tcp port %d: %s\n", atoi(argv[1]), strerror(errno));
		taskexit(lt, 1);
	}
	fdnoblock(fd);
	while((cfd = netaccept(lt, fd, remote, &rport)) >= 0){
		fprintf(stderr, "connection from %s:%d\n", remote, rport);
		taskcreate(lt, proxytask, (void*)cfd, STACK);
	}
}

void
proxytask(ltctx *lt, void *v)
{
	int fd, remotefd;

	fd = (int)v;
	if((remotefd = netdial(lt, TCP, server, port)) < 0){
		close(fd);
		return;
	}
	
	fprintf(stderr, "connected to %s:%d\n", server, port);

	taskcreate(lt, rwtask, mkfd2(fd, remotefd), STACK);
	taskcreate(lt, rwtask, mkfd2(remotefd, fd), STACK);
}

void
rwtask(ltctx *lt, void *v)
{
	int *a, rfd, wfd, n;
	char buf[2048];

	a = v;
	rfd = a[0];
	wfd = a[1];
	free(a);
	
	while((n = fdread(lt, rfd, buf, sizeof buf)) > 0)
		fdwrite(lt, wfd, buf, n);
	shutdown(wfd, SHUT_WR);
	close(rfd);
}

int
main(int argc_, char **argv_)
{
	openlog("tcpproxy", LOG_PERROR, LOG_USER);
	argc = argc_;
	argv = argv_;
	return libtaskmn(taskmain, 0, STACK);
}
