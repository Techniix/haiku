/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux futex(2): the primitive under every glibc and musl lock, condition
 * variable, semaphore, once and join.
 *
 * FreeBSD counterpart: sys/compat/linux/linux_futex.c, which maps futexes
 * onto FreeBSD's umtx queues (linux_futex_wait(), linux_futex_wake(),
 * linux_futex_requeue(), linux_futex_wakeop()). Haiku's user mutexes
 * (src/system/kernel/locks/user_mutex.cpp) implement Haiku's own lock
 * protocol, not the futex one, so Renguin keeps its own wait queues, in the
 * same shape as Linux's kernel/futex/: a hash of buckets, each with a lock
 * and a list of waiters, and the "compare under the bucket lock, then sleep"
 * rule that makes FUTEX_WAIT atomic with respect to FUTEX_WAKE.
 *
 * A futex is identified by a key (futex_key): (team, address) for
 * FUTEX_PRIVATE_FLAG operations and for memory only this process maps, and,
 * as Linux's get_futex_key(), (memory object, offset) for a shared futex
 * (no FUTEX_PRIVATE_FLAG) in shared memory: a MAP_SHARED mapping (anonymous
 * ones shared across fork(), files, memfds) or a System V segment. So
 * processes that share memory can wait and wake each other: process-shared
 * pthread mutexes, condition variables, barriers and semaphores
 * (PTHREAD_PROCESS_SHARED, sem_init(pshared)), and robust mutexes in shared
 * memory. Shared memory is recognized as Haiku areas that are B_SHARED_AREA
 * (MAP_SHARED anonymous memory, clones such as shmat()) or map a file's
 * cache directly (REGION_NO_PRIVATE_MAP); the object is the area's cache.
 *
 * Priority-inheritance futexes (FUTEX_LOCK_PI, FUTEX_LOCK_PI2,
 * FUTEX_TRYLOCK_PI, FUTEX_UNLOCK_PI; PTHREAD_PRIO_INHERIT mutexes) follow
 * Linux's protocol on the futex word (owner TID, FUTEX_WAITERS), without
 * the priority boost itself: a waiter blocks until the owner unlocks. musl
 * probes FUTEX_LOCK_PI once before it accepts PTHREAD_PRIO_INHERIT and
 * PulseAudio's client library asserts on the answer (Chromium's
 * single-process mode aborted there). The requeue-PI operations (for
 * condition variables on PI mutexes; glibc only) return ENOSYS.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <condition_variable.h>
#include <kernel.h>
#include <lock.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/atomic.h>
#include <vm/vm.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>


// futex() operations and flags (include/uapi/linux/futex.h).
#define LX_FUTEX_WAIT				0
#define LX_FUTEX_WAKE				1
#define LX_FUTEX_FD					2
#define LX_FUTEX_REQUEUE			3
#define LX_FUTEX_CMP_REQUEUE		4
#define LX_FUTEX_WAKE_OP			5
#define LX_FUTEX_LOCK_PI			6
#define LX_FUTEX_UNLOCK_PI			7
#define LX_FUTEX_TRYLOCK_PI			8
#define LX_FUTEX_WAIT_BITSET		9
#define LX_FUTEX_WAKE_BITSET		10
#define LX_FUTEX_WAIT_REQUEUE_PI	11
#define LX_FUTEX_CMP_REQUEUE_PI		12
#define LX_FUTEX_LOCK_PI2			13

// the PI futex word
#define LX_FUTEX_WAITERS			0x80000000u
#define LX_FUTEX_OWNER_DIED			0x40000000u
#define LX_FUTEX_TID_MASK			0x3fffffffu
#define LX_FUTEX_PRIVATE_FLAG		128
#define LX_FUTEX_CLOCK_REALTIME		256
#define LX_FUTEX_CMD_MASK			\
	(~(uint32)(LX_FUTEX_PRIVATE_FLAG | LX_FUTEX_CLOCK_REALTIME))
#define LX_FUTEX_BITSET_MATCH_ANY	0xffffffff

