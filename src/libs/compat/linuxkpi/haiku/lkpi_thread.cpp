/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Per-thread state for LinuxKPI on Haiku:
 *  - struct thread / curthread (include/sys/proc.h): Haiku's freebsd_network
 *    has none; LinuxKPI hangs Linux 'current' off curthread->td_lkpi_task.
 *  - FreeBSD sleepqueue(9) (include/sys/sleepqueue.h), used by LinuxKPI's
 *    wait queues, completions and schedule(): implemented with one Haiku
 *    ConditionVariable per sleeping thread, on hashed wait channels.
 *  - sched_pin()/sched_unpin(): Haiku thread_pin_to_current_cpu().
 *
 * Built without the LinuxKPI preinclude ("shim" profile): E* values are
 * Haiku's; results returned to LinuxKPI go through lkpi_errno_from_haiku().
 */

/* Haiku kernel headers first: freebsd_network's haiku-module.h (pulled in
 * by <sys/param.h>) #undefs ASSERT, which these inline functions use. */
#include <KernelExport.h>
#include <OS.h>
#include <condition_variable.h>
#include <interrupts.h>
#include <smp.h>
#include <Notifications.h>
#include <thread.h>
#include <thread_types.h>
#include <util/AutoLock.h>
#include <util/KMessage.h>

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sleepqueue.h>
#include <sys/kernel.h>

#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" int lkpi_errno_from_haiku(int haikuError);
/* LinuxKPI side (lkpi_misc.c): FreeBSD's linuxkpi_thread_dtor(). */
extern "C" void lkpi_thread_release_task(struct thread* td);

namespace {

struct lkpi_thread_rec;

struct sleepq_chain {
	spinlock		lock;
	lkpi_thread_rec*	waiters;	/* singly linked, LIFO */
};

struct lkpi_thread_rec {
	struct thread		td;		/* must be first */
	lkpi_thread_rec*	hashNext;

	/* sleepqueue state; a thread waits on at most one channel */
	const void*		sqWchan;
	lkpi_thread_rec*	sqNext;
	bool			sqWoken;
	bigtime_t		sqTimeout;
	ConditionVariable	sqVar;
	ConditionVariableEntry	sqEntry;

	/* the chain lock this thread holds via sleepq_lock() */
	sleepq_chain*		sqHeld;
	cpu_status		sqIrqState;

	/* reaper (thread has exited): unlinked from the hash, freed later */
	lkpi_thread_rec*	deadNext;
	bigtime_t		deadSince;
};

const int kThreadHashSize = 256;
const int kChainCount = 128;

spinlock sThreadHashLock = B_SPINLOCK_INITIALIZER;
lkpi_thread_rec* sThreadHash[kThreadHashSize];

/*
 * Records of exited threads (protected by sThreadHashLock). They are freed
 * only kRecordGrace after the thread exited: lock-free iterators
 * (FOREACH_THREAD_IN_PROC, lkpi_thread_first/next) may still be standing on
 * a record that was just unlinked, and they only ever hold one for a few
 * instructions. A dead record keeps its hashNext so such an iterator goes on.
 */
const bigtime_t kRecordGrace = 30000000LL;	/* 30 s */
lkpi_thread_rec* sDeadRecords;
int32 sRecordsLive;
int32 sRecordsCreated;
int32 sRecordsFreed;
sleepq_chain sChains[kChainCount];
struct proc sProc0 = { 0, "kernel" };

inline sleepq_chain*
chain_for(const void* wchan)
{
	return &sChains[((uintptr_t)wchan >> 6) % kChainCount];
}

lkpi_thread_rec*
rec_lookup(thread_id id)
{
	InterruptsSpinLocker locker(sThreadHashLock);
	for (lkpi_thread_rec* rec = sThreadHash[id % kThreadHashSize];
			rec != NULL; rec = rec->hashNext) {
		if (rec->td.td_tid == id)
			return rec;
	}
	return NULL;
}

}	// namespace


