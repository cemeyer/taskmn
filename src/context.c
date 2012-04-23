/* Copyright (c) 2005-2006 Russ Cox, MIT; see COPYRIGHT */

#include "taskimpl.h"

#if defined(__i386__)
# define NEEDX86MAKECONTEXT
# define NEEDSWAPCONTEXT
#elif defined(__x86_64__)
# define NEEDAMD64MAKECONTEXT
# define NEEDSWAPCONTEXT
#else
# error "Non-x86 not supported"
#endif

#ifdef NEEDX86MAKECONTEXT
void
makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...)
{
	int *sp;

	sp = (int*)ucp->uc_stack.ss_sp+ucp->uc_stack.ss_size/4;
	sp -= argc;
	sp = (void*)((uintptr_t)sp - (uintptr_t)sp%16);	/* 16-align for OS X */
	memmove(sp, &argc+1, argc*sizeof(int));

	*--sp = 0;			/* null return address */
	ucp->uc_mcontext.mc_ebp = 0;	/* null frame pointer for gdb6 */
	ucp->uc_mcontext.mc_eip = (long)func;
	ucp->uc_mcontext.mc_esp = (int)sp;
}
#endif

#ifdef NEEDAMD64MAKECONTEXT
void
makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...)
{
	long *sp;
	va_list va;

	memset(&ucp->uc_mcontext, 0, sizeof ucp->uc_mcontext);
	if(argc != 2)
		*(int*)0 = 0;
	va_start(va, argc);
	ucp->uc_mcontext.mc_rdi = va_arg(va, int);
	ucp->uc_mcontext.mc_rsi = va_arg(va, int);
	va_end(va);
	sp = (long*)ucp->uc_stack.ss_sp+ucp->uc_stack.ss_size/sizeof(long);
	sp -= argc;
	sp = (void*)((uintptr_t)sp - (uintptr_t)sp%16);	/* 16-align for OS X */
	*--sp = 0;			/* null return address */
	ucp->uc_mcontext.mc_rbp = 0;	/* null frame pointer for gdb6 */
	ucp->uc_mcontext.mc_rip = (long)func;
	ucp->uc_mcontext.mc_rsp = (long)sp;
}
#endif

#ifdef NEEDSWAPCONTEXT
int
swapcontext(ucontext_t *oucp, const ucontext_t *ucp)
{
	if(getcontext(oucp) == 0)
		setcontext(ucp);
	return 0;
}
#endif