// FUTEX_WAKE_OP encoding.
#define LX_FUTEX_OP_SET				0
#define LX_FUTEX_OP_ADD				1
#define LX_FUTEX_OP_OR				2
#define LX_FUTEX_OP_ANDN			3
#define LX_FUTEX_OP_XOR				4
#define LX_FUTEX_OP_OPARG_SHIFT		8
#define LX_FUTEX_OP_CMP_EQ			0
#define LX_FUTEX_OP_CMP_NE			1
#define LX_FUTEX_OP_CMP_LT			2
#define LX_FUTEX_OP_CMP_LE			3
#define LX_FUTEX_OP_CMP_GT			4
#define LX_FUTEX_OP_CMP_GE			5

static const int32 kBucketCount = 256;	// power of two


struct futex_bucket;

/*!	A thread blocked in FUTEX_WAIT. Lives on the waiting thread's stack and
	is linked into its bucket while it waits. Everything but the condition
	variable is protected by the bucket's lock. A requeue moves the waiter to
	another bucket while holding both locks, so the waiter re-reads its bucket
	pointer until it has locked the right one.
*/
/*!	What a futex word is: \a space is the team ID (private), or the shared
	memory's cache pointer with the top bit set; \a offset is the address,
	or the offset in that cache.
*/
struct futex_key {
	uint64				space;
	uint64				offset;

	bool operator==(const futex_key& other) const
	{
		return space == other.space && offset == other.offset;
	}
};

static const uint64 kSharedFutexSpace = 1ULL << 63;

struct futex_waiter {
	futex_waiter*		next;
	futex_waiter*		previous;
	futex_bucket*		bucket;
	futex_key			key;
	uint32				bitset;
	bool				woken;
	ConditionVariable	condition;
};

struct futex_bucket {
	mutex				lock;
	futex_waiter*		head;
};

static futex_bucket sBuckets[kBucketCount];


// #pragma mark - queues


status_t
renguin_futex_init()
{
	for (int32 i = 0; i < kBucketCount; i++) {
		mutex_init(&sBuckets[i].lock, "renguin futex");
		sBuckets[i].head = NULL;
	}
	return B_OK;
}


void
renguin_futex_uninit()
{
	for (int32 i = 0; i < kBucketCount; i++)
		mutex_destroy(&sBuckets[i].lock);
}


static futex_bucket*
bucket_for(const futex_key& futexKey)
{
	uint64 key = (futexKey.offset >> 2) ^ (futexKey.space << 32)
		^ (futexKey.space >> 7);
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccdULL;
	key ^= key >> 33;
	return &sBuckets[key & (kBucketCount - 1)];
}


static void
enqueue_waiter(futex_bucket* bucket, futex_waiter* waiter)
{
	waiter->previous = NULL;
	waiter->next = bucket->head;
	if (bucket->head != NULL)
		bucket->head->previous = waiter;
	bucket->head = waiter;
	atomic_pointer_set(&waiter->bucket, bucket);
}


static void
dequeue_waiter(futex_bucket* bucket, futex_waiter* waiter)
{
	if (waiter->previous != NULL)
		waiter->previous->next = waiter->next;
	else
		bucket->head = waiter->next;
	if (waiter->next != NULL)
		waiter->next->previous = waiter->previous;
	waiter->next = waiter->previous = NULL;
}


//! Wakes a waiter. The bucket must be locked; the waiter is dequeued.
static void
wake_waiter(futex_bucket* bucket, futex_waiter* waiter)
{
	dequeue_waiter(bucket, waiter);
	waiter->woken = true;
	waiter->condition.NotifyAll(B_OK);
		// The waiter cannot leave (and take the condition variable with it)
		// before it has locked the bucket again, which we hold.
}


static int32
wake_locked(futex_bucket* bucket, const futex_key& key, int32 count,
	uint32 bitset)
{
	int32 woken = 0;
	futex_waiter* waiter = bucket->head;
	while (waiter != NULL) {
		futex_waiter* next = waiter->next;
		if (waiter->key == key && (waiter->bitset & bitset) != 0) {
			wake_waiter(bucket, waiter);
			// As Linux: a count of 0 still wakes one waiter.
			if (++woken >= count)
				break;
		}
		waiter = next;
	}
	return woken;
}


