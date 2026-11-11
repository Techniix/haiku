/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for the user-level reader/writer locks after a signal
	interrupts a waiter: libroot's rw_lock (app_server's MultiLocker),
	pthread_rwlock_t and libshared's RWLockManager.

	All three park a waiting thread with _kern_block_thread() and hand the
	lock over with _kern_unblock_thread(). When a signal interrupts the
	wait, the kernel stores B_INTERRUPTED as the thread's wait status;
	_kern_block_thread() then returns at once and _kern_unblock_thread()
	leaves a wait status that is not positive alone. rw_lock and
	RWLockManager simply blocked again, so the waiter spun for ever, and an
	rw_lock handed over meanwhile stayed "held" by the spinning thread: the
	app_server hang in screen_capture_test (SIGCHLD from the Screen
	recording prompt app_server starts interrupts app_server's threads even
	though it is ignored). pthread_rwlock rechecked its queue and was only a
	busy loop until the hand-over.

	Each case holds the lock, lets a second thread wait for it, interrupts
	that thread (SIGCHLD, which is ignored by default but interrupts, and
	SIGUSR1 with a handler), measures the waiter's CPU time for a while (a
	blocked thread uses none), releases the lock and wants the waiter to get
	it within 3 s. A waiter that never gets it is killed so that the test
	ends. Output format: see 3rdparty/ce/README.md.
*/


#include <OS.h>

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <new>

#include <locks.h>
#include <RWLockManager.h>


#define SUITE "user_locks"

// a waiting thread may use this much CPU time while it waits (scheduling
// and the signal's delivery), measured over kMeasureTime
static const bigtime_t kMeasureTime = 400000;
static const bigtime_t kMaxWaitCPU = 40000;
static const bigtime_t kHandOverTimeout = 3000000;


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static void
usr1_handler(int)
{
}


static bigtime_t
cpu_time(thread_id thread)
{
	thread_info info;
	if (get_thread_info(thread, &info) != B_OK)
		return -1;
	return info.user_time + info.kernel_time;
}


// #pragma mark - the locks under test


enum LockKind {
	kRWLock,
	kPthreadRWLock,
	kRWLockManager
};


static rw_lock sRWLock;
static pthread_rwlock_t sPthreadLock;
static RWLockManager* sManager;
static RWLockable sLockable;


static void
init_locks()
{
	rw_lock_init(&sRWLock, "ce test rw_lock");
	pthread_rwlock_init(&sPthreadLock, NULL);
	sManager = new RWLockManager;
	sManager->Init();
}


static bool
lock(LockKind kind, bool write)
{
	switch (kind) {
		case kRWLock:
			return (write ? rw_lock_write_lock(&sRWLock)
				: rw_lock_read_lock(&sRWLock)) == B_OK;
		case kPthreadRWLock:
			return (write ? pthread_rwlock_wrlock(&sPthreadLock)
				: pthread_rwlock_rdlock(&sPthreadLock)) == 0;
		case kRWLockManager:
			return write ? sManager->WriteLock(&sLockable)
				: sManager->ReadLock(&sLockable);
	}
	return false;
}


static void
unlock(LockKind kind, bool write)
{
	switch (kind) {
		case kRWLock:
			if (write)
				rw_lock_write_unlock(&sRWLock);
			else
				rw_lock_read_unlock(&sRWLock);
			break;
		case kPthreadRWLock:
			pthread_rwlock_unlock(&sPthreadLock);
			break;
		case kRWLockManager:
			if (write)
				sManager->WriteUnlock(&sLockable);
			else
				sManager->ReadUnlock(&sLockable);
			break;
	}
}


// #pragma mark - waiter


struct Waiter {
	LockKind		kind;
	bool			write;
	int32			state;
		// 0 before the lock call, 1 waiting, 2 got it, 3 the call failed
	int32			release;
};


static status_t
waiter_thread(void* data)
{
	Waiter* waiter = (Waiter*)data;
	atomic_set(&waiter->state, 1);
	bool locked = lock(waiter->kind, waiter->write);
	atomic_set(&waiter->state, locked ? 2 : 3);
	if (!locked)
		return B_ERROR;

	while (atomic_get(&waiter->release) == 0)
		snooze(10000);
	unlock(waiter->kind, waiter->write);
	return B_OK;
}


