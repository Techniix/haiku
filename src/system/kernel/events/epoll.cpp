/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	epoll: Linux's scalable I/O event notification (epoll(7)). The user API
	lives in libgnu (headers/compatibility/gnu/sys/epoll.h); this is the
	kernel object behind it.

	Why not Haiku's event_queue directly
	------------------------------------
	Haiku already has a kqueue-like object, the event_queue
	(event_queue.cpp, _kern_event_queue_*). It is built on the generic
	select machinery: a select_sync subclass whose select_info records are
	handed to the objects' fd_select() hooks, which notify them when the
	object becomes ready. epoll uses exactly that machinery too, but cannot
	reuse the event_queue's bookkeeping, because the two key their
	registrations differently:

	 - The event_queue registers (fd number, current team) through
	   select_fd(), which hangs the select_info on the team's fd slot. A
	   registration therefore vanishes when that slot is closed, and level
	   triggering re-selects by fd number in the *calling* team. After
	   fork() the child calls epoll_wait() on the shared queue; deselecting
	   by fd number in the child's I/O context misses the parent's slot
	   entry, and the select_info would be freed while still in the
	   object's select pool.
	 - Linux keys an epoll registration on the open file description (plus
	   the fd number it was added under). It survives close() of that fd
	   while a dup()ed or inherited descriptor keeps the description open,
	   and is removed when the last reference goes away; and an epoll
	   instance is shared, registrations and all, across fork().

	So each EpollItem here is a select_info registered directly with the
	file_descriptor's fd_select()/fd_deselect() hooks (no fd slot), and
	every file_descriptor carries a list of the items watching it
	(file_descriptor::epoll_items). close_fd() calls
	epoll_descriptor_closed() when the open count drops to zero, before the
	descriptor's fd_close() hook, and that removes the items. The object
	notifications, select pools and select_sync reference counting are the
	same ones poll(), select() and the event_queue use.

	Readiness and triggering
	------------------------
	Haiku's select hooks are edge-like: an object notifies when it becomes
	ready (and immediately during fd_select() if it already is), but not
	again while it stays ready. Linux epoll asks each ready file for its
	*current* state when it reports it (f_op->poll()). The equivalent here
	is Probe(): deselect the item's events, clear its pending events and
	select them again; fd_select() then notifies whatever holds right now.

	 - Notify() (called by the objects) ORs the events into the item and
	   puts it on the ready list, waking epoll_wait() and anyone polling
	   the epoll descriptor itself.
	 - epoll_wait() takes the ready list, probes every item on it and
	   reports those that are still ready. Level-triggered items that were
	   reported stay on the ready list (the probe put them back at its
	   end, so delivery is round-robin), and are probed again by the next
	   call, which drops them once they are no longer ready. Edge-triggered
	   items are taken off and come back only with the next notification.
	   EPOLLONESHOT items are disarmed (deselected) until EPOLL_CTL_MOD.
	 - Polling the epoll descriptor (poll(), select(), or nesting it in
	   another epoll) probes the ready list the same way without reporting,
	   so it is readable exactly when epoll_wait() would return something.

	Locking
	-------
	 - sEpollLock (global mutex): every descriptor's epoll_items list, and
	   membership of items in epoll instances (ADD, DEL, close of either
	   side). Held across fd_select()/fd_deselect() for ADD/MOD/DEL.
	 - Epoll::fControlLock: serializes probing and arming of this
	   instance's items (epoll_wait(), poll of the epoll fd, MOD).
	 - Epoll::fReadyLock: the ready list, pending events, waiters and the
	   instance's own select pool. Taken by Notify(), i.e. under the
	   notifying object's own lock, so nothing may be selected or
	   deselected while it is held.
	 Order: sEpollLock -> fControlLock -> object locks -> fReadyLock. For
	 nested instances, outer before inner; EPOLL_CTL_ADD rejects loops
	 (ELOOP) and chains deeper than Linux's limit, so the order is acyclic.

	A close of the descriptor while ADD is linking an item is resolved
	without the global lock on the close path: ADD publishes the item in
	the descriptor's list and then re-reads the open count, close_fd()
	drops the open count and then reads the list, both with full barriers,
	so at least one of them sees the other.
