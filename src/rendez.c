#include "taskimpl.h"

void rendezinit(Rendez *r)
{
	r->l = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}

/*
 * sleep and wakeup
 */
void
tasksleep(Task *t, Rendez *r)
{
	addtask(&r->waiting, t);
	unlockmtx(&r->l);

	taskstate(t, "sleep");
	taskswitch(t);

	lockmtx(&r->l);
}

static int
_taskwakeup(Rendez *r, int all)
{
	int i;
	Task *t;

	for(i=0;; i++){
		if(i==1 && !all)
			break;
		if((t = r->waiting.head) == nil)
			break;
		deltask(&r->waiting, t);
		taskready(t);
	}
	return i;
}

int
taskwakeup(Rendez *r)
{
	return _taskwakeup(r, 0);
}

int
taskwakeupall(Rendez *r)
{
	return _taskwakeup(r, 1);
}

