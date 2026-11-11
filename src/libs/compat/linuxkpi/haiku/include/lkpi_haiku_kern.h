/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * FreeBSD kernel facilities LinuxKPI uses that Haiku's freebsd_network lacks,
 * where the implementation is small enough to be inline. Out-of-line ones are
 * declared in lkpi_haiku_kpi.h. Every item here came from the stage (a)
 * compile probe (docs/COVERAGE.md); none is speculative.
 */
#ifndef _LKPI_HAIKU_KERN_H_
#define _LKPI_HAIKU_KERN_H_

#include <sys/types.h>
#include <sys/param.h>
#include <machine/atomic.h>
#include <sys/kernel.h>	/* hz */
#include <OS.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- libkern ---------------------------------------------------------- */
#define strtouq(s, e, b)	((u_quad_t)strtoull((s), (e), (b)))
#define strtoq(s, e, b)		((quad_t)strtoll((s), (e), (b)))

static __inline char *
strchrnul(const char *s, int c)
{
	for (; *s != '\0' && *s != (char)c; s++)
		;
	return ((char *)s);
}

/* bitcount(9). freebsd_network's sys/types.h has the older __bitcount*
 * functions; newer FreeBSD (sys/bitcount.h) adds the constant-folding
 * __const_bitcount*() forms LinuxKPI's linux/bitops.h uses (same text). */
#ifndef __const_bitcount8
#define	__const_bitcount8(x) ( \
    !!((x) & (1 << 0)) + !!((x) & (1 << 1)) + \
    !!((x) & (1 << 2)) + !!((x) & (1 << 3)) + \
    !!((x) & (1 << 4)) + !!((x) & (1 << 5)) + \
    !!((x) & (1 << 6)) + !!((x) & (1 << 7)))
#define	__const_bitcount16(x) (__const_bitcount8(x) + __const_bitcount8((x) >> 8))
#define	__const_bitcount32(x) (__const_bitcount16(x) + __const_bitcount16((x) >> 16))
#define	__const_bitcount64(x) (__const_bitcount32(x) + __const_bitcount32((x) >> 32))
#endif
#define	bitcount(x)	__bitcount((u_int)(x))
#define	bitcountl(x)	__bitcountl((u_long)(x))
#define	bitcount16(x)	__bitcount16((uint16_t)(x))
#define	bitcount32(x)	__bitcount32((uint32_t)(x))
#define	bitcount64(x)	__bitcount64((uint64_t)(x))

/* ilog2 & friends: as FreeBSD sys/libkern.h (LinuxKPI's log2.h relies on
 * them; Haiku's compat libkern.h has fls/flsl only). */
static __inline int ilog2_int(int n) { return (8 * sizeof(n) - 1 - __builtin_clz((u_int)n)); }
static __inline int ilog2_long(long n) { return (8 * sizeof(n) - 1 - __builtin_clzl((u_long)n)); }
static __inline int ilog2_long_long(long long n)
{ return (8 * sizeof(n) - 1 - __builtin_clzll((unsigned long long)n)); }
#define ilog2_var(n)	_Generic((n), default: ilog2_int, long: ilog2_long,	\
	unsigned long: ilog2_long, long long: ilog2_long_long,			\
	unsigned long long: ilog2_long_long)(n)
#define ilog2_const(n)	(8 * (int)sizeof(unsigned long long) - 1 - __builtin_clzll(n))
#define ilog2(n)	(__builtin_constant_p(n) ? ilog2_const(n) : ilog2_var(n))
#define rounddown_pow_of_two(n)	((__typeof(n))1 << ilog2(n))
#define order_base_2(n)	ilog2(2*(n)-1)
#define roundup_pow_of_two(n)	((__typeof(n))1 << order_base_2(n))

static __inline u_long ulmin(u_long a, u_long b) { return (a < b ? a : b); }
static __inline u_long ulmax(u_long a, u_long b) { return (a > b ? a : b); }
static __inline u_int umin(u_int a, u_int b) { return (a < b ? a : b); }
static __inline u_int umax(u_int a, u_int b) { return (a > b ? a : b); }
static __inline quad_t qmin(quad_t a, quad_t b) { return (a < b ? a : b); }

#define WOULD_OVERFLOW(nmemb, size)	\
	((size) != 0 && (nmemb) > SIZE_MAX / (size))


/* Uniform in [0, upper), no modulo bias (rejection sampling, as libc). */
static __inline uint32_t
arc4random_uniform(uint32_t upper)
{
	uint32_t r, min;

	if (upper < 2)
		return (0);
	min = -upper % upper;
	do {
		r = arc4random();
	} while (r < min);
	return (r % upper);
}