*/


#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <new>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>

#include <AutoDeleter.h>
#include <AutoDeleterDrivers.h>
#include <condition_variable.h>
#include <epoll.h>
#include <fs/fd.h>
#include <fs/select_sync_pool.h>
#include <kernel.h>
#include <lock.h>
#include <signal.h>
#include <syscalls.h>
#include <thread.h>
#include <util/atomic.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <vfs.h>
#include <wait_for_objects.h>

#include "select_sync.h"

// The user ABI (event bits, struct epoll_event layout) is the Linux one,
// defined once in the libgnu header. It only needs <fcntl.h>, <signal.h>,
// <stdint.h> and <time.h>, which the kernel has; the gnu header directory
// is not on the kernel's include path, since its other headers would
// shadow system ones.
#include "../../../../headers/compatibility/gnu/sys/epoll.h"


//#define TRACE_EPOLL
#ifdef TRACE_EPOLL
#	define TRACE(x...) dprintf("epoll: " x)
#else
#	define TRACE(x...) do {} while (false)
#endif


// Linux allows epoll instances nested this deep (EP_MAX_NESTS).
static const int kMaxNesting = 4;

// Events reported in one epoll_wait() call at most. Returning fewer events
// than maxevents is always allowed; the rest come with the next call.
static const int kMaxEventsPerWait = 1024;

static const uint32 kValidEvents = EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR
	| EPOLLHUP | EPOLLRDNORM | EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND
	| EPOLLMSG | EPOLLRDHUP | EPOLLEXCLUSIVE | EPOLLWAKEUP | EPOLLONESHOT
	| EPOLLET;

// EPOLLEXCLUSIVE may only be combined with these (Linux EPOLLEXCLUSIVE_OK_BITS).
static const uint32 kExclusiveOkEvents = EPOLLIN | EPOLLOUT | EPOLLERR
	| EPOLLHUP | EPOLLWAKEUP | EPOLLET | EPOLLEXCLUSIVE;


class Epoll;


struct EpollItem : select_info {
	Epoll*					epoll;
	file_descriptor*		descriptor;
	int						fd;
		// fd number it was added under; with descriptor the lookup key

	uint32					requested;
		// EPOLL* mask from the last ADD/MOD, flags included
	uint64					data;

	uint16					interest;
		// B_EVENT_* bits to select (0 while a one-shot item is disarmed)
	uint16					selected;
		// B_EVENT_* bits whose fd_select() succeeded (to be deselected)
	uint16					pending;
		// B_EVENT_* bits notified since the last probe (fReadyLock)
	bool					onReadyList;
		// (fReadyLock)

	EpollItem*				nextOnDescriptor;
		// in descriptor->epoll_items (sEpollLock)

	DoublyLinkedListLink<EpollItem>	itemLink;
		// in Epoll::fItems (sEpollLock, fControlLock)
	DoublyLinkedListLink<EpollItem>	readyLink;
		// in Epoll::fReadyList (fReadyLock)
};


typedef DoublyLinkedList<EpollItem,
	DoublyLinkedListMemberGetLink<EpollItem, &EpollItem::itemLink> >
	EpollItemList;
typedef DoublyLinkedList<EpollItem,
	DoublyLinkedListMemberGetLink<EpollItem, &EpollItem::readyLink> >
	EpollReadyList;


static mutex sEpollLock = MUTEX_INITIALIZER("epoll");