//! Locks the buckets of two futexes, in a fixed order to avoid deadlocks.
static void
lock_two(futex_bucket* first, futex_bucket* second)
{
	if (first == second) {
		mutex_lock(&first->lock);
	} else if (first < second) {
		mutex_lock(&first->lock);
		mutex_lock(&second->lock);
	} else {
		mutex_lock(&second->lock);
		mutex_lock(&first->lock);
	}
}


static void
unlock_two(futex_bucket* first, futex_bucket* second)
{
	mutex_unlock(&first->lock);
	if (second != first)
		mutex_unlock(&second->lock);
}


/*!	The key of the futex word at \a address of the current team. A shared
	futex (\a isPrivate false) in shared memory is keyed by the memory.
*/
static void
futex_key_for(uint64 address, bool isPrivate, futex_key* _key)
{
	_key->space = (uint32)thread_get_current_thread()->team->id;
	_key->offset = address;
	if (isPrivate)
		return;

	VMAddressSpace* addressSpace = VMAddressSpace::GetCurrent();
	if (addressSpace == NULL)
		return;
	addressSpace->ReadLock();
	VMArea* area = addressSpace->LookupArea((addr_t)address);
	if (area != NULL) {
		VMCache* cache = vm_area_get_locked_cache(area);
		if ((area->protection & B_SHARED_AREA) != 0
			|| cache->type == CACHE_TYPE_VNODE) {
			_key->space = (uint64)(addr_t)cache | kSharedFutexSpace;
			_key->offset = (uint64)(address - area->Base())
				+ (uint64)area->cache_offset;
		}
		vm_area_put_locked_cache(cache);
	}
	addressSpace->ReadUnlock();
	addressSpace->Put();
}


/*!	Wakes \a count waiters of the shared futex at \a address (thread exit:
	CLONE_CHILD_CLEARTID and robust lists, which Linux wakes as shared
	futexes). \a team is the current team.
*/
int32
renguin_futex_wake(team_id team, addr_t address, int32 count)
{
	futex_key key;
	if (team == thread_get_current_thread()->team->id)
		futex_key_for(address, false, &key);
	else {
		key.space = (uint32)team;
		key.offset = address;
	}
	futex_bucket* bucket = bucket_for(key);
	MutexLocker locker(bucket->lock);
	return wake_locked(bucket, key, count, LX_FUTEX_BITSET_MATCH_ANY);
}


// #pragma mark - operations


static int64
check_address(uint64 address)
{
	if ((address & (sizeof(uint32) - 1)) != 0)
		return -LX_EINVAL;
	if (!IS_USER_ADDRESS((addr_t)address))
		return -LX_EFAULT;
	return 0;
}


/*!	Converts a futex timeout to a Haiku wait timeout. FUTEX_WAIT takes a
	relative timeout; FUTEX_WAIT_BITSET an absolute one, on CLOCK_MONOTONIC
	(Haiku's system_time()) or, with FUTEX_CLOCK_REALTIME, CLOCK_REALTIME.
	No timeout means waiting forever.
*/
static int64
get_timeout(uint64 userTimeout, bool absolute, bool realtime, uint32* _flags,
	bigtime_t* _timeout)
{
	*_flags = B_CAN_INTERRUPT;
	*_timeout = 0;
	if (userTimeout == 0)
		return 0;

	int64 timespec[2];
	if (renguin_copy_in(timespec, userTimeout, sizeof(timespec)) != B_OK)
		return -LX_EFAULT;
	if (timespec[0] < 0 || timespec[1] < 0 || timespec[1] >= 1000000000)
		return -LX_EINVAL;

	if (timespec[0] >= B_INFINITE_TIMEOUT / 1000000 - 1)
		return 0;	// too far away to matter: wait forever
	bigtime_t timeout = timespec[0] * 1000000 + (timespec[1] + 999) / 1000;

	if (!absolute)
		*_flags |= B_RELATIVE_TIMEOUT;
	else if (realtime)
		*_flags |= B_ABSOLUTE_REAL_TIME_TIMEOUT;
	else
		*_flags |= B_ABSOLUTE_TIMEOUT;
	*_timeout = timeout;
	return 0;
}


