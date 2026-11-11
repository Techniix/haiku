/*
 * Copyright 2009, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */

#include <RWLockManager.h>

#include <AutoLocker.h>

#include <syscalls.h>
#include <user_thread.h>


RWLockable::RWLockable()
	:
	fOwner(-1),
	fOwnerCount(0),
	fReaderCount(0)
{
}


RWLockManager::RWLockManager()
	:
	fLock("r/w lock manager")
{
}


RWLockManager::~RWLockManager()
{
}


bool
RWLockManager::ReadLock(RWLockable* lockable)
{
	AutoLocker<RWLockManager> locker(this);

	// Readers may enter while nobody holds or waits for the write lock, and
	// the write lock's owner may read-lock it, too. (Upstream only looked at
	// the queue and let readers in while a writer held the lock.)
	if (lockable->fOwner == find_thread(NULL)
		|| (lockable->fOwner < 0 && lockable->fWaiters.IsEmpty())) {
		lockable->fReaderCount++;
		return true;
	}

	return _Wait(lockable, false, B_INFINITE_TIMEOUT) == B_OK;
}


bool
RWLockManager::TryReadLock(RWLockable* lockable)
{
	AutoLocker<RWLockManager> locker(this);

	// Readers may enter while nobody holds or waits for the write lock, and
	// the write lock's owner may read-lock it, too. (Upstream only looked at
	// the queue and let readers in while a writer held the lock.)
	if (lockable->fOwner == find_thread(NULL)
		|| (lockable->fOwner < 0 && lockable->fWaiters.IsEmpty())) {
		lockable->fReaderCount++;
		return true;
	}

	return false;
}


status_t
RWLockManager::ReadLockWithTimeout(RWLockable* lockable, bigtime_t timeout)
{
	AutoLocker<RWLockManager> locker(this);

	// Readers may enter while nobody holds or waits for the write lock, and
	// the write lock's owner may read-lock it, too. (Upstream only looked at
	// the queue and let readers in while a writer held the lock.)
	if (lockable->fOwner == find_thread(NULL)
		|| (lockable->fOwner < 0 && lockable->fWaiters.IsEmpty())) {
		lockable->fReaderCount++;
		return B_OK;
	}

	return _Wait(lockable, false, timeout);
}


void
RWLockManager::ReadUnlock(RWLockable* lockable)
{
	AutoLocker<RWLockManager> locker(this);

	if (lockable->fReaderCount <= 0) {
		debugger("RWLockManager::ReadUnlock(): Not read-locked!");
		return;
	}

	if (--lockable->fReaderCount == 0)
		_Unblock(lockable);
}


bool
RWLockManager::WriteLock(RWLockable* lockable)
{
	AutoLocker<RWLockManager> locker(this);

	thread_id thread = find_thread(NULL);

	if (lockable->fOwner == thread) {
		lockable->fOwnerCount++;
		return true;
	}

	// (Upstream did not check for another writer here: a second writer
	// got the lock while the first still held it.)
	if (lockable->fOwner < 0 && lockable->fReaderCount == 0
		&& lockable->fWaiters.IsEmpty()) {
		lockable->fOwnerCount = 1;
		lockable->fOwner = find_thread(NULL);
		return true;
	}

	return _Wait(lockable, true, B_INFINITE_TIMEOUT) == B_OK;
}


bool
RWLockManager::TryWriteLock(RWLockable* lockable)
{
	AutoLocker<RWLockManager> locker(this);

	thread_id thread = find_thread(NULL);

	if (lockable->fOwner == thread) {
		lockable->fOwnerCount++;
		return true;
	}

	// (Upstream did not check for another writer here: a second writer
	// got the lock while the first still held it.)
	if (lockable->fOwner < 0 && lockable->fReaderCount == 0
		&& lockable->fWaiters.IsEmpty()) {
		lockable->fOwnerCount++;
		lockable->fOwner = thread;
		return true;
	}

	return false;
}