static uint16
haiku_events_for(uint32 events)
{
	// Errors and disconnects are always reported, as on Linux.
	uint16 result = B_EVENT_ERROR | B_EVENT_DISCONNECTED;
	if ((events & (EPOLLIN | EPOLLRDNORM)) != 0)
		result |= B_EVENT_READ;
	if ((events & (EPOLLOUT | EPOLLWRNORM)) != 0)
		result |= B_EVENT_WRITE;
	if ((events & EPOLLPRI) != 0)
		result |= B_EVENT_HIGH_PRIORITY_READ;
	if ((events & EPOLLRDBAND) != 0)
		result |= B_EVENT_PRIORITY_READ;
	if ((events & EPOLLWRBAND) != 0)
		result |= B_EVENT_PRIORITY_WRITE;
	if ((events & EPOLLRDHUP) != 0)
		result |= SELECT_FLAG(B_SELECT_READ_DISCONNECTED);
	return result;
}


static uint32
epoll_events_for(uint16 events, uint32 requested)
{
	uint32 result = 0;
	if ((events & B_EVENT_READ) != 0)
		result |= requested & (EPOLLIN | EPOLLRDNORM);
	if ((events & B_EVENT_WRITE) != 0)
		result |= requested & (EPOLLOUT | EPOLLWRNORM);
	if ((events & B_EVENT_HIGH_PRIORITY_READ) != 0)
		result |= requested & EPOLLPRI;
	if ((events & B_EVENT_PRIORITY_READ) != 0)
		result |= requested & EPOLLRDBAND;
	if ((events & B_EVENT_PRIORITY_WRITE) != 0)
		result |= requested & EPOLLWRBAND;
	if ((events & B_EVENT_ERROR) != 0)
		result |= EPOLLERR;
	if ((events & SELECT_FLAG(B_SELECT_READ_DISCONNECTED)) != 0) {
		// The peer shut down its sending side (stream sockets).
		result |= requested & EPOLLRDHUP;
	}
	if ((events & B_EVENT_DISCONNECTED) != 0) {
		// A full disconnect implies the peer's side is shut too, as on
		// Linux (objects other than sockets only report this one).
		result |= EPOLLHUP | (requested & EPOLLRDHUP);
	}
	return result;
}


class Epoll : public select_sync {
public:
								Epoll();
	virtual						~Epoll();

	virtual	status_t			Notify(select_info* info, uint16 events);

			// fd hooks
			void				Close();
			status_t			SelectSelf(uint8 event, selectsync* sync);
			status_t			DeselectSelf(uint8 event, selectsync* sync);

			// epoll_ctl() (sEpollLock held)
			status_t			Add(file_descriptor* descriptor, int fd,
									const epoll_event& event);
			status_t			Modify(EpollItem* item,
									const epoll_event& event);
			void				Remove(EpollItem* item);

			ssize_t				Wait(epoll_event* events, int maxEvents,
									bigtime_t deadline);

			bool				IsClosing() const { return fClosing; }
			bool				ContainsDescriptor(
									file_descriptor* descriptor) const;
			const EpollItemList& Items() const { return fItems; }

private:
			void				_Arm(EpollItem* item);
			void				_Disarm(EpollItem* item);
			uint16				_Probe(EpollItem* item);
			void				_TakeOffReadyList(EpollItem* item);
			int					_Scan(epoll_event* events, int maxEvents);
			void				_Unlink(EpollItem* item);

private:
			mutex				fControlLock;
			mutex				fReadyLock;
			ConditionVariable	fCondition;
			EpollItemList		fItems;
			EpollReadyList		fReadyList;
			select_sync_pool*	fSelectPool;
			bool				fClosing;
};


extern struct fd_ops sEpollOps;


static inline Epoll*
epoll_for(file_descriptor* descriptor)
{
	return descriptor->ops == &sEpollOps ? (Epoll*)descriptor->cookie : NULL;
}


Epoll::Epoll()
	:
	fSelectPool(NULL),
	fClosing(false)
{
	mutex_init(&fControlLock, "epoll control");
	mutex_init(&fReadyLock, "epoll ready");
	fCondition.Init(this, "epoll wait");
}