static int64
futex_wait(const futex_key& key, uint64 address, uint32 value, uint32 flags,
	bigtime_t timeout, uint32 bitset)
{
	if (bitset == 0)
		return -LX_EINVAL;

	futex_waiter waiter;
	waiter.next = waiter.previous = NULL;
	waiter.key = key;
	waiter.bitset = bitset;
	waiter.woken = false;
	waiter.condition.Init(&waiter, "renguin futex");

	ConditionVariableEntry entry;

	futex_bucket* bucket = bucket_for(key);
	mutex_lock(&bucket->lock);

	// The value check and queueing happen under the bucket lock, which a
	// waker must take after changing the value: either we see the new value,
	// or the waker sees us.
	uint32 current;
	if (renguin_copy_in(&current, address, sizeof(current)) != B_OK) {
		mutex_unlock(&bucket->lock);
		return -LX_EFAULT;
	}
	if (current != value) {
		mutex_unlock(&bucket->lock);
		return -LX_EAGAIN;
	}

	enqueue_waiter(bucket, &waiter);
	waiter.condition.Add(&entry);
	mutex_unlock(&bucket->lock);

	status_t status = entry.Wait(flags, timeout);

	// Lock the bucket we are in now; a requeue may have moved us.
	while (true) {
		bucket = atomic_pointer_get(&waiter.bucket);
		mutex_lock(&bucket->lock);
		if (atomic_pointer_get(&waiter.bucket) == bucket)
			break;
		mutex_unlock(&bucket->lock);
	}

	int64 result;
	if (waiter.woken) {
		result = 0;
	} else {
		dequeue_waiter(bucket, &waiter);
		switch (status) {
			case B_TIMED_OUT:
			case B_WOULD_BLOCK:
				result = -LX_ETIMEDOUT;
				break;
			case B_INTERRUPTED:
				result = -LX_EINTR;
				break;
			default:
				// a spurious wakeup is allowed
				result = status == B_OK ? 0 : renguin_encode(status);
				break;
		}
	}
	mutex_unlock(&bucket->lock);
	return result;
}


static int64
futex_requeue(bool isPrivate, uint64 address, uint64 address2,
	int32 wakeCount, int32 requeueCount, bool compare, uint32 value)
{
	int64 error = check_address(address2);
	if (error != 0)
		return error;
	if (wakeCount < 0 || requeueCount < 0)
		return -LX_EINVAL;

	futex_key key, key2;
	futex_key_for(address, isPrivate, &key);
	futex_key_for(address2, isPrivate, &key2);
	futex_bucket* bucket = bucket_for(key);
	futex_bucket* bucket2 = bucket_for(key2);
	lock_two(bucket, bucket2);

	if (compare) {
		uint32 current;
		if (renguin_copy_in(&current, address, sizeof(current)) != B_OK) {
			unlock_two(bucket, bucket2);
			return -LX_EFAULT;
		}
		if (current != value) {
			unlock_two(bucket, bucket2);
			return -LX_EAGAIN;
		}
	}

	int32 woken = 0;
	int32 requeued = 0;
	futex_waiter* waiter = bucket->head;
	while (waiter != NULL && (woken < wakeCount || requeued < requeueCount)) {
		futex_waiter* next = waiter->next;
		if (waiter->key == key) {
			if (woken < wakeCount) {
				wake_waiter(bucket, waiter);
				woken++;
			} else {
				dequeue_waiter(bucket, waiter);
				waiter->key = key2;
				enqueue_waiter(bucket2, waiter);
				requeued++;
			}
		}
		waiter = next;
	}

	unlock_two(bucket, bucket2);
	return woken + requeued;
}


