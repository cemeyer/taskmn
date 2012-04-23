/* Copyright (c) 2005-2006 Russ Cox, MIT; see COPYRIGHT */

#include <sys/cdefs.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "taskmn.h"

#define ASSERT(expr, fmt...) \
	do{ \
		if(!(expr)){ \
			fprintf(stderr, "" fmt); \
			abort(); \
		} \
	}while(0)

#define nil ((void*)0)
#define nelem(x) (sizeof(x)/sizeof((x)[0]))

#define ulong task_ulong
#define uint task_uint
#define uchar task_uchar
#define uvlong task_uvlong
#define vlong task_vlong

/* More FreeBSD borrowing (sys/cdefs) */
#ifndef __aligned
# define	__aligned(x)	__attribute__((__aligned__(x)))
#endif

/*
 * XXXCEM: We inherited these things from libtask. Use the stdint types that
 * make the most sense.
 */
typedef uintptr_t ulong;  /* only used in task.c to store a pointer */
typedef uint32_t uint;    /* expected to be word (32-bit) sized */
typedef uint8_t uchar;
typedef uint64_t uvlong;
typedef int64_t vlong;

#if defined(__i386__)
# include "386-ucontext.h"
#elif defined(__x86_64__)
# include "amd64-ucontext.h"
#endif

typedef struct Context Context;

struct Context
{
	ucontext_t	uc;
};

typedef struct Libtaskcontext Libtaskcontext;

struct Task
{
	char	name[256];
	char	state[256];
	/* these pointers are in general locked by whatever mechanism
	 * serializes access to the queue that contains them. they are in one
	 * queue at a time. */
	Task	*next;
	Task	*prev;
	/* end locked */
	Context	context;
	uvlong	alarmtime;
	uint	id;
	uchar	*stk;
	uint	stksize;
	int	exiting;
	int	readyout;
	int	alltaskslot;  /* protected by ltctx->sxlock */
	int	ready;
	void	(*startfn)(Task *, void*);
	void	*startarg;
	void	*udata;  /* pointer to per-task global data */
	Libtaskcontext *ltcontext;
	Context *schedctx;
	int	blocked;
};

void	taskready(Task*);
void	taskswitch(Task *);

void	addtask(Tasklist*, Task*);
void	deltask(Tasklist*, Task*);

enum
{
	MAXFD = 1024
};

struct Libtaskcontext
{
	/* protected by polllock */
	pthread_mutex_t polllock;
	struct pollfd pollfd[MAXFD];
	Task *polltask[MAXFD];
	Tasklist sleeping;
	int npollfd;
	int nwaiters;
	int pollwake[2];
	/* end polllock */

	/* scheduling stuff; protected by sxlock */
	pthread_rwlock_t sxlock __aligned(64);
	int tasknswitch;
	int taskexitval;  /* last task to exit */
	Task **alltask;
	int nalltask;
	int taskidgen;

	int startedfdtask;  /* needs to be not blocked by big polllock */

	void (*taskmain)(Task *, void *);
	void *taskmainarg;
	/* end sxlock locked */

	int log;  /* set once at initialization */

	/* ready queue; protected by runqueuelock */
	pthread_mutex_t runqueuelock __aligned(64);
	pthread_cond_t workavail;
	Tasklist taskrunqueue;
	int nstalled;
	/* end locked */

	/* threadpool management; protected by blockedth.l */
	struct Rendez blockedth __aligned(64);
	int curthr;
	int nthr;
	int nblocking;
#define LT_BLOCKED_THRESH 75/* percent */
	/* end locked */
};

static inline void
slocksx(pthread_rwlock_t *l)
{
	int r;
	r = pthread_rwlock_rdlock(l);
	ASSERT(r==0, "%s: %s", __func__, strerror(r));
}

static inline void
xlocksx(pthread_rwlock_t *l)
{
	int r;
	r = pthread_rwlock_wrlock(l);
	ASSERT(r==0, "%s: %s", __func__, strerror(r));
}

static inline void
unlocksx(pthread_rwlock_t *l)
{
	int r;
	r = pthread_rwlock_unlock(l);
	ASSERT(r==0, "%s: %s", __func__, strerror(r));
}

static inline void
lockmtx(pthread_mutex_t *l)
{
	int r;
	r = pthread_mutex_lock(l);
	ASSERT(r==0, "%s: %s", __func__, strerror(r));
}

static inline bool
trymtx(pthread_mutex_t *l)
{
	int r;
	r = pthread_mutex_trylock(l);
	if(r == EBUSY)
		return false;
	ASSERT(r==0, "%s: %s", __func__, strerror(r));
	return true;
}

static inline void
unlockmtx(pthread_mutex_t *l)
{
	int r;
	r = pthread_mutex_unlock(l);
	ASSERT(r==0, "%s: %s", __func__, strerror(r));
}

static inline void
condwaittime(pthread_cond_t *c, pthread_mutex_t *l, unsigned ms)
{
	int r;
	struct timespec ts;
#ifdef CLOCK_REALTIME_FAST
	r = clock_gettime(CLOCK_REALTIME_FAST, &ts);
#else
	r = clock_gettime(CLOCK_REALTIME, &ts);
#endif
	ASSERT(r==0, "%s: clock: %s", __func__, strerror(errno));
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (uint64_t)(ms % 1000) * 1000*1000;
	if(ts.tv_nsec > 1000*1000*1000){
		ts.tv_sec += ts.tv_nsec/(1000*1000*1000);
		ts.tv_nsec = ts.tv_nsec%(1000*1000*1000);
	}
	r = pthread_cond_timedwait(c, l, &ts);
	ASSERT(r==0 || r==ETIMEDOUT, "%s: %s", __func__, strerror(r));
}

static inline void
condnotify(pthread_cond_t *c)
{
	int r;
	r = pthread_cond_signal(c);
	ASSERT(r==0, "%s: %s", __func__, strerror(r));
}