namespace {

/* The record for Haiku thread 'id', created on first use. */
struct thread*
rec_get(thread_id id)
{
	lkpi_thread_rec* rec = rec_lookup(id);
	if (rec != NULL)
		return &rec->td;

	/* Allocate outside the spinlock. */
	rec = new(std::nothrow) lkpi_thread_rec;
	if (rec == NULL)
		panic("LinuxKPI: out of memory allocating struct thread");
	memset(&rec->td, 0, sizeof(rec->td));
	rec->td.td_tid = id;
	rec->td.td_proc = &sProc0;
	thread_info info;
	if (get_thread_info(id, &info) == B_OK) {
		rec->td.td_priority = info.priority;
		strlcpy(rec->td.td_name, info.name, sizeof(rec->td.td_name));
	}
	rec->sqWchan = NULL;
	rec->sqNext = NULL;
	rec->sqWoken = false;
	rec->sqTimeout = 0;
	rec->sqHeld = NULL;
	rec->deadNext = NULL;
	rec->deadSince = 0;

	InterruptsSpinLocker locker(sThreadHashLock);
	/* Another CPU may have created it meanwhile. */
	for (lkpi_thread_rec* other = sThreadHash[id % kThreadHashSize];
			other != NULL; other = other->hashNext) {
		if (other->td.td_tid == id) {
			locker.Unlock();
			delete rec;
			return &other->td;
		}
	}
	rec->hashNext = sThreadHash[id % kThreadHashSize];
	__atomic_store_n(&sThreadHash[id % kThreadHashSize], rec,
		__ATOMIC_RELEASE);
	sRecordsLive++;
	int32 created = ++sRecordsCreated;
	locker.Unlock();

	if ((created & 0xff) == 0) {
		dprintf("linuxkpi: %" B_PRId32 " thread records created, %" B_PRId32
			" live, %" B_PRId32 " freed\n", created, sRecordsLive,
			sRecordsFreed);
	}
	return &rec->td;
	/* Freed by the reaper below when the Haiku thread exits. */
}

}	// namespace


// #pragma mark - reaper


/*
 * FreeBSD frees a thread's LinuxKPI state from the thread_dtor event
 * (linuxkpi_thread_dtor() in linux_current.c). Haiku has no such event for
 * drivers, but its kernel "threads" notification service reports
 * THREAD_REMOVED from thread_exit(), in the exiting thread's own context
 * (src/system/kernel/thread.cpp). Without this, every thread that ever
 * touched LinuxKPI (for example through kfree(), which reads curthread) left
 * a record and a Linux task_struct behind for good.
 */
namespace {

void
reap_thread(thread_id id)
{
	lkpi_thread_rec* rec = rec_lookup(id);
	if (rec != NULL) {
		/* Linux 'current' first: it may still use the record. */
		lkpi_thread_release_task(&rec->td);
	}

	lkpi_thread_rec* expired = NULL;
	bigtime_t now = system_time();
	{
		InterruptsSpinLocker locker(sThreadHashLock);
		if (rec != NULL) {
			lkpi_thread_rec** pp = &sThreadHash[id % kThreadHashSize];
			for (; *pp != NULL; pp = &(*pp)->hashNext) {
				if (*pp == rec) {
					/* rec->hashNext stays valid for iterators on rec */
					__atomic_store_n(pp, rec->hashNext, __ATOMIC_RELEASE);
					rec->deadSince = now;
					rec->deadNext = sDeadRecords;
					sDeadRecords = rec;
					sRecordsLive--;
					break;
				}
			}
		}

		/* detach records that died more than kRecordGrace ago */
		lkpi_thread_rec** pp = &sDeadRecords;
		while (*pp != NULL) {
			lkpi_thread_rec* dead = *pp;
			if (now - dead->deadSince >= kRecordGrace) {
				*pp = dead->deadNext;
				dead->deadNext = expired;
				expired = dead;
			} else
				pp = &dead->deadNext;
		}
	}

	while (expired != NULL) {
		lkpi_thread_rec* next = expired->deadNext;
		delete expired;
		atomic_add(&sRecordsFreed, 1);
		expired = next;
	}
}


class ThreadReaper : public NotificationListener {
public:
	virtual void EventOccurred(NotificationService& /*service*/,
		const KMessage* event)
	{
		int32 code;
		if (event->FindInt32("event", &code) != B_OK
			|| code != THREAD_REMOVED)
			return;
		/* Use the Thread pointer: ThreadNotificationService::Notify(code,
		 * Thread*) passes team and thread IDs in swapped order. */
		Thread* thread;
		if (event->FindPointer("threadStruct", (void**)&thread) != B_OK
			|| thread == NULL)
			return;
		reap_thread(thread->id);
	}
};

ThreadReaper sReaper;
bool sReaperListening;

}	// namespace


