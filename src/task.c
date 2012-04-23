/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

#include "taskimpl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static void		contextswitch(Context *from, Context *to);
static __inline int	imin(int a, int b) { return (a < b ? a : b); }
static void		spawn(int left, ltctx *);
#define LOG(args...) \
    	do{ \
		if(lt->log){ \
			syslog(LOG_DEBUG, args); \
		} \
	}while(0)
#define POOL_LOCK	lockmtx(&lt->blockedth.l)
#define POOL_UNLOCK	unlockmtx(&lt->blockedth.l)
#define RUNQ_LOCK	lockmtx(&lt->runqueuelock)
#define RUNQ_UNLOCK	unlockmtx(&lt->runqueuelock)
#define SCHED_XLOCK	xlocksx(&lt->sxlock)
#define SCHED_SLOCK	slocksx(&lt->sxlock)
#define SCHED_UNLOCK	unlocksx(&lt->sxlock)
#define RUN_STALLED	condwaittime(&lt->workavail, &lt->runqueuelock, 2000/*ms*/)
#define RUN_AVAIL	condnotify(&lt->workavail)

static void __printflike(3, 4)
taskdebug(ltctx *lt, Task *task, char *fmt, ...)
{
	va_list arg;
	char buf[256];

	va_start(arg, fmt);
	vsnprintf(buf, nelem(buf)-1, fmt, arg);
	buf[nelem(buf)-1] = '\0';
	va_end(arg);

	if(task)
		LOG("=lt= %d.%d: %s", getpid(), task->id, buf);
	else
		LOG("=lt= %d._: %s", getpid(), buf);
}

static void
taskstart(uint y, uint x)
{
	Task *t;
	ulong z;

	z = x<<16;	/* hide undefined 32-bit shift from 32-bit compilers */
	z <<= 16;
	z |= y;
	t = (Task*)z;

	t->startfn(t, t->startarg);
	taskexit(t, 0);
}

static Task*
taskalloc(Task *task, void (*fn)(Task *, void*), void *arg)
{
	void *stack;
	Task *t;
	sigset_t zero;
	uint x, y;
	ulong z;
	int rc;
	ltctx *lt = task->ltcontext;
	const int SZ = 128*1024;

	/* allocate the task and stack together */
	stack = malloc(SZ/*stack*/);
	ASSERT(stack, "oom");

	/* put task struct at the end of the stack */
	t = (Task*)((char*)stack + SZ - sizeof(*t));
	t = (Task*)((char*)t - ((uintptr_t)t % 64));

	memset(t, 0, sizeof *t);
	t->stk = stack;
	t->stksize = (char*)t - (char*)stack;
	SCHED_XLOCK;
	t->id = ++lt->taskidgen;
	SCHED_UNLOCK;
	t->startfn = fn;
	t->startarg = arg;
	t->ltcontext = lt;

	/* do a reasonable initialization */
	memset(&t->context.uc, 0, sizeof t->context.uc);
	sigemptyset(&zero);
	sigprocmask(SIG_BLOCK, &zero, &t->context.uc.uc_sigmask);

	/* must initialize with current context */
	rc = getcontext(&t->context.uc);
	ASSERT(rc >= 0, "getcontext: %s", strerror(errno));

	/* call makecontext to do the real work. */
	t->context.uc.uc_stack.ss_sp = (char *)t->stk;
	t->context.uc.uc_stack.ss_size = t->stksize;

	/*
	 * All this magic is because you have to pass makecontext a
	 * function that takes some number of word-sized variables,
	 * and on 64-bit machines pointers are bigger than words.
	 */
	z = (ulong)t;
	y = z;
	z >>= 16;	/* hide undefined 32-bit shift from 32-bit compilers */
	x = z>>16;
	makecontext(&t->context.uc, (void (*)(void))taskstart, 2, y, x);

	return t;
}

int
taskcreate(Task *task, void (*f)(Task *, void*), void *arg)
{
	int id;
	Task *t;
	ltctx *lt = task->ltcontext;

	t = taskalloc(task, f, arg);
	id = t->id;

	SCHED_XLOCK;

	if(lt->nalltask%64 == 0){
		lt->alltask = realloc(lt->alltask,
		    (lt->nalltask+64)*sizeof(lt->alltask[0]));
		ASSERT(lt->alltask, "OOM");
	}
	t->alltaskslot = lt->nalltask;
	lt->alltask[lt->nalltask++] = t;

	SCHED_UNLOCK;

	taskready(t);
	return id;
}

void
taskswitch(Task *t)
{
	contextswitch(&t->context, t->schedctx);
}