static void
run_case(const char* name, LockKind kind, bool holderWrites,
	bool waiterWrites, int signal)
{
	char check[64];

	if (!lock(kind, holderWrites)) {
		snprintf(check, sizeof(check), "%s_handover", name);
		report(check, false, "the holder could not take the lock");
		return;
	}

	Waiter waiter = { kind, waiterWrites, 0, 0 };
	thread_id thread = spawn_thread(waiter_thread, "lock waiter",
		B_NORMAL_PRIORITY, &waiter);
	resume_thread(thread);

	// let it block in the lock call
	bigtime_t deadline = system_time() + 1000000;
	while (atomic_get(&waiter.state) == 0 && system_time() < deadline)
		snooze(5000);
	snooze(200000);
	bool waited = atomic_get(&waiter.state) == 1;

	status_t sent = send_signal(thread, signal);
	snooze(100000);

	// a blocked thread uses no CPU time; a spinning one uses all it gets
	bigtime_t cpuStart = cpu_time(thread);
	snooze(kMeasureTime);
	bigtime_t cpuUsed = cpu_time(thread) - cpuStart;

	snprintf(check, sizeof(check), "%s_blocked", name);
	report(check, waited && sent == B_OK && cpuUsed <= kMaxWaitCPU,
		"waiter %s, signal %d sent: %s; CPU time while waiting after it "
		"%" B_PRIdBIGTIME " us in %" B_PRIdBIGTIME " ms (at most %"
		B_PRIdBIGTIME " us)", waited ? "waiting" : "not waiting", signal,
		strerror(sent), cpuUsed, kMeasureTime / 1000, kMaxWaitCPU);

	bigtime_t released = system_time();
	unlock(kind, holderWrites);

	deadline = released + kHandOverTimeout;
	while (atomic_get(&waiter.state) == 1 && system_time() < deadline)
		snooze(5000);
	int32 state = atomic_get(&waiter.state);
	bigtime_t handOver = system_time() - released;

	snprintf(check, sizeof(check), "%s_handover", name);
	report(check, state == 2, "%s %" B_PRIdBIGTIME " ms after the release",
		state == 2 ? "waiter got the lock"
			: state == 3 ? "the lock call failed"
			: "waiter still waiting", handOver / 1000);

	if (state == 2) {
		atomic_set(&waiter.release, 1);
		status_t result;
		wait_for_thread(thread, &result);
		// the lock must be free again
		bool free = lock(kind, true);
		if (free)
			unlock(kind, true);
		snprintf(check, sizeof(check), "%s_free", name);
		report(check, free, "the lock can be taken again after the waiter "
			"released it");
	} else {
		// it can never finish: the lock is unusable from here on
		kill_thread(thread);
		status_t result;
		wait_for_thread(thread, &result);
		if (kind == kRWLock) {
			rw_lock_init(&sRWLock, "ce test rw_lock (again)");
		} else if (kind == kPthreadRWLock) {
			pthread_rwlock_init(&sPthreadLock, NULL);
		} else {
			sManager = new RWLockManager;
			sManager->Init();
			new(&sLockable) RWLockable;
		}
	}
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = usr1_handler;
	sigaction(SIGUSR1, &action, NULL);

	init_locks();

	// the app_server case: a writer waits for readers, SIGCHLD (ignored)
	run_case("rw_lock_writer_sigchld", kRWLock, false, true, SIGCHLD);
	run_case("rw_lock_writer_handler", kRWLock, false, true, SIGUSR1);
	run_case("rw_lock_reader_sigchld", kRWLock, true, false, SIGCHLD);
	run_case("pthread_rwlock_writer_sigchld", kPthreadRWLock, false, true,
		SIGCHLD);
	run_case("pthread_rwlock_reader_handler", kPthreadRWLock, true, false,
		SIGUSR1);
	run_case("rwlockmanager_writer_sigchld", kRWLockManager, false, true,
		SIGCHLD);
	// RWLockManager also let a second writer, or a reader, in while a writer
	// held the lock (it only looked at the queue); these two cases fail on
	// that as well ("not waiting").
	run_case("rwlockmanager_writer_handler", kRWLockManager, true, true,
		SIGUSR1);
	run_case("rwlockmanager_reader_handler", kRWLockManager, true, false,
		SIGUSR1);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