status_t
RWLockManager::WriteLockWithTimeout(RWLockable* lockable, bigtime_t timeout)
{
	AutoLocker<RWLockManager> locker(this);

	thread_id thread = find_thread(NULL);

	if (lockable->fOwner == thread) {
		lockable->fOwnerCount++;
		return B_OK;
	}

	// (Upstream did not check for another writer here: a second writer
	// got the lock while the first still held it.)
	if (lockable->fOwner < 0 && lockable->fReaderCount == 0
		&& lockable->fWaiters.IsEmpty()) {
		lockable->fOwnerCount++;
		lockable->fOwner = thread;
		return B_OK;
	}

	return _Wait(lockable, true, timeout);
}


void
RWLockManager::WriteUnlock(RWLockable* lockable)
{
	AutoLocker<RWLockManager> locker(this);

	if (find_thread(NULL) != lockable->fOwner) {
		debugger("RWLockManager::WriteUnlock(): Not write-locked by calling "
			"thread!");
		return;
	}

	if (--lockable->fOwnerCount == 0) {
		lockable->fOwner = -1;
		_Unblock(lockable);
	}
}


status_t
RWLockManager::_Wait(RWLockable* lockable, bool writer, bigtime_t timeout)
{
	if (timeout == 0)
		return B_TIMED_OUT;

	// enqueue a waiter
	RWLockable::Waiter waiter(writer);
	lockable->fWaiters.Add(&waiter);
	waiter.queued = true;
	get_user_thread()->wait_status = 1;

	// An absolute timeout, so that waiting again after an interruption does
	// not start the time over.
	uint32 flags = 0;
	if (timeout >= 0 && timeout != B_INFINITE_TIMEOUT) {
		bigtime_t now = system_time();
		flags = B_ABSOLUTE_TIMEOUT;
		timeout = timeout > B_INFINITE_TIMEOUT - now
			? B_INFINITE_TIMEOUT : now + timeout;
	}

	// wait
	Unlock();

	status_t error;
	while (true) {
		error = _kern_block_thread(flags, timeout);
		if (error != B_INTERRUPTED)
			break;

		// Interrupted by a signal (SIGCHLD interrupts even when ignored): the
		// kernel left B_INTERRUPTED in our wait status, so blocking again
		// would return at once and _kern_unblock_thread() would not wake us
		// any more. Check under the manager's lock whether _Unblock() has
		// given us the lock meanwhile; if not, arm the wait status again
		// while holding it, so that the hand-over cannot be lost.
		Lock();
		if (!waiter.queued)
			return waiter.status;
		get_user_thread()->wait_status = 1;
		Unlock();
	}

	Lock();

	if (!waiter.queued)
		return waiter.status;

	// we're still queued, which means an error (timeout, interrupt)
	// occurred
	lockable->fWaiters.Remove(&waiter);

	_Unblock(lockable);

	return error;
}


void
RWLockManager::_Unblock(RWLockable* lockable)
{
	// Check whether there any waiting threads at all and whether anyone
	// has the write lock
	RWLockable::Waiter* waiter = lockable->fWaiters.Head();
	if (waiter == NULL || lockable->fOwner >= 0)
		return;

	// writer at head of queue?
	if (waiter->writer) {
		if (lockable->fReaderCount == 0) {
			waiter->status = B_OK;
			waiter->queued = false;
			lockable->fWaiters.Remove(waiter);
			lockable->fOwner = waiter->thread;
			lockable->fOwnerCount = 1;

			_kern_unblock_thread(waiter->thread, B_OK);
		}
		return;
	}

	// wake up one or more readers -- we unblock more than one reader at
	// a time to save trips to the kernel
	while (!lockable->fWaiters.IsEmpty()
			&& !lockable->fWaiters.Head()->writer) {
		static const int kMaxReaderUnblockCount = 128;
		thread_id readers[kMaxReaderUnblockCount];
		int readerCount = 0;

		while (readerCount < kMaxReaderUnblockCount
				&& (waiter = lockable->fWaiters.Head()) != NULL
				&& !waiter->writer) {
			waiter->status = B_OK;
			waiter->queued = false;
			lockable->fWaiters.Remove(waiter);

			readers[readerCount++] = waiter->thread;
			lockable->fReaderCount++;
		}

		if (readerCount > 0)
			_kern_unblock_threads(readers, readerCount, B_OK);
	}
}
