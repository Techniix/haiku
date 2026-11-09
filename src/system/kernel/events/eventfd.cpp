/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	eventfd: a file descriptor that carries a 64-bit counter, as on Linux and
	FreeBSD 13+ (eventfd(2)). The user API lives in libgnu
	(headers/compatibility/gnu/sys/eventfd.h); this is the kernel object
	behind it.

	Semantics, as specified by Linux and implemented by FreeBSD:
	 - read() needs an 8-byte buffer. It blocks while the counter is 0
	   (EAGAIN with O_NONBLOCK). It returns the counter and resets it to 0,
	   or, in semaphore mode, returns 1 and decrements it.
	 - write() takes an 8-byte value and adds it to the counter. The value
	   0xffffffffffffffff is invalid. If the sum would exceed
	   0xfffffffffffffffe, it blocks (EAGAIN with O_NONBLOCK) until a read
	   makes room.
	 - The descriptor is readable while the counter is > 0, and writable
	   while at least 1 can be added without blocking.

	Many event loops (glib, libuv, Qt, Chromium, Wine, PipeWire) use eventfd
	for cross-thread wakeups, and a Linux compatibility layer needs it as a
	native object as well.
*/


#include <fcntl.h>
#include <new>
#include <string.h>
#include <sys/stat.h>

#include <condition_variable.h>
#include <eventfd.h>
#include <fs/fd.h>
#include <fs/select_sync_pool.h>
#include <kernel.h>
#include <lock.h>
#include <syscalls.h>
#include <syscall_restart.h>
#include <util/AutoLock.h>
#include <vfs.h>
#include <wait_for_objects.h>


static const uint64 kMaxCounter = 0xfffffffffffffffeULL;


struct EventFD {
	mutex				lock;
	ConditionVariable	condition;
	uint64				counter;
	bool				semaphore;
	select_sync_pool*	selectPool;

	EventFD(uint64 initialValue, bool semaphore)
		:
		counter(initialValue),
		semaphore(semaphore),
		selectPool(NULL)
	{
		mutex_init(&lock, "eventfd");
		condition.Init(this, "eventfd");
	}

	~EventFD()
	{
		delete_select_sync_pool(selectPool);
		mutex_destroy(&lock);
	}

	//! Wakes up waiters and select()ers after the counter changed.
	void Notify()
	{
		condition.NotifyAll();
		if (counter > 0)
			notify_select_event_pool(selectPool, B_SELECT_READ);
		if (counter < kMaxCounter)
			notify_select_event_pool(selectPool, B_SELECT_WRITE);
	}
};


static status_t
eventfd_close(file_descriptor* descriptor)
{
	return B_OK;
}


static void
eventfd_free(file_descriptor* descriptor)
{
	delete (EventFD*)descriptor->cookie;
}


static status_t
eventfd_read(file_descriptor* descriptor, off_t pos, void* buffer,
	size_t* _length)
{
	if (*_length < sizeof(uint64))
		return B_BAD_VALUE;

	EventFD* event = (EventFD*)descriptor->cookie;
	MutexLocker locker(event->lock);

	while (event->counter == 0) {
		if ((descriptor->open_mode & O_NONBLOCK) != 0)
			return B_WOULD_BLOCK;

		status_t status = event->condition.Wait(&event->lock,
			B_CAN_INTERRUPT);
		if (status != B_OK)
			return status;
	}

	uint64 value = event->semaphore ? 1 : event->counter;

	// Copy out before consuming, so that a bad buffer loses nothing.
	if (user_memcpy(buffer, &value, sizeof(value)) != B_OK)
		return B_BAD_ADDRESS;

	event->counter -= value;
	event->Notify();

	*_length = sizeof(uint64);
	return B_OK;
}