Epoll::~Epoll()
{
	ASSERT(fItems.IsEmpty());
	delete_select_sync_pool(fSelectPool);
	mutex_destroy(&fControlLock);
	mutex_destroy(&fReadyLock);
}


/*!	Called by the watched objects, with their own lock held. */
status_t
Epoll::Notify(select_info* info, uint16 events)
{
	EpollItem* item = static_cast<EpollItem*>(info);

	MutexLocker locker(fReadyLock);

	events &= item->interest;
	if (events == 0)
		return B_OK;

	item->pending |= events;
	if (!item->onReadyList) {
		item->onReadyList = true;
		fReadyList.Add(item);
		fCondition.NotifyAll();
		notify_select_event_pool(fSelectPool, B_SELECT_READ);
	}
	return B_OK;
}


/*!	Selects the item's interest events with its descriptor. fControlLock
	must be held, fReadyLock must not.
*/
void
Epoll::_Arm(EpollItem* item)
{
	file_descriptor* descriptor = item->descriptor;
	item->selected = 0;
	if (descriptor->ops->fd_select == NULL)
		return;

	for (uint16 event = 1; event < 16; event++) {
		if ((item->interest & SELECT_FLAG(event)) == 0)
			continue;
		if (descriptor->ops->fd_select(descriptor, event,
				(selectsync*)item) == B_OK) {
			item->selected |= SELECT_FLAG(event);
		}
	}
}


/*!	Deselects whatever _Arm() selected. After it returns, the object will
	not call Notify() for this item anymore.
*/
void
Epoll::_Disarm(EpollItem* item)
{
	file_descriptor* descriptor = item->descriptor;
	if (descriptor->ops->fd_deselect != NULL) {
		for (uint16 event = 1; event < 16; event++) {
			if ((item->selected & SELECT_FLAG(event)) != 0) {
				descriptor->ops->fd_deselect(descriptor, event,
					(selectsync*)item);
			}
		}
	}
	item->selected = 0;
}


void
Epoll::_TakeOffReadyList(EpollItem* item)
{
	MutexLocker locker(fReadyLock);
	item->pending = 0;
	if (item->onReadyList) {
		fReadyList.Remove(item);
		item->onReadyList = false;
	}
}


/*!	Returns the events that hold for the item right now (B_EVENT_* bits).
	If any do, the item is on the ready list afterwards.
*/
uint16
Epoll::_Probe(EpollItem* item)
{
	if (item->interest == 0)
		return 0;

	_Disarm(item);
	{
		MutexLocker locker(fReadyLock);
		item->pending = 0;
	}
	_Arm(item);

	MutexLocker locker(fReadyLock);
	return item->pending;
}


/*!	Probes the items on the ready list. With \a events, reports up to
	\a maxEvents of those that are ready and applies the edge-triggered and
	one-shot rules to them; without, only drops the items that are not
	ready anymore. Returns the number of ready (reported) items.
	fControlLock must be held.
*/
int
Epoll::_Scan(epoll_event* events, int maxEvents)
{
	// Items on the local list keep onReadyList set, so that Notify() only
	// adds to their pending events and does not link them anywhere else;
	// each one is unmarked when it is taken off the local list.
	EpollReadyList todo;
	{
		MutexLocker locker(fReadyLock);
		todo.TakeFrom(&fReadyList);
	}

	int count = 0;
	while (true) {
		MutexLocker locker(fReadyLock);
		if (count == maxEvents) {
			// Out of room: the rest goes back to the front of the ready
			// list, in order.
			EpollItem* front = fReadyList.Head();
			while (EpollItem* item = todo.RemoveHead())
				fReadyList.InsertBefore(front, item);
			break;
		}

		EpollItem* item = todo.RemoveHead();
		if (item == NULL)
			break;
		item->onReadyList = false;
		locker.Unlock();

		uint16 ready = _Probe(item);
		if (ready == 0)
			continue;

		if (events == NULL) {
			// Only looking: the probe left the item on the ready list.
			count++;
			continue;
		}

		events[count].events = epoll_events_for(ready, item->requested);
		events[count].data.u64 = item->data;
		count++;

		if ((item->requested & EPOLLONESHOT) != 0) {
			_Disarm(item);
			item->interest = 0;
			_TakeOffReadyList(item);
		} else if ((item->requested & EPOLLET) != 0) {
			// Edge-triggered: back only with the next notification.
			_TakeOffReadyList(item);
		}
		// Level-triggered items stay on the ready list (at its end, where
		// the probe put them) and are probed again next time.
	}

	return count;
}