//! Applies a FUTEX_WAKE_OP operation to *address, atomically.
static status_t
apply_wake_op(uint64 address, uint32 encoded, uint32* _oldValue)
{
	uint32 op = (encoded >> 28) & 0xf;
	int32 oparg = ((int32)(encoded << 8)) >> 20;		// bits 12-23, signed
	if ((op & LX_FUTEX_OP_OPARG_SHIFT) != 0) {
		op &= ~(uint32)LX_FUTEX_OP_OPARG_SHIFT;
		oparg = (int32)(1u << (oparg & 31));
	}
	if (op > LX_FUTEX_OP_XOR)
		return B_NOT_SUPPORTED;

	for (int attempt = 0; attempt < 1000; attempt++) {
		uint32 oldValue;
		if (renguin_copy_in(&oldValue, address, sizeof(oldValue)) != B_OK)
			return B_BAD_ADDRESS;

		uint32 newValue;
		switch (op) {
			case LX_FUTEX_OP_SET:	newValue = (uint32)oparg; break;
			case LX_FUTEX_OP_ADD:	newValue = oldValue + (uint32)oparg; break;
			case LX_FUTEX_OP_OR:	newValue = oldValue | (uint32)oparg; break;
			case LX_FUTEX_OP_ANDN:	newValue = oldValue & ~(uint32)oparg; break;
			default:				newValue = oldValue ^ (uint32)oparg; break;
		}

		uint32 seen;
		status_t status = renguin_user_atomic_cmpxchg(address, oldValue,
			newValue, &seen);
		if (status != B_OK)
			return status;
		if (seen == oldValue) {
			*_oldValue = oldValue;
			return B_OK;
		}
	}
	return B_BUSY;
}


static bool
wake_op_compare(uint32 encoded, uint32 oldValue)
{
	uint32 cmp = (encoded >> 24) & 0xf;
	int32 cmparg = ((int32)(encoded << 20)) >> 20;		// bits 0-11, signed
	int32 value = (int32)oldValue;
	switch (cmp) {
		case LX_FUTEX_OP_CMP_EQ:	return value == cmparg;
		case LX_FUTEX_OP_CMP_NE:	return value != cmparg;
		case LX_FUTEX_OP_CMP_LT:	return value < cmparg;
		case LX_FUTEX_OP_CMP_LE:	return value <= cmparg;
		case LX_FUTEX_OP_CMP_GT:	return value > cmparg;
		case LX_FUTEX_OP_CMP_GE:	return value >= cmparg;
		default:					return false;
	}
}


/*!	FUTEX_WAKE_OP: change *address2, wake waiters on address, and also on
	address2 if its old value passes the encoded comparison. glibc's
	condition variables used it before glibc 2.25.
*/
static int64
futex_wake_op(bool isPrivate, uint64 address, uint64 address2,
	int32 wakeCount, int32 wakeCount2, uint32 encoded)
{
	int64 error = check_address(address2);
	if (error != 0)
		return error;

	uint32 cmp = (encoded >> 24) & 0xf;
	if (cmp > LX_FUTEX_OP_CMP_GE)
		return -LX_ENOSYS;

	futex_key key, key2;
	futex_key_for(address, isPrivate, &key);
	futex_key_for(address2, isPrivate, &key2);
	futex_bucket* bucket = bucket_for(key);
	futex_bucket* bucket2 = bucket_for(key2);
	lock_two(bucket, bucket2);

	uint32 oldValue = 0;
	status_t status = apply_wake_op(address2, encoded, &oldValue);
	if (status != B_OK) {
		unlock_two(bucket, bucket2);
		if (status == B_NOT_SUPPORTED)
			return -LX_ENOSYS;
		if (status == B_BUSY)
			return -LX_EAGAIN;
		return -LX_EFAULT;
	}

	int64 woken = wake_locked(bucket, key, wakeCount,
		LX_FUTEX_BITSET_MATCH_ANY);
	if (wake_op_compare(encoded, oldValue)) {
		woken += wake_locked(bucket2, key2, wakeCount2,
			LX_FUTEX_BITSET_MATCH_ANY);
	}

	unlock_two(bucket, bucket2);
	return woken;
}


