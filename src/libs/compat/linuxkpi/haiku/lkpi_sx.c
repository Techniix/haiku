/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * LinuxKPI's sx(9) on a Haiku kernel mutex. See include/sys/sx.h for why
 * shared locks are exclusive here. Built in the "shim" profile (no LinuxKPI
 * preinclude): 'mutex' is Haiku's type in this file.
 */
#include <lock.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sx.h>

void
lkpi_sx_init_flags(struct sx *sx, const char *name, int flags)
{
	mutex_init(&sx->sx_mtx, name != NULL ? name : "lkpi sx");
	sx->sx_lock = 0;
	sx->sx_name = name;
}

void
lkpi_sx_destroy(struct sx *sx)
{
	mutex_destroy(&sx->sx_mtx);
}

void
lkpi_sx_xlock(struct sx *sx)
{
	mutex_lock(&sx->sx_mtx);
	sx->sx_lock = (uintptr_t)curthread;
}

int
lkpi_sx_xlock_sig(struct sx *sx)
{
	/* Haiku mutexes are not interruptible; never reports EINTR. */
	lkpi_sx_xlock(sx);
	return (0);
}

int
lkpi_sx_try_xlock(struct sx *sx)
{
	if (mutex_trylock(&sx->sx_mtx) != B_OK)
		return (0);
	sx->sx_lock = (uintptr_t)curthread;
	return (1);
}

void
lkpi_sx_xunlock(struct sx *sx)
{
	sx->sx_lock = 0;
	mutex_unlock(&sx->sx_mtx);
}

int
lkpi_sx_xlocked(struct sx *sx)
{
	return (sx->sx_lock == (uintptr_t)curthread);
}

struct thread *
lkpi_sx_xholder(struct sx *sx)
{
	return ((struct thread *)sx->sx_lock);
}