status_t
Epoll::Add(file_descriptor* descriptor, int fd, const epoll_event& event)
{
	EpollItem* item = new(std::nothrow) EpollItem;
	if (item == NULL)
		return B_NO_MEMORY;

	item->next = NULL;
	item->sync = this;
	item->events = 0;
	item->selected_events = 0;
	item->epoll = this;
	item->descriptor = descriptor;
	item->fd = fd;
	item->requested = event.events;
	item->data = event.data.u64;
	item->interest = haiku_events_for(event.events);
	item->selected = 0;
	item->pending = 0;
	item->onReadyList = false;

	// Publish the item on the descriptor, then check that the descriptor is
	// still open (see "Locking" above for the race with close_fd()).
	item->nextOnDescriptor = (EpollItem*)descriptor->epoll_items;
	atomic_pointer_set((EpollItem**)&descriptor->epoll_items, item);
	if (atomic_add(&descriptor->open_count, 0) <= 0) {
		descriptor->epoll_items = item->nextOnDescriptor;
		delete item;
		return B_FILE_ERROR;
	}

	MutexLocker locker(fControlLock);
	fItems.Add(item);
	_Arm(item);

	TRACE("%p: added %p (fd %d), events %#" B_PRIx32 "\n", this, descriptor,
		fd, event.events);
	return B_OK;
}


status_t
Epoll::Modify(EpollItem* item, const epoll_event& event)
{
	MutexLocker locker(fControlLock);

	_Disarm(item);
	_TakeOffReadyList(item);

	item->requested = event.events;
	item->data = event.data.u64;
	item->interest = haiku_events_for(event.events);

	// Selecting again also reports the current state, as Linux does on
	// EPOLL_CTL_MOD (and re-arms a disarmed one-shot item).
	_Arm(item);
	return B_OK;
}


/*!	Removes \a item from its descriptor's list. sEpollLock must be held. */
void
Epoll::_Unlink(EpollItem* item)
{
	EpollItem** link = (EpollItem**)&item->descriptor->epoll_items;
	while (*link != NULL && *link != item)
		link = &(*link)->nextOnDescriptor;
	if (*link == item)
		*link = item->nextOnDescriptor;
}


/*!	Removes and deletes \a item. sEpollLock must be held. */
void
Epoll::Remove(EpollItem* item)
{
	_Unlink(item);

	MutexLocker locker(fControlLock);
	fItems.Remove(item);
	_Disarm(item);
	_TakeOffReadyList(item);
	locker.Unlock();

	delete item;
}


bool
Epoll::ContainsDescriptor(file_descriptor* descriptor) const
{
	for (EpollItemList::ConstIterator it = fItems.GetIterator();
			it.HasNext();) {
		if (it.Next()->descriptor == descriptor)
			return true;
	}
	return false;
}


ssize_t
Epoll::Wait(epoll_event* events, int maxEvents, bigtime_t deadline)
{
	while (true) {
		MutexLocker controlLocker(fControlLock);
		if (fClosing)
			return B_FILE_ERROR;
		int count = _Scan(events, maxEvents);
		controlLocker.Unlock();

		if (count > 0)
			return count;
		if (deadline == 0)
			return 0;

		MutexLocker readyLocker(fReadyLock);
		if (fClosing)
			return B_FILE_ERROR;
		if (!fReadyList.IsEmpty())
			continue;

		status_t status;
		if (deadline < 0)
			status = fCondition.Wait(&fReadyLock, B_CAN_INTERRUPT);
		else {
			status = fCondition.Wait(&fReadyLock,
				B_CAN_INTERRUPT | B_ABSOLUTE_TIMEOUT, deadline);
		}
		if (fClosing)
			return B_FILE_ERROR;
		if (status == B_TIMED_OUT || status == B_WOULD_BLOCK)
			return 0;
		if (status != B_OK)
			return status;
	}
}


