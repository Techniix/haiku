/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * FreeBSD kernel functions that LinuxKPI calls but Haiku's freebsd_network
 * does not provide (docs/RESEARCH.md §3.2). Each is implemented in
 * haiku/lkpi_*.c. Keep this list in sync with docs/COVERAGE.md.
 */
#ifndef _LKPI_HAIKU_KPI_H_
#define _LKPI_HAIKU_KPI_H_

#include <sys/types.h>

struct thread;

#ifdef __cplusplus
extern "C" {
#endif

/* user memory (lkpi_uaccess.c): Haiku user_memcpy/user_strlcpy underneath */
int	copyinstr(const void *uaddr, void *kaddr, size_t len, size_t *done);
int	copyin_nofault(const void *uaddr, void *kaddr, size_t len);
int	copyout_nofault(const void *kaddr, void *uaddr, size_t len);
int	vm_fault_disable_pagefaults(void);
void	vm_fault_enable_pagefaults(int save);

/* CPU features (lkpi_cpu.c) */
extern unsigned int cpu_feature;
extern unsigned int cpu_feature2;
extern unsigned int cpu_stdext_feature;

/* memory (lkpi_page.c) */
extern long	physmem;		/* pages */

/* scheduling (lkpi_thread.c) */
void	sched_relinquish(struct thread *td);
void	kern_yield(int prio);
int	td_ast_pending(struct thread *td, int ast);
#define TDA_SCHED	1
#define PRI_UNCHANGED	-1

#ifdef __cplusplus
}
#endif

#endif
