/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/cpuset.h>: a fixed-size CPU bitmap with the
 * CPU_* macros LinuxKPI uses (cpumask_t). Haiku's SMP_MAX_CPUS is 64 on
 * x86_64; we size for 256 to match FreeBSD's default MAXCPU headroom.
 * CPU_FOREACH iterates the CPUs Haiku reports (smp_get_num_cpus()).
 */
#ifndef _LKPI_HAIKU_SYS_CPUSET_H_
#define _LKPI_HAIKU_SYS_CPUSET_H_

#include <sys/types.h>
#include <sys/smp.h>

#define CPU_SETSIZE	256
#define _LKPI_CPU_WORDS	(CPU_SETSIZE / (8 * sizeof(unsigned long)))

typedef struct _cpuset {
	unsigned long	__bits[_LKPI_CPU_WORDS];
} cpuset_t;

#define __lkpi_cpu_word(n)	((n) / (8 * sizeof(unsigned long)))
#define __lkpi_cpu_bit(n)	(1UL << ((n) % (8 * sizeof(unsigned long))))

#define CPU_ZERO(p)	memset((p), 0, sizeof(cpuset_t))
#define CPU_FILL(p)	memset((p), 0xff, sizeof(cpuset_t))
#define CPU_SET(n, p)	((p)->__bits[__lkpi_cpu_word(n)] |= __lkpi_cpu_bit(n))
#define CPU_CLR(n, p)	((p)->__bits[__lkpi_cpu_word(n)] &= ~__lkpi_cpu_bit(n))
#define CPU_ISSET(n, p)	(((p)->__bits[__lkpi_cpu_word(n)] & __lkpi_cpu_bit(n)) != 0)
#define CPU_COPY(f, t)	(*(t) = *(f))
#define CPU_SETOF(n, p)	do { CPU_ZERO(p); CPU_SET(n, p); } while (0)
#define CPU_COUNT(p)	({ int __c = 0; unsigned __i;			\
	for (__i = 0; __i < _LKPI_CPU_WORDS; __i++)				\
		__c += __builtin_popcountl((p)->__bits[__i]); __c; })
#define CPU_EMPTY(p)	(CPU_COUNT(p) == 0)
#define CPU_OR(d, a, b)	do { unsigned __i; for (__i = 0; __i < _LKPI_CPU_WORDS; __i++) \
	(d)->__bits[__i] = (a)->__bits[__i] | (b)->__bits[__i]; } while (0)
#define CPU_AND(d, a, b) do { unsigned __i; for (__i = 0; __i < _LKPI_CPU_WORDS; __i++) \
	(d)->__bits[__i] = (a)->__bits[__i] & (b)->__bits[__i]; } while (0)
#define CPU_ANDNOT(d, a, b) do { unsigned __i; for (__i = 0; __i < _LKPI_CPU_WORDS; __i++) \
	(d)->__bits[__i] = (a)->__bits[__i] & ~(b)->__bits[__i]; } while (0)
#define CPU_FFS(p)	({ int __r = 0; unsigned __i;				\
	for (__i = 0; __i < _LKPI_CPU_WORDS; __i++) if ((p)->__bits[__i]) {	\
		__r = __i * 8 * sizeof(unsigned long) + __builtin_ffsl((p)->__bits[__i]); \
		break; } __r; })

#define CPU_ABSENT(n)	((n) >= (int)smp_get_num_cpus())
#define CPU_FOREACH(i)	for ((i) = 0; (i) < (int)smp_get_num_cpus(); (i)++)

#define CPU_LEVEL_ROOT		1
#define CPU_LEVEL_CPUSET	2
#define CPU_LEVEL_WHICH		3
#define CPU_WHICH_TID		1
#define CPU_WHICH_PID		2
#define CPU_WHICH_CPUSET	3
#define CPU_WHICH_IRQ		4

extern cpuset_t	all_cpus;
extern cpuset_t	*cpuset_root;

#ifdef __cplusplus
extern "C" {
#endif
int32	smp_get_num_cpus(void);
int	intr_setaffinity(int irq, int mode, const void *mask);
#ifdef __cplusplus
}
#endif

#endif