/*!	The epoll descriptor is readable while epoll_wait() would report an
	event; it is never writable.
*/
status_t
Epoll::SelectSelf(uint8 event, selectsync* sync)
{
	{
		MutexLocker locker(fReadyLock);
		status_t status = add_select_sync_pool_entry(&fSelectPool, sync,
			event);
		if (status != B_OK)
			return status;
	}

	if (event != B_SELECT_READ)
		return B_OK;

	MutexLocker controlLocker(fControlLock);
	if (fClosing)
		return B_OK;
	if (_Scan(NULL, INT_MAX) > 0)
		return notify_select_event(sync, event);
	return B_OK;
}


status_t
Epoll::DeselectSelf(uint8 event, selectsync* sync)
{
	MutexLocker locker(fReadyLock);
	remove_select_sync_pool_entry(&fSelectPool, sync, event);
	return B_OK;
}


/*!	The last close of the epoll descriptor: all registrations go. */
void
Epoll::Close()
{
	MutexLocker globalLocker(sEpollLock);
	{
		MutexLocker locker(fControlLock);
		fClosing = true;
	}

	while (EpollItem* item = fItems.Head())
		Remove(item);
	globalLocker.Unlock();

	MutexLocker locker(fReadyLock);
	fCondition.NotifyAll(B_FILE_ERROR);
}


//	#pragma mark - fd hooks


static status_t
epoll_close(file_descriptor* descriptor)
{
	((Epoll*)descriptor->cookie)->Close();
	return B_OK;
}


static void
epoll_free(file_descriptor* descriptor)
{
	put_select_sync((Epoll*)descriptor->cookie);
}


static status_t
epoll_select(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	return ((Epoll*)descriptor->cookie)->SelectSelf(event, sync);
}


static status_t
epoll_deselect(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	return ((Epoll*)descriptor->cookie)->DeselectSelf(event, sync);
}


static status_t
epoll_read_stat(file_descriptor* descriptor, struct stat* stat)
{
	// Linux reports an anonymous inode; like eventfd, look like a FIFO.
	memset(stat, 0, sizeof(*stat));
	stat->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	stat->st_nlink = 1;
	return B_OK;
}


static status_t
epoll_set_flags(file_descriptor* descriptor, int flags)
{
	return B_OK;
}


struct fd_ops sEpollOps = {
	&epoll_close,
	&epoll_free,
	NULL,	// read
	NULL,	// write
	NULL,	// readv
	NULL,	// writev
	NULL,	// seek
	NULL,	// ioctl
	&epoll_set_flags,
	&epoll_select,
	&epoll_deselect,
	NULL,	// read_dir
	NULL,	// rewind_dir
	&epoll_read_stat,
	NULL,	// write_stat
};


//	#pragma mark - kernel private


/*!	Called by close_fd() (and for disconnected descriptors by put_fd())
	when the last open reference to \a descriptor goes away, before its
	fd_close() hook: removes it from every epoll instance watching it.
*/
void
epoll_descriptor_closed(file_descriptor* descriptor)
{
	if (atomic_pointer_get((EpollItem**)&descriptor->epoll_items) == NULL)
		return;

	MutexLocker locker(sEpollLock);
	while (EpollItem* item = (EpollItem*)descriptor->epoll_items)
		item->epoll->Remove(item);
}


//	#pragma mark - syscalls