extern "C" void
lkpi_thread_reaper_init(void)
{
	new(&sReaper) ThreadReaper;
	status_t status = NotificationManager::Manager().AddListener("threads",
		THREAD_REMOVED, sReaper);
	sReaperListening = status == B_OK;
	if (!sReaperListening) {
		dprintf("linuxkpi: cannot watch thread exits (%s); thread records "
			"will not be freed\n", strerror(status));
	}
}


extern "C" void
lkpi_thread_reaper_uninit(void)
{
	if (sReaperListening) {
		/* Waits for a notification in progress (service lock). */
		NotificationManager::Manager().RemoveListener("threads", NULL,
			sReaper);
		sReaperListening = false;
	}
	/* The driver is going away: nothing iterates any more. */
	InterruptsSpinLocker locker(sThreadHashLock);
	lkpi_thread_rec* dead = sDeadRecords;
	sDeadRecords = NULL;
	locker.Unlock();
	while (dead != NULL) {
		lkpi_thread_rec* next = dead->deadNext;
		delete dead;
		atomic_add(&sRecordsFreed, 1);
		dead = next;
	}
}


extern "C" void
lkpi_thread_get_stats(int32* live, int32* created, int32* freed)
{
	*live = sRecordsLive;
	*created = sRecordsCreated;
	*freed = sRecordsFreed;
}


extern "C" struct thread*
lkpi_curthread(void)
{
	return rec_get(find_thread(NULL));
}


extern "C" int lkpi_errno_from_haiku(int haikuError);

/* kthread_add(9) for LinuxKPI: the new thread is created stopped (Haiku's
 * spawn_kernel_thread() does that anyway) and *newtdp is its record. */
extern "C" int
lkpi_kthread_add(void (*func)(void*), void* arg, void* /* proc */,
	struct thread** newtdp, int /* flags */, int /* pages */,
	const char* fmt, ...)
{
	char name[B_OS_NAME_LENGTH];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(name, sizeof(name), fmt, ap);
	va_end(ap);

	thread_id id = spawn_kernel_thread((thread_func)func, name,
		B_NORMAL_PRIORITY, arg);
	if (id < 0)
		return lkpi_errno_from_haiku(id);
	if (newtdp != NULL)
		*newtdp = rec_get(id);
	return 0;
}


extern "C" void
lkpi_sched_add(struct thread* td, int /* flags */)
{
	resume_thread(td->td_tid);
}


/* FreeBSD priorities: lower is more important; below PRI_MIN_TIMESHARE (120)
 * are interrupt/realtime classes. Haiku: higher is more important. Map the
 * "raised" FreeBSD classes LinuxKPI uses (PI_SWI(SWI_NET) for kthreads) to
 * normal priority for now (see LKPI_THREAD_PRIORITY, lkpi_haiku_wrap.h). */
extern "C" void
lkpi_sched_prio(struct thread* td, u_char prio)
{
	/* See LKPI_THREAD_PRIORITY in lkpi_haiku_wrap.h. */
	td->td_priority = prio;
	set_thread_priority(td->td_tid, B_NORMAL_PRIORITY);
}


extern "C" void
sched_pin(void)
{
	thread_pin_to_current_cpu(thread_get_current_thread());
}


extern "C" void
sched_unpin(void)
{
	thread_unpin_from_current_cpu(thread_get_current_thread());
}


extern "C" void
sched_relinquish(struct thread*)
{
	thread_yield();
}


extern "C" void
kern_yield(int)
{
	thread_yield();
}


extern "C" int
td_ast_pending(struct thread*, int)
{
	return 0;
}


//	#pragma mark - sleepqueue(9)