/* Linux jiffies: FreeBSD aliases the variable to 'ticks' in assembly. On
 * Haiku it is a real variable (linux/jiffies.h declares it), kept current
 * by a 1 ms periodic kernel timer (lkpi_thread.cpp); hz is 1000. A macro
 * would break drivers that have struct members named 'jiffies' (mt76). */
unsigned long	lkpi_get_jiffies(void);

/* The Haiku kernel exports no ffs(); use the builtins (libkern ffs(9)). */
#ifndef ffs
#define ffs(x)		__builtin_ffs(x)
#endif
#ifndef ffsl
#define ffsl(x)		__builtin_ffsl(x)
#endif
#ifndef ffsll
#define ffsll(x)	__builtin_ffsll(x)
#endif

/* FreeBSD kernel qsort_r (POSIX argument order, sys/libkern.h); Haiku's
 * kernel does not export one. lkpi_misc.c. */
void	lkpi_qsort_r(void *base, size_t nmemb, size_t size,
	    int (*compar)(const void *, const void *, void *), void *thunk);
#define qsort_r		lkpi_qsort_r

/* FPU use from kernel code (linux/slab.h kvmalloc paths reach it; real
 * users are DRM drivers). lkpi_misc.c. */
/* declared by linux/compat.h: void lkpi_fpu_safe_exec(fpu_safe_exec_cb_t, void *) */

/* kern_mib.c */
#define OSRELEASELEN	32
extern char osrelease[];

/* ---- user memory stores (lkpi_misc.c, Haiku user_memcpy) -------------- */
int	subyte(volatile void *base, int byte);
int	suword16(volatile void *base, int word);
int	suword32(volatile void *base, int32_t word);
int	suword64(volatile void *base, int64_t word);
int	suword(volatile void *base, long word);
#ifndef VM_MAXUSER_ADDRESS
#define VM_MAXUSER_ADDRESS	0x0000800000000000UL	/* Haiku x86_64 USER_TOP + 1 */
#endif
#define IOCPARM_SHIFT	13
#define IOCPARM_MAX	(1 << IOCPARM_SHIFT)

/* ---- Giant: FreeBSD's DROP_GIANT/PICKUP_GIANT (sys/mutex.h) shape ------ */
#define DROP_GIANT()	do {						\
	int _giantcnt = 0;						\
	if (mtx_owned(&Giant)) {					\
		_giantcnt = 1;						\
		mtx_unlock(&Giant);					\
	}
#define PICKUP_GIANT()							\
	if (_giantcnt)							\
		mtx_lock(&Giant);					\
} while (0)

/* ---- SMP rendezvous (lkpi_misc.c, Haiku call_all_cpus_sync) ----------- */
#define smp_no_rendezvous_barrier	((void (*)(void *))0)
void	smp_rendezvous(void (*setup)(void *), void (*action)(void *),
	    void (*teardown)(void *), void *arg);

/* ---- time: sbintime_t (32.32 fixed-point seconds) ---------------------- */
typedef int64_t	sbintime_t;
#define SBT_1S	((sbintime_t)1 << 32)
#define SBT_1MS	(SBT_1S / 1000)
#define SBT_1US	(SBT_1S / 1000000)
#define SBT_1NS	(SBT_1S / 1000000000)
#define SBT_MAX	0x7fffffffffffffffLL

/* Exact scaling without 128-bit arithmetic (no __divti3 in the kernel), as
 * FreeBSD's sys/time.h: floor for sbt->unit, ceil for unit->sbt. Valid while
 * the scale factors are <= 2^32, which all callers here satisfy. */
static __inline int64_t
__lkpi_scale64(int64_t x, int64_t f, int64_t d, int ceil)
{
	int neg = x < 0;
	uint64_t ux = neg ? -(uint64_t)x : (uint64_t)x, r;

	r = (ux / d) * f + ((ux % d) * f + (ceil != neg ? d - 1 : 0)) / d;
	return (neg ? -(int64_t)r : (int64_t)r);
}
static __inline sbintime_t nstosbt(int64_t ns) { return (__lkpi_scale64(ns, SBT_1S, 1000000000, 1)); }
static __inline sbintime_t ustosbt(int64_t us) { return (__lkpi_scale64(us, SBT_1S, 1000000, 1)); }
static __inline sbintime_t mstosbt(int64_t ms) { return (__lkpi_scale64(ms, SBT_1S, 1000, 1)); }
static __inline int64_t sbttons(sbintime_t sbt) { return (__lkpi_scale64(sbt, 1000000000, SBT_1S, 0)); }
static __inline int64_t sbttous(sbintime_t sbt) { return (__lkpi_scale64(sbt, 1000000, SBT_1S, 0)); }
static __inline int64_t sbttoms(sbintime_t sbt) { return (__lkpi_scale64(sbt, 1000, SBT_1S, 0)); }
static __inline sbintime_t sbinuptime(void) { return (ustosbt(system_time())); }
static __inline sbintime_t getsbinuptime(void) { return (sbinuptime()); }
static __inline sbintime_t tick_sbt_value(void) { return (SBT_1S / hz); }
#define tick_sbt	tick_sbt_value()