static status_t
get_epoll(int epfd, file_descriptor*& descriptor, Epoll*& epoll)
{
	descriptor = get_fd(get_current_io_context(false), epfd);
	if (descriptor == NULL)
		return B_FILE_ERROR;

	epoll = epoll_for(descriptor);
	if (epoll == NULL) {
		put_fd(descriptor);
		return B_BAD_VALUE;
	}
	return B_OK;
}


/*!	Whether \a target can be reached from \a from through nested epoll
	instances, or the nesting below \a from is deeper than \a depthLeft.
	sEpollLock must be held.
*/
static bool
epoll_reaches(Epoll* from, Epoll* target, int depthLeft)
{
	if (from == target || depthLeft <= 0)
		return true;

	for (EpollItemList::ConstIterator it = from->Items().GetIterator();
			it.HasNext();) {
		Epoll* inner = epoll_for(it.Next()->descriptor);
		if (inner != NULL && epoll_reaches(inner, target, depthLeft - 1))
			return true;
	}
	return false;
}


int
_user_epoll_create(int openFlags)
{
	if ((openFlags & ~(O_CLOEXEC | O_CLOFORK)) != 0)
		return B_BAD_VALUE;

	Epoll* epoll = new(std::nothrow) Epoll;
	if (epoll == NULL)
		return B_NO_MEMORY;

	file_descriptor* descriptor = alloc_fd();
	if (descriptor == NULL) {
		delete epoll;
		return B_NO_MEMORY;
	}

	descriptor->ops = &sEpollOps;
	descriptor->cookie = epoll;
	descriptor->open_mode = O_RDWR;

	io_context* context = get_current_io_context(false);
	rw_lock_write_lock(&context->lock);
	int fd = new_fd(context, descriptor);
	if (fd < 0) {
		rw_lock_write_unlock(&context->lock);
		descriptor->ops = NULL;
		descriptor->cookie = NULL;
		put_fd(descriptor);
		delete epoll;
		return fd;
	}

	fd_set_close_on_exec(context, fd, (openFlags & O_CLOEXEC) != 0);
	fd_set_close_on_fork(context, fd, (openFlags & O_CLOFORK) != 0);
	rw_lock_write_unlock(&context->lock);

	return fd;
}


status_t
_user_epoll_ctl(int epfd, int op, int fd, const struct epoll_event* userEvent)
{
	epoll_event event = {};
	if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
		if (userEvent == NULL || !IS_USER_ADDRESS(userEvent)
			|| user_memcpy(&event, userEvent, sizeof(event)) != B_OK) {
			return B_BAD_ADDRESS;
		}
	} else if (op != EPOLL_CTL_DEL)
		return B_BAD_VALUE;

	file_descriptor* epollDescriptor;
	Epoll* epoll;
	status_t status = get_epoll(epfd, epollDescriptor, epoll);
	if (status != B_OK)
		return status;
	FileDescriptorPutter epollPutter(epollDescriptor);

	FileDescriptorPutter descriptor(get_fd(get_current_io_context(false), fd));
	if (!descriptor.IsSet())
		return B_FILE_ERROR;
	if (descriptor.Get() == epollDescriptor)
		return B_BAD_VALUE;

	Epoll* target = epoll_for(descriptor.Get());

	if (op != EPOLL_CTL_DEL) {
		// Linux ignores bits it does not know; so do we.
		event.events &= kValidEvents;
		if ((event.events & EPOLLEXCLUSIVE) != 0) {
			// As Linux: only on ADD, only with a few events, and not for
			// epoll descriptors.
			if (op == EPOLL_CTL_MOD || target != NULL
				|| (event.events & ~kExclusiveOkEvents) != 0) {
				return B_BAD_VALUE;
			}
		}
	}

	if (op == EPOLL_CTL_ADD && target == NULL) {
		// Like Linux, refuse what is always ready and cannot be waited for:
		// regular files and directories, and objects without select().
		struct stat st;
		if (descriptor->ops->fd_select == NULL)
			return B_NOT_ALLOWED;
		if (descriptor->ops->fd_read_stat != NULL
			&& descriptor->ops->fd_read_stat(descriptor.Get(), &st) == B_OK
			&& (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) {
			return B_NOT_ALLOWED;
		}
	}

	MutexLocker locker(sEpollLock);
	if (epoll->IsClosing())
		return B_FILE_ERROR;

	EpollItem* item = NULL;
	for (EpollItem* candidate = (EpollItem*)descriptor->epoll_items;
			candidate != NULL; candidate = candidate->nextOnDescriptor) {
		if (candidate->epoll == epoll && candidate->fd == fd) {
			item = candidate;
			break;
		}
	}

	switch (op) {
		case EPOLL_CTL_ADD:
			if (item != NULL)
				return EEXIST;
			if (target != NULL) {
				// No loops, and no deeper nesting than Linux allows.
				if (epoll_reaches(target, epoll, kMaxNesting))
					return ELOOP;
			}
			return epoll->Add(descriptor.Get(), fd, event);

		case EPOLL_CTL_MOD:
			if (item == NULL)
				return B_ENTRY_NOT_FOUND;
			if ((item->requested & EPOLLEXCLUSIVE) != 0)
				return B_BAD_VALUE;
			return epoll->Modify(item, event);

		case EPOLL_CTL_DEL:
			if (item == NULL)
				return B_ENTRY_NOT_FOUND;
			epoll->Remove(item);
			return B_OK;
	}

	return B_BAD_VALUE;
}


