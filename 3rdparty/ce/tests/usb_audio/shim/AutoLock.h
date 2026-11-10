/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_AUDIO_TEST_SHIM_AUTO_LOCK_H
#define _USB_AUDIO_TEST_SHIM_AUTO_LOCK_H

//!	The kernel's MutexLocker for usb_audio_uac2_test (see kernel.h here).

#include <lock.h>

class MutexLocker {
public:
	MutexLocker(mutex& lock) : fLock(&lock) { mutex_lock(fLock); }
	MutexLocker(mutex* lock) : fLock(lock) { mutex_lock(fLock); }
	~MutexLocker() { if (fLock != NULL) mutex_unlock(fLock); }
	void Unlock() { if (fLock != NULL) mutex_unlock(fLock); fLock = NULL; }

private:
	mutex*	fLock;
};

#endif	// _USB_AUDIO_TEST_SHIM_AUTO_LOCK_H