#ifndef timespecsub
#define timespecsub(tsp, usp, vsp) do {				\
	(vsp)->tv_sec = (tsp)->tv_sec - (usp)->tv_sec;			\
	(vsp)->tv_nsec = (tsp)->tv_nsec - (usp)->tv_nsec;		\
	if ((vsp)->tv_nsec < 0) {					\
		(vsp)->tv_sec--;					\
		(vsp)->tv_nsec += 1000000000L;				\
	}								\
} while (0)
#define timespecadd(tsp, usp, vsp) do {				\
	(vsp)->tv_sec = (tsp)->tv_sec + (usp)->tv_sec;			\
	(vsp)->tv_nsec = (tsp)->tv_nsec + (usp)->tv_nsec;		\
	if ((vsp)->tv_nsec >= 1000000000L) {				\
		(vsp)->tv_sec++;					\
		(vsp)->tv_nsec -= 1000000000L;				\
	}								\
} while (0)
#endif

static __inline void
nanouptime(struct timespec *ts)
{
	nanotime_t ns = system_time_nsecs();
	ts->tv_sec = ns / 1000000000LL;
	ts->tv_nsec = ns % 1000000000LL;
}
#define getnanouptime(ts)	nanouptime(ts)
static __inline void
nanotime(struct timespec *ts)
{
	bigtime_t us = real_time_clock_usecs();
	ts->tv_sec = us / 1000000;
	ts->tv_nsec = (us % 1000000) * 1000;
}
#define getnanotime(ts)	nanotime(ts)

/* ---- refcount(9), header-only, without the sleep/wakeup waiter bit ---- */
static __inline void refcount_init(volatile u_int *c, u_int v)
{ atomic_store_rel_int(c, v); }
static __inline u_int refcount_load(volatile u_int *c)
{ return (atomic_load_int(c)); }
static __inline u_int refcount_acquire(volatile u_int *c)
{ return (atomic_fetchadd_int(c, 1)); }
static __inline bool refcount_acquire_if_not_zero(volatile u_int *c)
{
	u_int old = atomic_load_int(c);
	do {
		if (old == 0)
			return (false);
	} while (!atomic_fcmpset_int(c, &old, old + 1));
	return (true);
}
static __inline bool refcount_release(volatile u_int *c)
{ return (atomic_fetchadd_int(c, (u_int)-1) == 1); }
static __inline bool refcount_release_if_not_last(volatile u_int *c)
{
	u_int old = atomic_load_int(c);
	do {
		if (old == 1)
			return (false);
	} while (!atomic_fcmpset_int(c, &old, old - 1));
	return (true);
}
static __inline bool refcount_release_if_last(volatile u_int *c)
{
	u_int old = 1;
	return (atomic_fcmpset_int(c, &old, 0));
}

/* ---- per-CPU data (FreeBSD DPCPU model) --------------------------------
 * Objects declared with DPCPU_DEFINE live in the "set_lkpi_dpcpu" section;
 * that section is the CPU 0 copy, and lkpi_dpcpu_init() (lkpi_misc.c) makes
 * one copy per further CPU, recording its offset. DPCPU_ID_PTR(id, n) is
 * &n shifted by that offset, so names may be arrays, as in FreeBSD
 * (e.g. linux_rcu.c's linux_epoch_record[RCU_TYPE_MAX]). */
#ifndef LKPI_MAX_CPUS
#define LKPI_MAX_CPUS	64	/* Haiku x86_64 SMP_MAX_CPUS */
#endif
int32	smp_get_current_cpu(void);
extern uintptr_t lkpi_dpcpu_off[LKPI_MAX_CPUS];
void	lkpi_dpcpu_init(void);
#define curcpu			((int)smp_get_current_cpu())
#define DPCPU_DEFINE(t, n)						\
	t n __attribute__((__section__("set_lkpi_dpcpu"), __used__))
#define DPCPU_DEFINE_STATIC(t, n)	static DPCPU_DEFINE(t, n)
#define DPCPU_DECLARE(t, n)	extern t n
#define DPCPU_ID_PTR(id, n)						\
	((__typeof(&(n)))((uintptr_t)&(n) + lkpi_dpcpu_off[(id)]))
#define DPCPU_ID_GET(id, n)	(*DPCPU_ID_PTR(id, n))
#define DPCPU_ID_SET(id, n, v)	(*DPCPU_ID_PTR(id, n) = (v))
#define DPCPU_PTR(n)		DPCPU_ID_PTR(curcpu, n)
#define DPCPU_GET(n)		(*DPCPU_PTR(n))
#define DPCPU_SET(n, v)		(*DPCPU_PTR(n) = (v))
#define PCPU_GET(field)		__lkpi_pcpu_##field
#define __lkpi_pcpu_cpuid	curcpu

