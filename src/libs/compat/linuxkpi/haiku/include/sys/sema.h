/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/sema.h> (sema(9)), used by LinuxKPI's
 * linux/semaphore.h. Same structure as FreeBSD (a counter under a mutex
 * with a condition variable); implemented in lkpi_sema.c on
 * freebsd_network's mtx and cv.
 */
#ifndef _LKPI_HAIKU_SYS_SEMA_H_
#define _LKPI_HAIKU_SYS_SEMA_H_

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>

struct sema {
	struct mtx	sema_mtx;
	struct cv	sema_cv;
	int		sema_waiters;
	int		sema_value;
};

#ifdef __cplusplus
extern "C" {
#endif
void	sema_init(struct sema *sema, int value, const char *description);
void	sema_destroy(struct sema *sema);
void	_sema_post(struct sema *sema, const char *file, int line);
void	_sema_wait(struct sema *sema, const char *file, int line);
int	_sema_timedwait(struct sema *sema, int timo, const char *file,
	    int line);
int	_sema_trywait(struct sema *sema, const char *file, int line);
int	sema_value(struct sema *sema);
#ifdef __cplusplus
}
#endif

#define	sema_post(sema)		_sema_post((sema), __FILE__, __LINE__)
#define	sema_wait(sema)		_sema_wait((sema), __FILE__, __LINE__)
#define	sema_timedwait(sema, timo)				\
	_sema_timedwait((sema), (timo), __FILE__, __LINE__)
#define	sema_trywait(sema)	_sema_trywait((sema), __FILE__, __LINE__)

#endif
