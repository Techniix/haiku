/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin: a Linux x86-64 ABI personality for Haiku. Registers itself as a
 * foreign ABI personality (see headers/private/kernel/foreign_personality.h)
 * so the kernel routes unmodified Linux executables and their system calls to
 * this module. Native Haiku programs are untouched.
 *
 * This is the proof-of-concept stage: it loads a statically linked Linux
 * x86-64 executable and implements write(), exit() and exit_group().
 */


#include "renguin.h"

#include <KernelExport.h>


#define RENGUIN_MODULE_NAME \
	FOREIGN_PERSONALITY_MODULE_PREFIX "/renguin/v1"


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			status_t status = renguin_futex_init();
			if (status != B_OK)
				return status;
			renguin_procfs_init();
			dprintf("renguin: Linux x86-64 personality loaded\n");
			return B_OK;
		}
		case B_MODULE_UNINIT:
			renguin_futex_uninit();
			return B_OK;
		default:
			return B_ERROR;
	}
}


static foreign_personality_module_info sRenguinModule = {
	{
		RENGUIN_MODULE_NAME,
		0,
		std_ops
	},

	"linux",
	renguin_probe,
	renguin_load,
	renguin_enter_userspace,
	renguin_syscall,
	renguin_team_released,
	renguin_team_forked,
	renguin_thread_entering_userspace,
	renguin_setup_signal_frame
};


bool
renguin_is_linux_team(Team* team)
{
	return team != NULL && team->personality == &sRenguinModule;
}


module_info* modules[] = {
	(module_info*)&sRenguinModule,
	NULL
};
