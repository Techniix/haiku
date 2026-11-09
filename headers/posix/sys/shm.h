/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYS_SHM_H
#define _SYS_SHM_H


#include <sys/cdefs.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <time.h>


/* shmat() flags */
#define SHM_RDONLY	010000	/* attach read-only */
#define SHM_RND		020000	/* round the attach address down to SHMLBA */

/* Segment low boundary address multiple: the page size. */
#define SHMLBA		4096

typedef unsigned long shmatt_t;

struct shmid_ds {
	struct ipc_perm	shm_perm;	/* operation permission structure */
	size_t			shm_segsz;	/* size of segment in bytes */
	pid_t			shm_lpid;	/* process ID of last shmat()/shmdt() */
	pid_t			shm_cpid;	/* process ID of creator */
	shmatt_t		shm_nattch;	/* number of current attaches */
	time_t			shm_atime;	/* time of last shmat() */
	time_t			shm_dtime;	/* time of last shmdt() */
	time_t			shm_ctime;	/* time of last change by shmctl() */
};


__BEGIN_DECLS

void*	shmat(int id, const void* address, int flags);
int		shmctl(int id, int command, struct shmid_ds* buffer);
int		shmdt(const void* address);
int		shmget(key_t key, size_t size, int flags);

__END_DECLS


#endif	/* _SYS_SHM_H */
