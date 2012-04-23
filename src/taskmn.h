/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

#ifndef _TASK_H_
#define _TASK_H_ 1

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/cdefs.h>

/* Borrowed from FreeBSD: */
#ifndef __printflike
# define	__printflike(fmtarg, firstvararg) \
	__attribute__((__format__ (__printf__, fmtarg, firstvararg)))
#endif

#include <inttypes.h>
#include <stdarg.h>

typedef struct Libtaskcontext ltctx;
typedef struct Task Task;
typedef struct Tasklist Tasklist;

/*
 * libtaskmn() allocates a new libtask context and starts running f() as the
 * first task in this instance. Nthr is the initial size of the threadpool.
 * This can be changed later with taskpoolsize().
 *
 * This function only returns when all tasks have exited (see taskexit()); it
 * returns with the error code that the last task to exit returns.
 */
int		libtaskmn(void (*f)(Task *lt, void *arg), void *arg, int nthr);
void		taskpoolsize(Task *, int);

/*
 * basic procs and threads
 */

int		taskcreate(Task *, void (*f)(Task *t, void *arg), void *arg);
void**		taskdata(Task *);
unsigned int	taskdelay(Task *, unsigned int ms);
void		taskexit(Task *, int);
char*		taskgetname(Task *);
char*		taskgetstate(Task *);
unsigned	taskid(Task *);
void		taskname(Task *, char*, ...) __printflike(2, 3);
void		taskstate(Task *, char*, ...) __printflike(2, 3);
void		tasksystem(Task *);
int		taskyield(Task *);

/*
 * declare that a section of code may block; taskmn internally prevents
 * some fraction of threads from running blocking sections at any time.
 */
void		taskblocking(Task *);
void		tasknonblocking(Task *);

struct Tasklist	/* used internally */
{
	Task	*head;
	Task	*tail;
};

/*
 * sleep and wakeup (condition variables)
 */
typedef struct Rendez Rendez;

struct Rendez
{
	pthread_mutex_t	l;
	Tasklist waiting;
};

/*
 * Note: Much like pthread conds, caller must lock the rendez lock around
 * tasksleep(). Caller must also lock the rendez lock for calls
 * to taskwakeup() and taskwakeupall();
 */
void	rendezinit(Rendez *);
void	tasksleep(Task *, Rendez*);
int	taskwakeup(Rendez*);
int	taskwakeupall(Rendez*);

/*
 * Threaded I/O.
 * (Note: a trip through fdwait() is slow -- we only poll when the ready queue
 * is empty.)
 */
int		fdnoblock(int);
ssize_t		fdread(Task*, int, void*, int);
ssize_t		fdread1(Task*, int, void*, int);	/* always uses fdwait */
ssize_t		fdwrite(Task*, int, void*, int);
void		fdwait(Task*, int, char);

/*
 * Network dialing - sets non-blocking automatically
 */
enum
{
	UDP = 0,
	TCP = 1,
};

int		netannounce(Task *, int, char*, int);
int		netaccept(Task *, int);
int		netdial(Task *, int, char*, int);
int		netlookup(Task *, char*, uint32_t*);	/* blocks entire program! */

#ifdef __cplusplus
}
#endif

#endif
