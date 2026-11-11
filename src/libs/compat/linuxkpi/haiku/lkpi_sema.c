/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * sema(9) for LinuxKPI's linux/semaphore.h, on freebsd_network's mtx and cv.
 * Semantics follow FreeBSD's kern_sema.c: a counting semaphore; waiters
 * sleep on the condition variable while the count is zero.
 *
 * cv_timedwait() is a freebsd_network function, so its result is a Haiku
 * error code and is translated (lkpi_haiku_wrap.h does not cover it).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sema.h>

void
sema_init(struct sema *sema, int value, const char *description)
{
	KASSERT((value >= 0), ("%s(): negative value\n", __func__));
	bzero(sema, sizeof(*sema));
	mtx_init(&sema->sema_mtx, description, "sema backing lock",
	    MTX_DEF);
	cv_init(&sema->sema_cv, description);
	sema->sema_value = value;
}

void
sema_destroy(struct sema *sema)
{
	KASSERT((sema->sema_waiters == 0), ("%s(): waiters\n", __func__));
	mtx_destroy(&sema->sema_mtx);
	cv_destroy(&sema->sema_cv);
}

void
_sema_post(struct sema *sema, const char *file, int line)
{
	mtx_lock(&sema->sema_mtx);
	sema->sema_value++;
	if (sema->sema_waiters && sema->sema_value > 0)
		cv_signal(&sema->sema_cv);
	mtx_unlock(&sema->sema_mtx);
}

void
_sema_wait(struct sema *sema, const char *file, int line)
{
	mtx_lock(&sema->sema_mtx);
	while (sema->sema_value == 0) {
		sema->sema_waiters++;
		cv_wait(&sema->sema_cv, &sema->sema_mtx);
		sema->sema_waiters--;
	}
	sema->sema_value--;
	mtx_unlock(&sema->sema_mtx);
}

/* Returns 0 if the semaphore was taken, else EWOULDBLOCK (as FreeBSD). */
int
_sema_timedwait(struct sema *sema, int timo, const char *file, int line)
{
	int error;

	mtx_lock(&sema->sema_mtx);
	for (error = 0; sema->sema_value == 0 && error == 0;) {
		sema->sema_waiters++;
		error = lkpi_errno_from_haiku(cv_timedwait(&sema->sema_cv,
		    &sema->sema_mtx, timo));
		sema->sema_waiters--;
	}
	if (sema->sema_value > 0) {
		sema->sema_value--;
		error = 0;
	}
	mtx_unlock(&sema->sema_mtx);
	return (error);
}

int
_sema_trywait(struct sema *sema, const char *file, int line)
{
	int ret;

	mtx_lock(&sema->sema_mtx);
	if (sema->sema_value > 0) {
		sema->sema_value--;
		ret = 1;
	} else
		ret = 0;
	mtx_unlock(&sema->sema_mtx);
	return (ret);
}

int
sema_value(struct sema *sema)
{
	int ret;

	mtx_lock(&sema->sema_mtx);
	ret = sema->sema_value;
	mtx_unlock(&sema->sema_mtx);
	return (ret);
}
