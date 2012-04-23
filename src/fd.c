#include <sys/poll.h>

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include "taskimpl.h"

static uvlong	nsec(void);
static void	_startfdtask(Task *);

#define TRY_POLL_LOCK	trymtx(&lt->polllock)
#define POLL_LOCK	lockmtx(&lt->polllock)
#define POLL_UNLOCK	unlockmtx(&lt->polllock)
#define SCHED_XLOCK	xlocksx(&lt->sxlock)
#define SCHED_UNLOCK	unlocksx(&lt->sxlock)

static void
fdtask(Task *task, void *v)
{
	int i, ms, ntasks, rc;
	Task *t;
	uvlong now;
	ltctx *lt = task->ltcontext;

	taskname(task, "fdtask");
	for(;;){
		/* let everyone else run */
		while(taskyield(task) > 0)
			;
		/* we're not blocking anything else - poll for i/o */
		errno = 0;
		taskstate(task, "poll");

		SCHED_XLOCK;
		ntasks = lt->nalltask;
		rc = lt->taskexitval;
		SCHED_UNLOCK;

		/* we're all that's left -- bail */
		if(ntasks == 1)
			taskexit(task, rc);  /* preserve prior exit code */

		taskblocking(task);
		POLL_LOCK;
		while(lt->nwaiters > 0){
			POLL_UNLOCK;
			/* give other threads a chance to register fds with us */
			POLL_LOCK;
		}

		if((t=lt->sleeping.head) == nil)
			ms = -1;
		else{
			/* sleep at most 5s */
			now = nsec();
			if(now >= t->alarmtime)
				ms = 0;
			else if(now+5*1000*1000*1000LL >= t->alarmtime)
				ms = (t->alarmtime - now)/1000000;
			else
				ms = 5000;
		}
		rc = poll(lt->pollfd, lt->npollfd, ms);
		tasknonblocking(task);

		if(rc < 0){
			if(errno == EINTR){
				POLL_UNLOCK;
				continue;
			}
			ASSERT(false, "poll: %s", strerror(errno));
		}

		/* wake up the guys who deserve it */
		for(i=0; i<lt->npollfd; i++){
			while(i < lt->npollfd && lt->pollfd[i].revents){
				if(lt->pollfd[i].fd == lt->pollwake[0]){
					/* notified -- drain pipe */
					char c;
					ssize_t r;
					r = read(lt->pollwake[0], &c, 1);
					ASSERT(r==1, "read(2): %s",
					    strerror(errno));
					i++;
					lt->nwaiters++;
				}else{
					taskready(lt->polltask[i]);
					--lt->npollfd;
					lt->pollfd[i] = lt->pollfd[lt->npollfd];
					lt->polltask[i] = lt->polltask[lt->npollfd];
				}
			}
		}

		now = nsec();
		while((t=lt->sleeping.head) && now >= t->alarmtime){
			deltask(&lt->sleeping, t);
			taskready(t);
		}

		POLL_UNLOCK;
	}
}

static void
_startfdtask(Task *t)
{
	ltctx *lt = t->ltcontext;
	int rc;

	SCHED_XLOCK;
	if(lt->startedfdtask){
		SCHED_UNLOCK;
		return;
	}

	POLL_LOCK;

	rc = pipe(lt->pollwake);
	ASSERT(rc==0, "pipe(2): %s", strerror(errno));

	rc = fdnoblock(lt->pollwake[0]);
	ASSERT(rc==0, "fcntl(2): %s", strerror(errno));
	rc = fdnoblock(lt->pollwake[1]);
	ASSERT(rc==0, "fcntl(2): %s", strerror(errno));

	ASSERT(lt->npollfd < MAXFD, "too many poll fds");
	lt->polltask[lt->npollfd] = nil;
	lt->pollfd[lt->npollfd].fd = lt->pollwake[0];
	lt->pollfd[lt->npollfd].events = POLLIN;
	lt->pollfd[lt->npollfd].revents = 0;
	lt->npollfd++;

	POLL_UNLOCK;

	lt->startedfdtask = 1;

	SCHED_UNLOCK;

	taskcreate(t, fdtask, 0);
}