/*!	FUTEX_LOCK_PI, FUTEX_LOCK_PI2 and (with \a tryOnly) FUTEX_TRYLOCK_PI:
	takes the lock word for the calling thread (word = TID, keeping
	FUTEX_OWNER_DIED clear: a dead owner's lock is simply taken over), or,
	when it is held, sets FUTEX_WAITERS (so the owner's unlock comes to the
	kernel) and waits for the unlock.
*/
static int64
futex_lock_pi(bool isPrivate, uint64 address, uint64 userTimeout,
	bool realtime, bool tryOnly)
{
	futex_key key;
	futex_key_for(address, isPrivate, &key);

	uint32 flags = B_CAN_INTERRUPT;
	bigtime_t timeout = 0;
	if (!tryOnly) {
		// FUTEX_LOCK_PI's timeout is absolute, on CLOCK_REALTIME
		// (FUTEX_LOCK_PI2: CLOCK_MONOTONIC unless FUTEX_CLOCK_REALTIME)
		int64 error = get_timeout(userTimeout, true, realtime, &flags,
			&timeout);
		if (error != 0)
			return error;
	}

	uint32 tid = (uint32)thread_get_current_thread()->id & LX_FUTEX_TID_MASK;
	for (int attempt = 0; attempt < 100000; attempt++) {
		uint32 value;
		if (renguin_copy_in(&value, address, sizeof(value)) != B_OK)
			return -LX_EFAULT;
		uint32 owner = value & LX_FUTEX_TID_MASK;
		if (owner == tid)
			return -LX_EDEADLK;

		uint32 seen;
		if (owner == 0) {
			// free (or its owner died): take it, keeping the waiters bit
			uint32 newValue = tid | (value & LX_FUTEX_WAITERS);
			if (renguin_user_atomic_cmpxchg(address, value, newValue, &seen)
					!= B_OK) {
				return -LX_EFAULT;
			}
			if (seen == value)
				return 0;
			continue;
		}
		if (tryOnly)
			return -LX_EAGAIN;

		if ((value & LX_FUTEX_WAITERS) == 0) {
			if (renguin_user_atomic_cmpxchg(address, value,
					value | LX_FUTEX_WAITERS, &seen) != B_OK) {
				return -LX_EFAULT;
			}
			if (seen != value)
				continue;
			value |= LX_FUTEX_WAITERS;
		}

		// Wait while the word still says "held, with waiters"; the unlock
		// changes it and wakes every waiter, which then try again.
		int64 result = futex_wait(key, address, value, flags, timeout,
			LX_FUTEX_BITSET_MATCH_ANY);
		if (result == -LX_ETIMEDOUT || result == -LX_EINTR)
			return result;
		// 0 (woken) or EAGAIN (the word changed): try again
	}
	return -LX_EAGAIN;
}


/*!	FUTEX_UNLOCK_PI: the owner releases the lock word and wakes all its
	waiters (they compete again; the winner sets FUTEX_WAITERS again if
	others still wait, so no wake-up is lost).
*/
static int64
futex_unlock_pi(bool isPrivate, uint64 address)
{
	uint32 tid = (uint32)thread_get_current_thread()->id & LX_FUTEX_TID_MASK;
	for (int attempt = 0; attempt < 100000; attempt++) {
		uint32 value;
		if (renguin_copy_in(&value, address, sizeof(value)) != B_OK)
			return -LX_EFAULT;
		if ((value & LX_FUTEX_TID_MASK) != tid)
			return -LX_EPERM;
		uint32 seen;
		if (renguin_user_atomic_cmpxchg(address, value, 0, &seen) != B_OK)
			return -LX_EFAULT;
		if (seen != value)
			continue;
		if ((value & LX_FUTEX_WAITERS) != 0) {
			futex_key key;
			futex_key_for(address, isPrivate, &key);
			futex_bucket* bucket = bucket_for(key);
			MutexLocker locker(bucket->lock);
			wake_locked(bucket, key, INT32_MAX, LX_FUTEX_BITSET_MATCH_ANY);
		}
		return 0;
	}
	return -LX_EAGAIN;
}