void
taskready(Task *t)
{
	ltctx *lt = t->ltcontext;

	t->ready = 1;

	RUNQ_LOCK;
	addtask(&lt->taskrunqueue, t);
	RUN_AVAIL;
	RUNQ_UNLOCK;
}

int
taskyield(Task *t)
{
	int n;
	ltctx *lt = t->ltcontext;

	SCHED_SLOCK;
	n = lt->tasknswitch;
	SCHED_UNLOCK;

	t->readyout = 1;
	taskstate(t, "yield");
	taskswitch(t);

	SCHED_SLOCK;
	n = lt->tasknswitch - n - 1;
	SCHED_UNLOCK;

	return n;
}

void
taskexit(Task *t, int val)
{
	ltctx *lt = t->ltcontext;

	SCHED_XLOCK;
	lt->taskexitval = val;
	SCHED_UNLOCK;

	t->exiting = 1;
	taskswitch(t);
}

static void
contextswitch(Context *from, Context *to)
{
	int rc;
	rc = swapcontext(&from->uc, &to->uc);
	ASSERT(rc >= 0, "swapcontext failed: %s", strerror(errno));
}

static void
taskscheduler(ltctx *lt)
{
	int i, suicide, nspawn, curthr;
	Task *t;
	Context schedctx;

	taskdebug(lt, nil, "scheduler enter");
	for(;;){
		SCHED_XLOCK;

		if(lt->nalltask == 0){
			taskdebug(lt, nil, "no more tasks, bailing");
			SCHED_UNLOCK;
			return;
		}

		SCHED_UNLOCK;

		RUNQ_LOCK;

		while(true){
			t = lt->taskrunqueue.head;
			if(t)
				break;

			lt->nstalled++;

			POOL_LOCK;
			curthr = lt->curthr;
			if(curthr != lt->nthr){
				POOL_UNLOCK;
				lt->nstalled--;
				RUNQ_UNLOCK;
				goto adjthreads;
			}
			POOL_UNLOCK;

			if(lt->nstalled == curthr){
				/* all other threads must be stalled as well,
				   so no need for locks */
				ASSERT(false, "No tasks (of %d) are runnable!",
				    lt->nalltask);
			}

			RUN_STALLED;
			lt->nstalled--;
		}

		deltask(&lt->taskrunqueue, t);

		RUNQ_UNLOCK;

		SCHED_XLOCK;

		t->ready = 0;
		t->readyout = 0;
		lt->tasknswitch++;

		SCHED_UNLOCK;

		taskdebug(lt, t, "run %d (%s)", t->id, t->name);

		t->schedctx = &schedctx;
		contextswitch(&schedctx, &t->context);
#if 0
print("back in scheduler\n");
#endif
		t->schedctx = NULL;

		/* if the task is exiting, it won't be on the run queue for
		 * another thread to pick up anyway */
		if(t->exiting){
			SCHED_XLOCK;

			i = t->alltaskslot;
			lt->alltask[i] = lt->alltask[--lt->nalltask];
			lt->alltask[i]->alltaskslot = i;
			free(t->stk);

			SCHED_UNLOCK;
		}else if(t->readyout){
			taskready(t);
		}

adjthreads:
		/* adjust to user-set threadpool size */
		suicide = 0;
		nspawn = 0;
		POOL_LOCK;
		if(lt->curthr > lt->nthr){
			lt->curthr--;
			suicide = 1;
		}else if(lt->curthr < lt->nthr){
			nspawn = lt->nthr - lt->curthr;
			lt->curthr = lt->nthr;
		}
		POOL_UNLOCK;

		if(suicide)
			return;
		if(nspawn)
			spawn(nspawn-1, lt);
	}
}

void**
taskdata(Task *t)
{
	return &t->udata;
}

/*
 * debugging
 */
void
taskname(Task *t, char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vsnprintf(t->name, nelem(t->name)-1, fmt, arg);
	t->name[nelem(t->name)-1] = '\0';
	va_end(arg);
}

char*
taskgetname(Task *t)
{
	return t->name;
}

void
taskstate(Task *t, char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vsnprintf(t->state, nelem(t->state)-1, fmt, arg);
	t->state[nelem(t->state)-1] = '\0';
	va_end(arg);
}

char*
taskgetstate(Task *t)
{
	return t->state;
}

/*
 * startup
 */

static void
taskmainstart(Task *t, void *v)
{
	taskname(t, "taskmain");
	t->ltcontext->taskmain(t, t->ltcontext->taskmainarg);
}

struct workerarg
{
	ltctx*	lt;
	int	nleft;
};

