/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/proc.h>.
 *
 * Haiku's freebsd_network deliberately has no 'struct thread' and defines
 * curthread as NULL (compat/sys/pcpu.h). LinuxKPI needs one: Linux 'current'
 * (struct task_struct) hangs off curthread->td_lkpi_task. We provide a small
 * per-Haiku-thread record, looked up by thread_id (lkpi_thread.c). Only the
 * fields LinuxKPI touches exist.
 */
#ifndef _LKPI_HAIKU_SYS_PROC_H_
#define _LKPI_HAIKU_SYS_PROC_H_

#include_next <sys/proc.h>
#include <sys/pcpu.h>
#include <OS.h>
#include <signal.h>

struct task_struct;
struct ucred;

struct proc {
	pid_t		p_pid;
	char		p_comm[32];
	int		p_flag;
	sigset_t	p_siglist;	/* always empty, see sys/signalvar.h */
};
#define P_WEXIT		0x02000	/* FreeBSD value; never set on Haiku */
#define P_SYSTEM	0x00200

struct thread {
	thread_id		td_tid;
	struct proc		*td_proc;
	struct task_struct	*td_lkpi_task;
	struct ucred		*td_ucred;
	int			td_pflags;
	int			td_critnest;
	int			td_intr_nesting_level;
	int			td_owepreempt;
	int			td_ast_pending;
	int			td_inhibitors;	/* always 0 on Haiku */
	unsigned char		td_priority;
	sigset_t		td_siglist;	/* always empty */
	sigset_t		td_sigmask;
	char			td_name[B_OS_NAME_LENGTH];
};

#define TDP_NOFAULTING	0x00000080	/* value as in FreeBSD; private to LinuxKPI */
#define TDP_ITHREAD	0x00400000	/* never set: Haiku interrupt handlers of
					   freebsd_network run in normal threads */
#define PID_MAX		99999

struct sx;

#ifdef __cplusplus
extern "C" {
#endif

struct thread	*lkpi_curthread(void);
/* FreeBSD kernel hooks LinuxKPI installs (linux_current.c sets them). */
extern int	(*lkpi_alloc_current)(struct thread *, int);
void		tdsignal(struct thread *td, int sig);	/* lkpi_thread.cpp */
int		linux_alloc_current_noop(struct thread *, int);	/* lkpi_misc.c */

/*
 * Process model: on Haiku every thread LinuxKPI sees is a kernel thread,
 * so there is exactly one struct proc (lkpi_proc0()) and the "threads in
 * the process" are the struct thread records lkpi_thread.cpp has created.
 */
struct proc	*lkpi_proc0(void);
struct thread	*lkpi_thread_first(struct proc *p);
struct thread	*lkpi_thread_next(struct thread *td);
struct thread	*tdfind(int32_t tid, pid_t pid);	/* "locked" proc: no-op lock */
struct proc	*pfind(pid_t pid);			/* always NULL */
void		thread_reap_barrier(void);
extern struct sx allproc_lock;
bool		lkpi_can_sleep(void);	/* lkpi_thread.cpp: interrupts on */

#ifdef __cplusplus
}
#endif

#undef curthread
#define curthread	lkpi_curthread()

#define FOREACH_PROC_IN_SYSTEM(p)					\
	for ((p) = lkpi_proc0(); (p) != NULL; (p) = NULL)
#define FOREACH_THREAD_IN_PROC(p, td)					\
	for ((td) = lkpi_thread_first(p); (td) != NULL;			\
	    (td) = lkpi_thread_next(td))
#define THREAD_CAN_SLEEP()	(lkpi_can_sleep())

#define PROC_LOCK(p)		do { } while (0)
#define PROC_UNLOCK(p)		do { } while (0)
#define PROC_LOCK_ASSERT(p, w)	do { } while (0)

#endif