ssize_t
_user_epoll_wait(int epfd, struct epoll_event* userEvents, int maxEvents,
	bigtime_t timeout, const sigset_t* userSigMask)
{
	if (maxEvents <= 0 || (size_t)maxEvents > INT_MAX / sizeof(epoll_event))
		return B_BAD_VALUE;
	if (userEvents == NULL || !IS_USER_ADDRESS(userEvents))
		return B_BAD_ADDRESS;

	sigset_t sigMask;
	if (userSigMask != NULL
		&& (!IS_USER_ADDRESS(userSigMask)
			|| user_memcpy(&sigMask, userSigMask, sizeof(sigMask)) != B_OK)) {
		return B_BAD_ADDRESS;
	}

	// Relative timeout to absolute deadline; -1 (any negative) waits
	// forever, 0 does not wait.
	bigtime_t deadline = timeout;
	if (timeout > 0) {
		deadline = system_time() + timeout;
		if (deadline < 0)
			deadline = -1;
	} else if (timeout < 0)
		deadline = -1;

	file_descriptor* descriptor;
	Epoll* epoll;
	status_t status = get_epoll(epfd, descriptor, epoll);
	if (status != B_OK)
		return status;
	FileDescriptorPutter putter(descriptor);

	int count = min_c(maxEvents, kMaxEventsPerWait);
	epoll_event* events = (epoll_event*)malloc(count * sizeof(epoll_event));
	if (events == NULL)
		return B_NO_MEMORY;
	MemoryDeleter eventsDeleter(events);

	// epoll_pwait(): swap the signal mask for the wait, like ppoll(). The
	// old mask is restored on the way back to userland, after any signal
	// that the temporary mask let through has been handled.
	if (userSigMask != NULL) {
		sigset_t oldSigMask;
		sigprocmask(SIG_SETMASK, &sigMask, &oldSigMask);
		Thread* thread = thread_get_current_thread();
		thread->old_sig_block_mask = oldSigMask;
		thread->flags |= THREAD_FLAGS_OLD_SIGMASK;
	}

	// Never restarted after a signal handler (EINTR even with SA_RESTART),
	// as on Linux: we do not set THREAD_FLAGS_RESTART_SYSCALL.
	ssize_t result = epoll->Wait(events, count, deadline);
	if (result > 0
		&& user_memcpy(userEvents, events, result * sizeof(epoll_event))
			!= B_OK) {
		return B_BAD_ADDRESS;
	}
	return result;
}