static inline lkpi_thread_rec*
cur_rec()
{
	return (lkpi_thread_rec*)lkpi_curthread();
}


extern "C" void
sleepq_lock(const void* wchan)
{
	lkpi_thread_rec* self = cur_rec();
	sleepq_chain* chain = chain_for(wchan);

	cpu_status state = disable_interrupts();
	acquire_spinlock(&chain->lock);
	self->sqHeld = chain;
	self->sqIrqState = state;
}


extern "C" void
sleepq_release(const void* wchan)
{
	lkpi_thread_rec* self = cur_rec();
	sleepq_chain* chain = self->sqHeld;

	self->sqHeld = NULL;
	release_spinlock(&chain->lock);
	restore_interrupts(self->sqIrqState);
}


extern "C" void
sleepq_add(const void* wchan, struct lock_object*, const char* wmesg,
	int flags, int queue)
{
	lkpi_thread_rec* self = cur_rec();
	sleepq_chain* chain = self->sqHeld;

	self->sqWchan = wchan;
	self->sqWoken = false;
	self->sqTimeout = 0;
	self->sqVar.Init(self, wmesg != NULL ? wmesg : "lkpi sleepq");
	self->sqVar.Add(&self->sqEntry);
	self->sqNext = chain->waiters;
	chain->waiters = self;
}


extern "C" void
sleepq_set_timeout(const void* wchan, int timo)
{
	/* timo is in ticks (hz per second) */
	cur_rec()->sqTimeout = (bigtime_t)timo * 1000000 / hz;
}


static void
chain_remove(sleepq_chain* chain, lkpi_thread_rec* rec)
{
	for (lkpi_thread_rec** p = &chain->waiters; *p != NULL;
			p = &(*p)->sqNext) {
		if (*p == rec) {
			*p = rec->sqNext;
			rec->sqNext = NULL;
			return;
		}
	}
}


/* Drop the chain lock, sleep, and return a FreeBSD errno (0, EINTR,
 * EWOULDBLOCK). FreeBSD returns with the chain unlocked too. */
static int
sleepq_do_wait(bool interruptible, bool timed)
{
	lkpi_thread_rec* self = cur_rec();
	sleepq_chain* chain = self->sqHeld;
	uint32 flags = interruptible ? B_CAN_INTERRUPT : 0;
	bigtime_t timeout = 0;

	if (timed) {
		flags |= B_RELATIVE_TIMEOUT;
		timeout = self->sqTimeout;
	}

	self->sqHeld = NULL;
	release_spinlock(&chain->lock);
	restore_interrupts(self->sqIrqState);

	status_t status = self->sqEntry.Wait(flags, timeout);

	cpu_status state = disable_interrupts();
	acquire_spinlock(&chain->lock);
	bool woken = self->sqWoken;
	if (!woken)
		chain_remove(chain, self);
	self->sqWchan = NULL;
	release_spinlock(&chain->lock);
	restore_interrupts(state);

	if (woken || status == B_OK)
		return 0;
	if (status == B_TIMED_OUT || status == B_WOULD_BLOCK)
		return lkpi_errno_from_haiku(EWOULDBLOCK);
	return lkpi_errno_from_haiku(EINTR);
}


extern "C" void
sleepq_wait(const void*, int)
{
	sleepq_do_wait(false, false);
}


extern "C" int
sleepq_wait_sig(const void*, int)
{
	return sleepq_do_wait(true, false);
}


extern "C" int
sleepq_timedwait(const void*, int)
{
	return sleepq_do_wait(false, true);
}


extern "C" int
sleepq_timedwait_sig(const void*, int)
{
	return sleepq_do_wait(true, true);
}


/* Caller holds the chain lock (sleepq_lock()); FreeBSD semantics. */
static int
sleepq_wake(const void* wchan, bool all)
{
	sleepq_chain* chain = chain_for(wchan);
	lkpi_thread_rec** p = &chain->waiters;

	while (*p != NULL) {
		lkpi_thread_rec* rec = *p;
		if (rec->sqWchan != wchan || rec->sqWoken) {
			p = &rec->sqNext;
			continue;
		}
		*p = rec->sqNext;
		rec->sqNext = NULL;
		rec->sqWoken = true;
		rec->sqVar.NotifyOne();
		if (!all)
			break;
	}
	return 0;	/* FreeBSD: "need to kick swapper" — never on Haiku */
}