/* sched_pin(): pin to the current CPU (lkpi_thread.cpp, Haiku
 * thread_pin_to_current_cpu()). */
void	sched_pin(void);
void	sched_unpin(void);

/* ---- callout(9) sbintime variants, on Haiku's tick-based callouts ---- */
#define C_DIRECT_EXEC	0x0001
#define C_PRELBIN(x)	(((x) + 1) << 1)
#define C_HARDCLOCK	0x0100
#define C_ABSOLUTE	0x0200
#define C_PRECALC	0x0400
#define C_CATCH		0x0800

static __inline int
__lkpi_sbt_to_ticks(sbintime_t sbt)
{
	int64_t t;

	if (sbt <= 0)
		return (1);
	t = __lkpi_scale64(sbt, hz, SBT_1S, 1);
	return (t < 1 ? 1 : (t > 0x7fffffff ? 0x7fffffff : (int)t));
}
#define callout_reset_sbt(c, sbt, pr, fn, arg, flags)			\
	callout_reset((c), __lkpi_sbt_to_ticks(((flags) & C_ABSOLUTE) ?	\
	    (sbt) - sbinuptime() : (sbt)), (fn), (arg))
#define callout_schedule_sbt(c, sbt, pr, flags)				\
	callout_schedule((c), __lkpi_sbt_to_ticks(((flags) & C_ABSOLUTE) ?	\
	    (sbt) - sbinuptime() : (sbt)))
/* Haiku's callout_active() means "running now"; there is no separate
 * active flag to clear. */
#define callout_deactivate(c)	do { } while (0)

static __inline int
pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags)
{
	snooze(sbt > 0 ? sbttous(sbt) : 1);
	return (0);
}

/* ---- witness / sleep annotations: no-ops on Haiku ---------------------- */
#define WARN_GIANTOK		0x01
#define WARN_PANIC		0x02
#define WARN_SLEEPOK		0x04
#define WITNESS_WARN(flags, lock, fmt, ...)	(0)
#define THREAD_NO_SLEEPING()	do { } while (0)
#define THREAD_SLEEPING_OK()	do { } while (0)

/* ---- taskqueue extensions (patches/haiku/0002 adds the real ones) ---- */
#define TASK_INITIALIZER(priority, func, context)			\
	{ .ta_priority = (priority), .ta_func = (func),			\
	  .ta_context = (context) }

/* ---- malloc(9) extras --------------------------------------------------- */
size_t	malloc_usable_size(const void *p);	/* lkpi_misc.c */
#define malloc_size(sz)			(sz)
#define zfree(p, type)	do {						\
	if ((p) != NULL)						\
		explicit_bzero((p), malloc_usable_size(p));		\
	free((p), (type));						\
} while (0)
#define malloc_domainset(sz, type, ds, fl)	malloc((sz), (type), (fl))
#define contigmalloc_domainset(sz, type, ds, fl, lo, hi, al, bd)	\
	contigmalloc((sz), (type), (fl), (lo), (hi), (al), (bd))
void	explicit_bzero(void *b, size_t len);

/* ---- VM object locks: objects are not used by the page shim ------------ */
#define VM_OBJECT_WLOCK(o)		do { } while (0)
#define VM_OBJECT_WUNLOCK(o)		do { } while (0)
#define VM_OBJECT_RLOCK(o)		do { } while (0)
#define VM_OBJECT_RUNLOCK(o)		do { } while (0)
#define VM_OBJECT_ASSERT_WLOCKED(o)	do { } while (0)
#define PHYS_IN_DMAP(pa)		(0)
void	vm_wait(void *obj);
void	kmem_free(void *addr, vm_size_t size);
void	pmap_invalidate_cache(void);
void	pmap_force_invalidate_cache_range(vm_offset_t sva, vm_offset_t eva);

/* ---- device numbers (Linux dev_t helpers on FreeBSD's) ---------------- */
#ifndef major
#define major(d)		((int)(((uint64_t)(d) >> 32) & 0xffffffff))
#define minor(d)		((int)((uint64_t)(d) & 0xffffffff))
#define makedev(M, m)		((dev_t)(((uint64_t)(M) << 32) | (uint32_t)(m)))
#endif

/* libkern hexdump(9) (FreeBSD sys/systm.h); lkpi_misc.c */
void	hexdump(const void *ptr, int length, const char *hdr, int flags);
#define	HD_COLUMN_MASK	0xff
#define	HD_DELIM_MASK	0xff00
#define	HD_OMIT_COUNT	(1 << 16)
#define	HD_OMIT_HEX	(1 << 17)
#define	HD_OMIT_CHARS	(1 << 18)

#ifdef __cplusplus
}
#endif

#endif
