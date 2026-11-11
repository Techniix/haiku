/*
 * Copyright 2009, Colin Günther, coling@gmx.de.
 * Copyright 2007, Hugo Santos. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _FBSD_COMPAT_SYS__MUTEX_H_
#define _FBSD_COMPAT_SYS__MUTEX_H_


#include <lock.h>
#include <KernelExport.h>


struct mtx {
	int						type;
	union {
		struct {
			mutex			lock;
			thread_id		owner;
		}					mutex_;
		recursive_lock		recursive;
		struct {
			spinlock		lock;
			cpu_status		state;
			thread_id		owner;
				// the holder, for mtx_owned(); the union's size is
				// set by the larger members, so this adds nothing
		}					spinlock_;
	} u;
};


#endif /* _FBSD_COMPAT_SYS__MUTEX_H_ */
