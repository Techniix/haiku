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
 * A futex is identified by (team, address). Linux identifies a shared
 * (non-FUTEX_PRIVATE_FLAG) futex by the underlying memory object instead, so
 * that processes sharing memory can use it; that is not supported yet. glibc
 * uses shared futexes inside one process too (pthread_join() waits on the
 * thread's TID word without FUTEX_PRIVATE_FLAG), which works.
 *
 * Priority-inheritance operations (FUTEX_LOCK_PI and friends, used only by
 * PTHREAD_PRIO_INHERIT mutexes) return ENOSYS.
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
struct futex_waiter {
	futex_waiter*		next;
	futex_waiter*		previous;
	futex_bucket*		bucket;
	team_id				team;
	addr_t				address;
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
bucket_for(team_id team, addr_t address)
{
	uint64 key = ((uint64)address >> 2) ^ ((uint64)(uint32)team << 32);
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
wake_locked(futex_bucket* bucket, team_id team, addr_t address, int32 count,
	uint32 bitset)
{
	int32 woken = 0;
	futex_waiter* waiter = bucket->head;
	while (waiter != NULL) {
		futex_waiter* next = waiter->next;
		if (waiter->team == team && waiter->address == address
			&& (waiter->bitset & bitset) != 0) {
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


int32
renguin_futex_wake(team_id team, addr_t address, int32 count)
{
	futex_bucket* bucket = bucket_for(team, address);
	MutexLocker locker(bucket->lock);
	return wake_locked(bucket, team, address, count,
		LX_FUTEX_BITSET_MATCH_ANY);
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
futex_wait(team_id team, uint64 address, uint32 value, uint32 flags,
	bigtime_t timeout, uint32 bitset)
{
	if (bitset == 0)
		return -LX_EINVAL;

	futex_waiter waiter;
	waiter.next = waiter.previous = NULL;
	waiter.team = team;
	waiter.address = (addr_t)address;
	waiter.bitset = bitset;
	waiter.woken = false;
	waiter.condition.Init(&waiter, "renguin futex");

	ConditionVariableEntry entry;

	futex_bucket* bucket = bucket_for(team, (addr_t)address);
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
futex_requeue(team_id team, uint64 address, uint64 address2, int32 wakeCount,
	int32 requeueCount, bool compare, uint32 value)
{
	int64 error = check_address(address2);
	if (error != 0)
		return error;
	if (wakeCount < 0 || requeueCount < 0)
		return -LX_EINVAL;

	futex_bucket* bucket = bucket_for(team, (addr_t)address);
	futex_bucket* bucket2 = bucket_for(team, (addr_t)address2);
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
		if (waiter->team == team && waiter->address == (addr_t)address) {
			if (woken < wakeCount) {
				wake_waiter(bucket, waiter);
				woken++;
			} else {
				dequeue_waiter(bucket, waiter);
				waiter->address = (addr_t)address2;
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
futex_wake_op(team_id team, uint64 address, uint64 address2, int32 wakeCount,
	int32 wakeCount2, uint32 encoded)
{
	int64 error = check_address(address2);
	if (error != 0)
		return error;

	uint32 cmp = (encoded >> 24) & 0xf;
	if (cmp > LX_FUTEX_OP_CMP_GE)
		return -LX_ENOSYS;

	futex_bucket* bucket = bucket_for(team, (addr_t)address);
	futex_bucket* bucket2 = bucket_for(team, (addr_t)address2);
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

	int64 woken = wake_locked(bucket, team, (addr_t)address, wakeCount,
		LX_FUTEX_BITSET_MATCH_ANY);
	if (wake_op_compare(encoded, oldValue)) {
		woken += wake_locked(bucket2, team, (addr_t)address2, wakeCount2,
			LX_FUTEX_BITSET_MATCH_ANY);
	}

	unlock_two(bucket, bucket2);
	return woken;
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
		&& command != LX_FUTEX_WAIT_BITSET) {
		return -LX_ENOSYS;
	}

	int64 error = check_address(address);
	if (error != 0)
		return error;

	team_id team = thread_get_current_thread()->team->id;

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
			return futex_wait(team, address, (uint32)value, flags,
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
			futex_bucket* bucket = bucket_for(team, (addr_t)address);
			MutexLocker locker(bucket->lock);
			return wake_locked(bucket, team, (addr_t)address, (int32)value,
				bitset);
		}

		case LX_FUTEX_REQUEUE:
		case LX_FUTEX_CMP_REQUEUE:
			// The requeue count travels in the timeout argument.
			return futex_requeue(team, address, address2, (int32)value,
				(int32)timeout, command == LX_FUTEX_CMP_REQUEUE,
				(uint32)value3);

		case LX_FUTEX_WAKE_OP:
			return futex_wake_op(team, address, address2, (int32)value,
				(int32)timeout, (uint32)value3);

		case LX_FUTEX_FD:
		case LX_FUTEX_LOCK_PI:
		case LX_FUTEX_UNLOCK_PI:
		case LX_FUTEX_TRYLOCK_PI:
		case LX_FUTEX_WAIT_REQUEUE_PI:
		case LX_FUTEX_CMP_REQUEUE_PI:
		case LX_FUTEX_LOCK_PI2:
			log_unsupported(command);
			return -LX_ENOSYS;

		default:
			return -LX_ENOSYS;
	}
}
