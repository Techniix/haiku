/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	System V shared memory: thin wrappers around the kernel implementation
	in src/system/kernel/posix/xsi_shared_memory.cpp.
*/


#include <sys/shm.h>

#include <errno.h>

#include <errno_private.h>
#include <syscall_utils.h>
#include <syscalls.h>


int
shmget(key_t key, size_t size, int flags)
{
	RETURN_AND_SET_ERRNO(_kern_xsi_shmget(key, size, flags));
}


void*
shmat(int id, const void* address, int flags)
{
	void* result;
	status_t status = _kern_xsi_shmat(id, address, flags, &result);
	if (status != B_OK) {
		__set_errno(status);
		return (void*)-1;
	}
	return result;
}


int
shmdt(const void* address)
{
	RETURN_AND_SET_ERRNO(_kern_xsi_shmdt(address));
}


int
shmctl(int id, int command, struct shmid_ds* buffer)
{
	RETURN_AND_SET_ERRNO(_kern_xsi_shmctl(id, command, buffer));
}