static void
log_unsupported(uint32 command)
{
	static team_id sLastTeam = -1;
	team_id team = thread_get_current_thread()->team->id;
	if (atomic_get_and_set(&sLastTeam, team) == team)
		return;
	dprintf("renguin: team %" B_PRId32 ": futex operation %" B_PRIu32
		" not supported\n", team, command);
}


/*!	futex(uaddr, op, val, timeout_or_val2, uaddr2, val3). */
int64
renguin_sys_futex(uint64 address, uint64 op, uint64 value, uint64 timeout,
	uint64 address2, uint64 value3)
{
	uint32 command = (uint32)op & LX_FUTEX_CMD_MASK;
	bool realtime = (op & LX_FUTEX_CLOCK_REALTIME) != 0;
	if (realtime && command != LX_FUTEX_WAIT
		&& command != LX_FUTEX_WAIT_BITSET && command != LX_FUTEX_LOCK_PI
		&& command != LX_FUTEX_LOCK_PI2) {
		return -LX_ENOSYS;
	}

	int64 error = check_address(address);
	if (error != 0)
		return error;

	bool isPrivate = (op & LX_FUTEX_PRIVATE_FLAG) != 0;

	switch (command) {
		case LX_FUTEX_WAIT:
		case LX_FUTEX_WAIT_BITSET:
		{
			bool bitsetOp = command == LX_FUTEX_WAIT_BITSET;
			uint32 flags;
			bigtime_t waitTimeout;
			error = get_timeout(timeout, bitsetOp, realtime, &flags,
				&waitTimeout);
			if (error != 0)
				return error;
			futex_key key;
			futex_key_for(address, isPrivate, &key);
			return futex_wait(key, address, (uint32)value, flags,
				waitTimeout, bitsetOp
					? (uint32)value3 : LX_FUTEX_BITSET_MATCH_ANY);
		}

		case LX_FUTEX_WAKE:
		case LX_FUTEX_WAKE_BITSET:
		{
			uint32 bitset = command == LX_FUTEX_WAKE_BITSET
				? (uint32)value3 : LX_FUTEX_BITSET_MATCH_ANY;
			if (bitset == 0)
				return -LX_EINVAL;
			futex_key key;
			futex_key_for(address, isPrivate, &key);
			futex_bucket* bucket = bucket_for(key);
			MutexLocker locker(bucket->lock);
			return wake_locked(bucket, key, (int32)value, bitset);
		}

		case LX_FUTEX_REQUEUE:
		case LX_FUTEX_CMP_REQUEUE:
			// The requeue count travels in the timeout argument.
			return futex_requeue(isPrivate, address, address2, (int32)value,
				(int32)timeout, command == LX_FUTEX_CMP_REQUEUE,
				(uint32)value3);

		case LX_FUTEX_WAKE_OP:
			return futex_wake_op(isPrivate, address, address2, (int32)value,
				(int32)timeout, (uint32)value3);

		case LX_FUTEX_LOCK_PI:
			// always CLOCK_REALTIME
			return futex_lock_pi(isPrivate, address, timeout, true, false);
		case LX_FUTEX_LOCK_PI2:
			return futex_lock_pi(isPrivate, address, timeout, realtime, false);
		case LX_FUTEX_TRYLOCK_PI:
			return futex_lock_pi(isPrivate, address, 0, false, true);
		case LX_FUTEX_UNLOCK_PI:
			return futex_unlock_pi(isPrivate, address);

		case LX_FUTEX_FD:
		case LX_FUTEX_WAIT_REQUEUE_PI:
		case LX_FUTEX_CMP_REQUEUE_PI:
			log_unsupported(command);
			return -LX_ENOSYS;

		default:
			return -LX_ENOSYS;
	}
}
