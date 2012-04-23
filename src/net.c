#include "taskimpl.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/poll.h>

#include <string.h>

int
netannounce(Task *t, int istcp, char *server, int port)
{
	int fd, n, proto;
	struct sockaddr_in sa;
	socklen_t sn;
	uint32_t ip;

	taskstate(t, "netannounce");
	proto = istcp ? SOCK_STREAM : SOCK_DGRAM;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	if(server != nil && strcmp(server, "*") != 0){
		if(netlookup(t, server, &ip) < 0){
			taskstate(t, "netlookup failed");
			return -1;
		}
		memmove(&sa.sin_addr, &ip, 4);
	}
	sa.sin_port = htons(port);
	if((fd = socket(AF_INET, proto, 0)) < 0){
		taskstate(t, "socket failed");
		return -1;
	}

	/* set reuse flag for tcp */
	if(istcp && getsockopt(fd, SOL_SOCKET, SO_TYPE, (void*)&n, &sn) >= 0){
		n = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&n, sizeof n);
	}

	if(bind(fd, (struct sockaddr*)&sa, sizeof sa) < 0){
		taskstate(t, "bind failed");
		close(fd);
		return -1;
	}

	if(proto == SOCK_STREAM)
		listen(fd, 16);

	fdnoblock(fd);
	taskstate(t, "netannounce succeeded");
	return fd;
}

int
netaccept(Task *t, int fd)
{
	int cfd, one, rc;

	fdwait(t, fd, 'r');

	taskstate(t, "netaccept");
	if((cfd = accept(fd, NULL, NULL)) < 0){
		taskstate(t, "accept failed");
		return -1;
	}
	rc = fdnoblock(cfd);
	ASSERT(rc==0, "fcntl: %s", strerror(errno));
	one = 1;
	setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one);
	taskstate(t, "netaccept succeeded");
	return cfd;
}

#define CLASS(p) ((*(unsigned char*)(p))>>6)
static int
parseip(char *name, uint32_t *ip)
{
	unsigned char addr[4];
	char *p;
	int i, x;

	p = name;
	for(i=0; i<4 && *p; i++){
		x = strtoul(p, &p, 0);
		if(x < 0 || x >= 256)
			return -1;
		if(*p != '.' && *p != 0)
			return -1;
		if(*p == '.')
			p++;
		addr[i] = x;
	}

	switch(CLASS(addr)){
	case 0:
	case 1:
		if(i == 3){
			addr[3] = addr[2];
			addr[2] = addr[1];
			addr[1] = 0;
		}else if(i == 2){
			addr[3] = addr[1];
			addr[2] = 0;
			addr[1] = 0;
		}else if(i != 4)
			return -1;
		break;
	case 2:
		if(i == 3){
			addr[3] = addr[2];
			addr[2] = 0;
		}else if(i != 4)
			return -1;
		break;
	}
	*ip = *(uint32_t*)addr;
	return 0;
}

int
netlookup(Task *t, char *name, uint32_t *ip)
{
	struct hostent *he;

	if(parseip(name, ip) >= 0)
		return 0;

	/* BUG - Name resolution blocks.  Need a non-blocking DNS. */
	/* XXXCEM - we have threads now. no problem. */
	taskstate(t, "netlookup");
	taskblocking(t);
	if((he = gethostbyname(name)) != 0){
		*ip = *(uint32_t*)he->h_addr;
		taskstate(t, "netlookup succeeded");
		tasknonblocking(t);
		return 0;
	}
	tasknonblocking(t);

	taskstate(t, "netlookup failed");
	return -1;
}

int
netdial(Task *t, int istcp, char *server, int port)
{
	int proto, fd, n, rc;
	uint32_t ip;
	struct sockaddr_in sa;
	socklen_t sn;

	if(netlookup(t, server, &ip) < 0)
		return -1;

	taskstate(t, "netdial");
	proto = istcp ? SOCK_STREAM : SOCK_DGRAM;
	if((fd = socket(AF_INET, proto, 0)) < 0){
		taskstate(t, "socket failed");
		return -1;
	}
	rc = fdnoblock(fd);
	ASSERT(rc==0, "fcntl: %s", strerror(errno));

	/* for udp */
	if(!istcp){
		n = 1;
		setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &n, sizeof n);
	}

	/* start connecting */
	memset(&sa, 0, sizeof sa);
	memmove(&sa.sin_addr, &ip, 4);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if(connect(fd, (struct sockaddr*)&sa, sizeof sa) < 0 && errno != EINPROGRESS){
		taskstate(t, "connect failed");
		close(fd);
		return -1;
	}

	/* wait for finish */
	fdwait(t, fd, 'w');
	sn = sizeof sa;
	if(getpeername(fd, (struct sockaddr*)&sa, &sn) >= 0){
		taskstate(t, "connect succeeded");
		return fd;
	}

	/* report error */
	sn = sizeof n;
	getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&n, &sn);
	if(n == 0)
		n = ECONNREFUSED;
	close(fd);
	taskstate(t, "connect failed");
	errno = n;
	return -1;
}