static status_t
eventfd_write(file_descriptor* descriptor, off_t pos, const void* buffer,
	size_t* _length)
{
	if (*_length < sizeof(uint64))
		return B_BAD_VALUE;

	uint64 value;
	if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
		return B_BAD_ADDRESS;
	if (value == UINT64_MAX)
		return B_BAD_VALUE;

	EventFD* event = (EventFD*)descriptor->cookie;
	MutexLocker locker(event->lock);

	while (kMaxCounter - event->counter < value) {
		if ((descriptor->open_mode & O_NONBLOCK) != 0)
			return B_WOULD_BLOCK;

		status_t status = event->condition.Wait(&event->lock,
			B_CAN_INTERRUPT);
		if (status != B_OK)
			return status;
	}

	event->counter += value;
	if (value > 0)
		event->Notify();

	*_length = sizeof(uint64);
	return B_OK;
}


static status_t
eventfd_set_flags(file_descriptor* descriptor, int flags)
{
	// O_NONBLOCK is read from descriptor->open_mode, which fcntl() updates
	// after we accept the change; there is nothing else to do.
	return B_OK;
}


static status_t
eventfd_select(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	EventFD* eventFD = (EventFD*)descriptor->cookie;
	MutexLocker locker(eventFD->lock);

	status_t status = add_select_sync_pool_entry(&eventFD->selectPool, sync,
		event);
	if (status != B_OK)
		return status;

	// Signal right away if the condition already holds.
	if (event == B_SELECT_READ && eventFD->counter > 0)
		return notify_select_event(sync, event);
	if (event == B_SELECT_WRITE && eventFD->counter < kMaxCounter)
		return notify_select_event(sync, event);

	return B_OK;
}


static status_t
eventfd_deselect(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	EventFD* eventFD = (EventFD*)descriptor->cookie;
	MutexLocker locker(eventFD->lock);

	remove_select_sync_pool_entry(&eventFD->selectPool, sync, event);
	return B_OK;
}


static status_t
eventfd_read_stat(file_descriptor* descriptor, struct stat* stat)
{
	// Like a pipe: no file system object behind it.
	memset(stat, 0, sizeof(*stat));
	stat->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	stat->st_nlink = 1;
	stat->st_blksize = sizeof(uint64);
	return B_OK;
}


static struct fd_ops sEventFDOps = {
	&eventfd_close,
	&eventfd_free,
	&eventfd_read,
	&eventfd_write,
	NULL,	// readv
	NULL,	// writev
	NULL,	// seek
	NULL,	// ioctl
	&eventfd_set_flags,
	&eventfd_select,
	&eventfd_deselect,
	NULL,	// read_dir
	NULL,	// rewind_dir
	&eventfd_read_stat,
	NULL,	// write_stat
};


//	#pragma mark - syscall


/*!	Creates an eventfd. \a openFlags may contain O_CLOEXEC, O_CLOFORK and
	O_NONBLOCK; libgnu translates the EFD_* flags.
*/
int
_user_eventfd(uint32 initialValue, int openFlags, bool semaphore)
{
	if ((openFlags & ~(O_CLOEXEC | O_CLOFORK | O_NONBLOCK)) != 0)
		return B_BAD_VALUE;

	EventFD* event = new(std::nothrow) EventFD(initialValue, semaphore);
	if (event == NULL)
		return B_NO_MEMORY;

	file_descriptor* descriptor = alloc_fd();
	if (descriptor == NULL) {
		delete event;
		return B_NO_MEMORY;
	}

	descriptor->ops = &sEventFDOps;
	descriptor->cookie = event;
	descriptor->open_mode = O_RDWR | (openFlags & O_NONBLOCK);

	io_context* context = get_current_io_context(false);
	rw_lock_write_lock(&context->lock);
	int fd = new_fd(context, descriptor);
	if (fd < 0) {
		rw_lock_write_unlock(&context->lock);
		// put_fd() would call our fd_free() for the cookie; detach it first,
		// as event_queue does, and delete it ourselves.
		descriptor->ops = NULL;
		descriptor->cookie = NULL;
		put_fd(descriptor);
		delete event;
		return fd;
	}

	fd_set_close_on_exec(context, fd, (openFlags & O_CLOEXEC) != 0);
	fd_set_close_on_fork(context, fd, (openFlags & O_CLOFORK) != 0);
	rw_lock_write_unlock(&context->lock);

	return fd;
}