static void *
workerthr(void *arg)
{
	struct workerarg *wa;
	ltctx *lt;
	int childleft[2] = { 0, 0}, left, thisleft;

	wa = (struct workerarg *)arg;
	lt = wa->lt;
	left = wa->nleft;
	free(arg);

	/* the following 10 lines try to bring up threadpool in parallel */
	thisleft = imin(left, 2);
	left -= thisleft;
	if(left > 0){
		childleft[0] = (left+1)/2;
		childleft[1] = left - childleft[0];
	}
	if(thisleft > 0)
		spawn(childleft[0], lt);
	if(thisleft > 1)
		spawn(childleft[1], lt);

	taskscheduler(lt);
	/* no more tasks want to run */

	return nil;
}

/*
 * Spawns left+1 threads. ex, to spawn 4 threads:
 *
 * spawn(3, lt);
 */
static void
spawn(int left, ltctx *lt)
{
	struct workerarg *wa;
	int r;
	pthread_t pt;

	wa = malloc(sizeof *wa);
	ASSERT(wa, "oom");
	wa->nleft = left;
	wa->lt = lt;

	r = pthread_create(&pt, NULL, workerthr, (void*)wa);
	ASSERT(r==0, "pthread_create: %s", strerror(errno));
}

int
libtaskmn(void (*f)(Task *lt, void *arg), void *arg, int nthr)
{
	ltctx *ltcontext;
	Task faketask;
	struct workerarg *wa;
	int rc;

	ltcontext = malloc(sizeof *ltcontext);
	ASSERT(ltcontext, "OOM");
	memset(ltcontext, 0, sizeof *ltcontext);

	ltcontext->polllock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	ltcontext->sxlock = (pthread_rwlock_t)PTHREAD_RWLOCK_INITIALIZER;
	ltcontext->runqueuelock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	ltcontext->workavail = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	rendezinit(&ltcontext->blockedth);

	ltcontext->taskmain = f;
	ltcontext->taskmainarg = arg;
	if(getenv("TASKMN_SPAM"))
		ltcontext->log = 1;

	memset(&faketask, 0, sizeof faketask);
	faketask.ltcontext = ltcontext;
	taskcreate(&faketask, taskmainstart, nil);

	ltcontext->nthr = nthr;
	ltcontext->curthr = nthr;

	while(true){
		wa = malloc(sizeof *wa);  /* freed by caller */
		ASSERT(wa, "oom");
		wa->nleft = nthr-1/*we are already a thread!*/;
		wa->lt = ltcontext;

		/* join the proletariat */
		workerthr(wa);

		/* this thread may accidentally suicide; if so, restart it
		 * until another thread falls on its sword */
		nthr = 1;
		xlocksx(&ltcontext->sxlock);
		if(ltcontext->nalltask == 0)
			nthr = 0;
		unlocksx(&ltcontext->sxlock);

		if(nthr == 0)
			break;

		/* this thread didn't really die */
		lockmtx(&ltcontext->blockedth.l);
		ltcontext->curthr++;
		unlockmtx(&ltcontext->blockedth.l);
	}

	if(ltcontext->alltask)
		free(ltcontext->alltask);
	rc = ltcontext->taskexitval;
	free(ltcontext);
	return rc;
}

/*
 * hooray for linked lists
 */
void
addtask(Tasklist *l, Task *t)
{
	if(l->tail){
		l->tail->next = t;
		t->prev = l->tail;
	}else{
		l->head = t;
		t->prev = nil;
	}
	l->tail = t;
	t->next = nil;
}

void
deltask(Tasklist *l, Task *t)
{
	if(t->prev)
		t->prev->next = t->next;
	else
		l->head = t->next;
	if(t->next)
		t->next->prev = t->prev;
	else
		l->tail = t->prev;
}

unsigned int
taskid(Task *t)
{
	return t->id;
}

void
taskblocking(Task *task)
{
	ltctx *lt = task->ltcontext;

	ASSERT(!task->blocked, "double-blocked task!");
	task->blocked = 1;

	POOL_LOCK;

	while(true){
		if((lt->nblocking+1)*100/lt->curthr <= LT_BLOCKED_THRESH){
			/* there aren't too many blocking threads; go for it. */
			lt->nblocking++;
			goto out;
		}

		tasksleep(task, &lt->blockedth);
	}

out:
	POOL_UNLOCK;
}

void
tasknonblocking(Task *task)
{
	ltctx *lt = task->ltcontext;

	ASSERT(task->blocked, "double-unblocked task!");
	task->blocked = 0;

	POOL_LOCK;

	lt->nblocking--;
	taskwakeup(&lt->blockedth);

	POOL_UNLOCK;
}

void
taskpoolsize(Task *task, int nthr)
{
	ltctx *lt = task->ltcontext;

	POOL_LOCK;
	lt->nthr = nthr;
	POOL_UNLOCK;
}
