/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_XSI_SHARED_MEMORY_H
#define KERNEL_XSI_SHARED_MEMORY_H


#include <sys/cdefs.h>
#include <sys/shm.h>

#include <OS.h>


__BEGIN_DECLS

void		xsi_shm_init();

int			_user_xsi_shmget(key_t key, size_t size, int flags);
status_t	_user_xsi_shmat(int id, const void* address, int flags,
				void** _address);
status_t	_user_xsi_shmdt(const void* address);
status_t	_user_xsi_shmctl(int id, int command, struct shmid_ds* buffer);

__END_DECLS


#endif	/* KERNEL_XSI_SHARED_MEMORY_H */
