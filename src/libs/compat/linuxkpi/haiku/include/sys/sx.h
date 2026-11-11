/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/sx.h>, replacing (not extending) Haiku's
 * freebsd_network compat/sys/sx.h inside LinuxKPI translation units.
 *
 * Haiku's sx is a bare rw_lock with only init/xlock/xunlock/destroy, and
 * Haiku's rw_lock has no try-lock. LinuxKPI needs try-lock, owner queries,
 * interruptible variants and the 'sx_lock' owner word (linux/mutex.h reads
 * m->sx.sx_lock). Shared locking is mapped to exclusive: correct, less
 * concurrent. Implementation: haiku/lkpi_sx.c (Haiku mutex underneath).
 * No struct sx crosses the boundary into Haiku's freebsd_network.
 */
#ifndef _LKPI_HAIKU_SYS_SX_H_
#define _LKPI_HAIKU_SYS_SX_H_
#define _FBSD_COMPAT_SYS_SX_H_	/* suppress Haiku's compat/sys/sx.h */

#include <sys/types.h>
#include <lock.h>

struct thread;

struct sx {
	mutex		sx_mtx;		/* Haiku kernel mutex (token 'mutex' is
					   renamed only after this header) */
	uintptr_t	sx_lock;	/* owning struct thread*, 0 if free */
	const char	*sx_name;
};

#define SX_DUPOK	0x01
#define SX_NOWITNESS	0x02
#define SX_RECURSE	0x04
#define SX_NEW		0x20
#define SX_NOADAPTIVE	0x10
#define SX_OWNER(x)	((x) & ~(uintptr_t)0)
#define SX_LOCKED	1
#define SA_XLOCKED	0x04
#define SA_LOCKED	0x01
#define SA_UNLOCKED	0x00

#ifdef __cplusplus
extern "C" {
#endif
void	lkpi_sx_init_flags(struct sx *, const char *, int);
void	lkpi_sx_destroy(struct sx *);
void	lkpi_sx_xlock(struct sx *);
int	lkpi_sx_xlock_sig(struct sx *);
int	lkpi_sx_try_xlock(struct sx *);
void	lkpi_sx_xunlock(struct sx *);
int	lkpi_sx_xlocked(struct sx *);
struct thread *lkpi_sx_xholder(struct sx *);
#ifdef __cplusplus
}
#endif

#define sx_init(sx, name)		lkpi_sx_init_flags(sx, name, 0)
#define sx_init_flags(sx, name, f)	lkpi_sx_init_flags(sx, name, f)
#define sx_destroy(sx)			lkpi_sx_destroy(sx)
#define sx_xlock(sx)			lkpi_sx_xlock(sx)
#define sx_xlock_sig(sx)		lkpi_sx_xlock_sig(sx)
#define sx_try_xlock(sx)		lkpi_sx_try_xlock(sx)
#define sx_xunlock(sx)			lkpi_sx_xunlock(sx)
#define sx_xlocked(sx)			lkpi_sx_xlocked(sx)
#define sx_xholder(sx)			lkpi_sx_xholder(sx)
/* shared == exclusive (see above) */
#define sx_slock(sx)			lkpi_sx_xlock(sx)
#define sx_slock_sig(sx)		lkpi_sx_xlock_sig(sx)
#define sx_try_slock(sx)		lkpi_sx_try_xlock(sx)
#define sx_sunlock(sx)			lkpi_sx_xunlock(sx)
#define sx_downgrade(sx)		do { } while (0)
#define sx_has_waiters(sx)		(0)
#define sx_assert(sx, what)		do { } while (0)

/*
 * SX_SYSINIT / SX_SYSINIT_FLAGS (FreeBSD sys/sx.h): a statically declared
 * lock initialised by a SYSINIT at SI_SUB_LOCK, before any module_init().
 * Linux's DEFINE_MUTEX() expands to this in linux/mutex.h.
 */
#include <sys/kernel.h>

struct sx_args {
	struct sx	*sa_sx;
	const char	*sa_desc;
	int		sa_flags;
};

static inline void
lkpi_sx_sysinit(void *arg)
{
	struct sx_args *sargs = (struct sx_args *)arg;

	lkpi_sx_init_flags(sargs->sa_sx, sargs->sa_desc, sargs->sa_flags);
}

static inline void
lkpi_sx_sysuninit(void *arg)
{
	lkpi_sx_destroy((struct sx *)arg);
}

#define SX_SYSINIT_FLAGS(name, sxa, desc, flags)			\
	static struct sx_args name##_args = {				\
		(sxa),							\
		(desc),							\
		(flags)							\
	};								\
	SYSINIT(name##_sx_sysinit, SI_SUB_LOCK, SI_ORDER_MIDDLE,	\
	    lkpi_sx_sysinit, &name##_args);				\
	SYSUNINIT(name##_sx_sysuninit, SI_SUB_LOCK, SI_ORDER_MIDDLE,	\
	    lkpi_sx_sysuninit, (sxa))
#define SX_SYSINIT(name, sxa, desc)	SX_SYSINIT_FLAGS(name, sxa, desc, 0)

#endif