extern "C" int
sleepq_signal(const void* wchan, int flags, int pri, int queue)
{
	return sleepq_wake(wchan, false);
}


extern "C" int
sleepq_broadcast(const void* wchan, int flags, int pri, int queue)
{
	return sleepq_wake(wchan, true);
}


extern "C" void
lkpi_sleepq_init(void)
{
	for (int i = 0; i < kChainCount; i++) {
		B_INITIALIZE_SPINLOCK(&sChains[i].lock);
		sChains[i].waiters = NULL;
	}
}


extern "C" void
tdsignal(struct thread* td, int sig)
{
	send_signal_etc(td->td_tid, sig, B_DO_NOT_RESCHEDULE);
}


//	#pragma mark - process model (include/sys/proc.h)


extern "C" struct proc*
lkpi_proc0(void)
{
	return &sProc0;
}


/* Iteration is lock-free: records are prepended to a hash chain (next
 * pointer written before publication); an exited thread's record is
 * unlinked but keeps its next pointer and is freed only kRecordGrace later
 * (reaper above). */
static struct thread*
first_from_bucket(int bucket)
{
	for (; bucket < kThreadHashSize; bucket++) {
		lkpi_thread_rec* rec = __atomic_load_n(&sThreadHash[bucket], __ATOMIC_ACQUIRE);
		if (rec != NULL)
			return &rec->td;
	}
	return NULL;
}


extern "C" struct thread*
lkpi_thread_first(struct proc*)
{
	return first_from_bucket(0);
}


extern "C" struct thread*
lkpi_thread_next(struct thread* td)
{
	lkpi_thread_rec* rec = (lkpi_thread_rec*)td;
	if (rec->hashNext != NULL)
		return &rec->hashNext->td;
	return first_from_bucket(rec->td.td_tid % kThreadHashSize + 1);
}


extern "C" struct thread*
tdfind(int32_t tid, pid_t)
{
	lkpi_thread_rec* rec = rec_lookup(tid);
	return rec != NULL ? &rec->td : NULL;
}


extern "C" struct proc*
pfind(pid_t)
{
	return NULL;
}


extern "C" void
thread_reap_barrier(void)
{
}


extern "C" bool
lkpi_can_sleep(void)
{
	return are_interrupts_enabled();
}


// #pragma mark - jiffies


/* linux/jiffies.h: "extern unsigned long jiffies;". freebsd_network's hz is
 * 1000, so one jiffy is one millisecond of system_time(). */
extern "C" {
unsigned long jiffies;
}

namespace {

timer sJiffiesTimer;
bool sJiffiesTimerActive;

int32
jiffies_tick(timer*)
{
	jiffies = (unsigned long)(system_time() / 1000);
	return B_HANDLED_INTERRUPT;
}

}	// namespace


extern "C" void
lkpi_jiffies_init(void)
{
	jiffies = (unsigned long)(system_time() / 1000);
	if (add_timer(&sJiffiesTimer, jiffies_tick, 1000, B_PERIODIC_TIMER)
			== B_OK)
		sJiffiesTimerActive = true;
	else
		panic("LinuxKPI: cannot start the jiffies timer");
}


extern "C" void
lkpi_jiffies_uninit(void)
{
	if (sJiffiesTimerActive) {
		cancel_timer(&sJiffiesTimer);
		sJiffiesTimerActive = false;
	}
}


/* rtprio(2) for LinuxKPI's sched_set_fifo[_low]() (mt76 worker threads).
 * Haiku has no real-time FIFO class that is safe for a busy driver thread;
 * use normal priority, like lkpi_sched_prio(). (sys/rtprio.h
 * declares it without extern "C", so it is not included here.) */
struct rtprio;
extern "C" int
rtp_to_pri(struct rtprio* rtp, struct thread* td)
{
	if (td == NULL)
		return EINVAL;
	set_thread_priority(td->td_tid, B_NORMAL_PRIORITY);
	return 0;
}
