/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_AUDIO_TEST_SHIM_LOCK_H
#define _USB_AUDIO_TEST_SHIM_LOCK_H

/*!	The kernel mutex API the usb_audio driver uses, on pthread mutexes, for
	usb_audio_uac2_test (see kernel.h in this directory).
*/

#include <pthread.h>

#include "shim_dprintf.h"

typedef struct mutex {
	pthread_mutex_t	lock;
} mutex;

static inline void
mutex_init(mutex* lock, const char* name)
{
	pthread_mutex_init(&lock->lock, NULL);
}

static inline void
mutex_destroy(mutex* lock)
{
	pthread_mutex_destroy(&lock->lock);
}

static inline status_t
mutex_lock(mutex* lock)
{
	return pthread_mutex_lock(&lock->lock) == 0 ? B_OK : B_ERROR;
}

static inline void
mutex_unlock(mutex* lock)
{
	pthread_mutex_unlock(&lock->lock);
}

#endif	// _USB_AUDIO_TEST_SHIM_LOCK_H