uint
taskdelay(Task *task, uint ms)
{
	uvlong when, now;
	ssize_t rc;
	Task *t;
	ltctx *lt = task->ltcontext;

	_startfdtask(task);

	/* try grabbing the lock first */
	if(TRY_POLL_LOCK)
		goto locked;

	/* failing that, wake the sleeping fdtask by making an fd go active */
	rc = write(lt->pollwake[1], "w", 1);
	ASSERT(rc==1, "write(2): %s", strerror(errno));

	POLL_LOCK;
	lt->nwaiters--;
locked:

	now = nsec();
	when = now+(uvlong)ms*1000000;
	for(t=lt->sleeping.head; t!=nil && t->alarmtime < when; t=t->next)
		;

	if(t){
		task->prev = t->prev;
		task->next = t;
	}else{
		task->prev = lt->sleeping.tail;
		task->next = nil;
	}

	t = task;
	t->alarmtime = when;
	if(t->prev)
		t->prev->next = t;
	else
		lt->sleeping.head = t;
	if(t->next)
		t->next->prev = t;
	else
		lt->sleeping.tail = t;
	POLL_UNLOCK;

	taskswitch(task);

	return (nsec() - now)/1000000;
}

void
fdwait(Task *task, int fd, char rw)
{
	int bits;
	ssize_t rc;
	ltctx *lt = task->ltcontext;

	SCHED_XLOCK;
	if(!lt->startedfdtask)
		_startfdtask(task);
	SCHED_UNLOCK;

	/* try grabbing the lock first */
	if(TRY_POLL_LOCK)
		goto locked;

	/* failing that, wake the sleeping fdtask by making an fd go active */
	rc = write(lt->pollwake[1], "w", 1);
	ASSERT(rc==1, "write(2): %s", strerror(errno));

	POLL_LOCK;
	lt->nwaiters--;
locked:
	ASSERT(lt->npollfd < MAXFD, "too many poll file descriptors");

	taskstate(task, "fdwait for %s", rw=='r' ? "read" : rw=='w' ? "write" : "error");
	bits = 0;
	switch(rw){
	case 'r':
		bits |= POLLIN;
		break;
	case 'w':
		bits |= POLLOUT;
		break;
	}

	lt->polltask[lt->npollfd] = task;
	lt->pollfd[lt->npollfd].fd = fd;
	lt->pollfd[lt->npollfd].events = bits;
	lt->pollfd[lt->npollfd].revents = 0;
	lt->npollfd++;

	POLL_UNLOCK;

	taskswitch(task);
}

/* Like fdread but always calls fdwait before reading. */
ssize_t
fdread1(Task *task, int fd, void *buf, int n)
{
	ssize_t m;

	do
		fdwait(task, fd, 'r');
	while((m = read(fd, buf, n)) < 0 && errno == EAGAIN);
	return m;
}

ssize_t
fdread(Task *task, int fd, void *buf, int n)
{
	ssize_t m;

	while((m=read(fd, buf, n)) < 0 && errno == EAGAIN)
		fdwait(task, fd, 'r');
	return m;
}

ssize_t
fdwrite(Task *task, int fd, void *buf, int n)
{
	ssize_t m, tot;

	for(tot=0; tot<n; tot+=m){
		while((m=write(fd, (char*)buf+tot, n-tot)) < 0 && errno == EAGAIN)
			fdwait(task, fd, 'w');
		if(m < 0)
			return m;
		if(m == 0)
			break;
	}
	return tot;
}

int
fdnoblock(int fd)
{
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL)|O_NONBLOCK);
}

uvlong
nsec()
{
	int rc;
	struct timespec ts;

#ifdef CLOCK_REALTIME_FAST
	rc = clock_gettime(CLOCK_REALTIME_FAST, &ts);
#else
	rc = clock_gettime(CLOCK_REALTIME, &ts);
#endif
	ASSERT(rc == 0);
	return (uvlong)ts.tv_sec*1000*1000*1000 + ts.tv_nsec*1000;
}

