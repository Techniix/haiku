/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	generic/syncobj/v1: DRM sync objects and sync_file descriptors with a
	kernel fence API (Quasar R-18). The interface is described in
	<syncobj_module.h>; the user ABI in <drm/drm_syncobj.h> and
	<linux/sync_file.h>.

	Model (the same as Linux's drm_syncobj.c and dma-fence-chain.c, written
	from their documented behaviour):
	 - A fence signals once, with a status. A "composite" fence signals once
	   all of its (up to two) parents have; it carries the first error.
	   SYNC_IOC_MERGE builds one from two fences; a timeline point is one
	   whose parents are the previous point and the point's own fence (a
	   fence chain). A signalled chain drops its link to the previous point,
	   so a long timeline does not keep its history alive.
	 - A sync object holds the newest fence (or none). Point 0 is that
	   fence; point N > 0 is the oldest chain whose number is >= N.
	 - Every state change (a fence signalled, an object's fence replaced)
	   wakes all waiters once; each waiter re-checks its own condition. The
	   number of waiters is small (a compositor, a few clients), and this
	   keeps waits for "all of", "any of", "submitted" and "available" one
	   loop with no per-object waiter lists.

	One recursive lock protects all state. Fence callbacks and the internal
	work done on signalling (chains, eventfds, select()) run with it held.
	Fence reference counts are atomic; the last release frees under the
	lock, iteratively, so a long chain does not recurse.
*/


#include <syncobj_module.h>

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <condition_variable.h>
#include <eventfd.h>
#include <fs/fd.h>
#include <fs/select_sync_pool.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <wait_for_objects.h>

#include <drm/drm_syncobj.h>
#include <linux/sync_file.h>


//#define TRACE_SYNCOBJ
#ifdef TRACE_SYNCOBJ
#	define TRACE(x...) dprintf("syncobj: " x)
#else
#	define TRACE(x...)
#endif


static const uint32 kMaxHandles = 65536;
static const uint32 kMaxWaitHandles = 4096;


struct syncobj_fence {
	int32					refCount;
	bool					signaled;
	status_t				error;
	bigtime_t				timestamp;
	char					driverName[32];
	char					timelineName[32];

	// composite fences (merges and timeline chains)
	syncobj_fence*			parents[2];
	syncobj_fence_callback	parentLinks[2];
	int32					pendingParents;

	// timeline chains: seqno and the previous point (dropped on signal)
	bool					isChain;
	uint64					seqno;
	syncobj_fence*			chainPrevious;

	syncobj_fence_callback*	callbacks;

	syncobj_fence*			nextFree;
		// work list while being freed or signalled
};


struct SyncObj {
	int32					refCount;		// under sLock
	syncobj_fence*			fence;
};


struct SwPoint {
	SwPoint*				next;
	uint32					value;
	syncobj_fence*			fence;			// referenced
};


struct syncobj_file {
	SyncObj**				handles;		// handle h is handles[h - 1]
	uint32					capacity;

	// sw_sync CPU timeline (/dev/syncobj only)
	uint32					swValue;
	SwPoint*				swPoints;
};


struct EventfdWatch {
	EventfdWatch*			next;
	SyncObj*				object;			// weak; removed with the object
	uint64					point;
	uint32					flags;
	file_descriptor*		eventfd;		// referenced
};


struct SyncFile {
	SyncFile*				next;			// sSyncFiles
	syncobj_fence*			fence;
	select_sync_pool*		selectPool;
	char					name[32];
};


static recursive_lock sLock;
static ConditionVariable sChanged;
static EventfdWatch* sEventfdWatches;
static SyncFile* sSyncFiles;
static int32 sFenceCount;
	// for the leak check in the harness (dprintf at uninit)


static void composite_parent_signaled(syncobj_fence* parent,
	syncobj_fence_callback* link);


//	#pragma mark - user/kernel memory


/*!	\a user: the request came from user space (its structure is at a user
	address), so every address in it is a user address, and anything else
	(including low addresses such as 16, which are not user addresses) is
	B_BAD_ADDRESS. Kernel callers pass kernel structures and addresses.
*/
static status_t
copy_from(void* to, const void* from, size_t size, bool user)
{
	if (user) {
		if (!IS_USER_ADDRESS(from))
			return B_BAD_ADDRESS;
		return user_memcpy(to, from, size);
	}
	memcpy(to, from, size);
	return B_OK;
}


static status_t
copy_to(void* to, const void* from, size_t size, bool user)
{
	if (user) {
		if (!IS_USER_ADDRESS(to))
			return B_BAD_ADDRESS;
		return user_memcpy(to, from, size);
	}
	memcpy(to, from, size);
	return B_OK;
}


//	#pragma mark - fences


static syncobj_fence*
fence_create(const char* driverName, const char* timelineName)
{
	syncobj_fence* fence = (syncobj_fence*)calloc(1, sizeof(syncobj_fence));
	if (fence == NULL)
		return NULL;

	fence->refCount = 1;
	strlcpy(fence->driverName, driverName != NULL ? driverName : "",
		sizeof(fence->driverName));
	strlcpy(fence->timelineName, timelineName != NULL ? timelineName : "",
		sizeof(fence->timelineName));
	atomic_add(&sFenceCount, 1);
	return fence;
}


static void
fence_acquire(syncobj_fence* fence)
{
	if (fence != NULL)
		atomic_add(&fence->refCount, 1);
}


static bool
unlink_callback_locked(syncobj_fence* fence, syncobj_fence_callback* callback)
{
	syncobj_fence_callback** link = &fence->callbacks;
	while (*link != NULL) {
		if (*link == callback) {
			*link = callback->_next;
			callback->_next = NULL;
			return true;
		}
		link = &(*link)->_next;
	}
	return false;
}


static void
fence_release(syncobj_fence* fence)
{
	if (fence == NULL || atomic_add(&fence->refCount, -1) != 1)
		return;

	// The last reference: free it and whatever only it kept alive, without
	// recursion (a pending timeline can be a long chain).
	RecursiveLocker locker(sLock);

	syncobj_fence* list = fence;
	fence->nextFree = NULL;
	while (list != NULL) {
		syncobj_fence* current = list;
		list = current->nextFree;

		syncobj_fence* drop[3] = { NULL, NULL, current->chainPrevious };
		for (int i = 0; i < 2; i++) {
			if (current->parents[i] == NULL)
				continue;
			// a pending composite is still linked to its parents
			unlink_callback_locked(current->parents[i],
				&current->parentLinks[i]);
			drop[i] = current->parents[i];
		}

		for (int i = 0; i < 3; i++) {
			if (drop[i] != NULL && atomic_add(&drop[i]->refCount, -1) == 1) {
				drop[i]->nextFree = list;
				list = drop[i];
			}
		}

		free(current);
		atomic_add(&sFenceCount, -1);
	}
}


static void changed_locked();


/*!	Signals \a fence and every composite that becomes complete with it.
	Called with sLock held.
*/
static status_t
fence_signal_locked(syncobj_fence* fence, status_t error)
{
	if (fence->signaled)
		return B_NOT_ALLOWED;

	fence->error = error;
	fence->nextFree = NULL;
	syncobj_fence* work = fence;

	while (work != NULL) {
		syncobj_fence* current = work;
		work = current->nextFree;

		current->signaled = true;
		current->timestamp = system_time();

		// Its parents are signalled now; drop them (and, for a chain, the
		// previous point: everything before a signalled point is). They
		// were taken off "work" before "current" became complete, so
		// freeing them here cannot touch the work list.
		for (int i = 0; i < 2; i++) {
			syncobj_fence* parent = current->parents[i];
			current->parents[i] = NULL;
			fence_release(parent);
		}
		syncobj_fence* previous = current->chainPrevious;
		current->chainPrevious = NULL;
		fence_release(previous);

		syncobj_fence_callback* callback = current->callbacks;
		current->callbacks = NULL;
		while (callback != NULL) {
			syncobj_fence_callback* next = callback->_next;
			callback->_next = NULL;
			if (callback->func == composite_parent_signaled) {
				syncobj_fence* child = (syncobj_fence*)callback->data;
				if (current->error != B_OK && child->error == B_OK)
					child->error = current->error;
				if (--child->pendingParents == 0 && !child->signaled) {
					child->nextFree = work;
					work = child;
				}
			} else
				callback->func(current, callback);
			callback = next;
		}
	}

	changed_locked();
	return B_OK;
}


static void
composite_parent_signaled(syncobj_fence* parent, syncobj_fence_callback* link)
{
	// never called: fence_signal_locked() handles these links itself
}


/*!	A fence that signals when \a a and \a b (either may be NULL) have.
	Takes references to both. With \a chain, a timeline point: \a a is the
	previous point, \a b the point's fence.
*/
static syncobj_fence*
composite_create_locked(syncobj_fence* a, syncobj_fence* b, bool chain,
	uint64 seqno)
{
	syncobj_fence* fence = fence_create("syncobj",
		chain ? "timeline" : "merge");
	if (fence == NULL)
		return NULL;

	fence->isChain = chain;
	fence->seqno = seqno;
	if (chain && a != NULL) {
		fence_acquire(a);
		fence->chainPrevious = a;
	}

	syncobj_fence* parents[2] = { a, b };
	for (int i = 0; i < 2; i++) {
		syncobj_fence* parent = parents[i];
		if (parent == NULL)
			continue;
		if (parent->signaled) {
			if (parent->error != B_OK && fence->error == B_OK)
				fence->error = parent->error;
			continue;
		}
		fence_acquire(parent);
		fence->parents[i] = parent;
		fence->parentLinks[i].func = composite_parent_signaled;
		fence->parentLinks[i].data = fence;
		fence->parentLinks[i]._next = parent->callbacks;
		parent->callbacks = &fence->parentLinks[i];
		fence->pendingParents++;
	}

	if (fence->pendingParents == 0) {
		fence->signaled = true;
		fence->timestamp = system_time();
		if (fence->chainPrevious != NULL) {
			syncobj_fence* previous = fence->chainPrevious;
			fence->chainPrevious = NULL;
			fence_release(previous);
		}
	}
	return fence;
}


static syncobj_fence*
signaled_stub()
{
	syncobj_fence* fence = fence_create("syncobj", "signaled");
	if (fence != NULL) {
		fence->signaled = true;
		fence->timestamp = system_time();
	}
	return fence;
}


static int32
fence_status_locked(syncobj_fence* fence)
{
	if (!fence->signaled)
		return 0;
	return fence->error != B_OK ? (fence->error < 0 ? fence->error : -EIO)
		: 1;
}


//	#pragma mark - sync objects


static SyncObj*
object_create(bool signaled)
{
	SyncObj* object = (SyncObj*)calloc(1, sizeof(SyncObj));
	if (object == NULL)
		return NULL;
	object->refCount = 1;
	if (signaled) {
		object->fence = signaled_stub();
		if (object->fence == NULL) {
			free(object);
			return NULL;
		}
	}
	return object;
}


static void
object_release_locked(SyncObj* object)
{
	if (--object->refCount > 0)
		return;

	EventfdWatch** link = &sEventfdWatches;
	while (*link != NULL) {
		EventfdWatch* watch = *link;
		if (watch->object == object) {
			*link = watch->next;
			put_fd(watch->eventfd);
			free(watch);
		} else
			link = &watch->next;
	}

	fence_release(object->fence);
	free(object);
}


/*!	The fence for \a point of \a object, without a reference; NULL if none
	was submitted for it yet.
*/
static syncobj_fence*
object_find_locked(SyncObj* object, uint64 point)
{
	syncobj_fence* fence = object->fence;
	if (fence == NULL || point == 0)
		return fence;
	if (!fence->isChain || fence->seqno < point)
		return NULL;

	// the oldest chain with seqno >= point
	while (fence->chainPrevious != NULL && fence->chainPrevious->isChain
		&& fence->chainPrevious->seqno >= point) {
		fence = fence->chainPrevious;
	}
	return fence;
}


static void
object_replace_locked(SyncObj* object, syncobj_fence* fence)
{
	fence_acquire(fence);
	syncobj_fence* old = object->fence;
	object->fence = fence;
	fence_release(old);
	changed_locked();
}


static status_t
object_add_point_locked(SyncObj* object, syncobj_fence* fence, uint64 point)
{
	// Linux accepts unordered points (with a debug message); they join the
	// chain all the same.
	syncobj_fence* chain = composite_create_locked(object->fence, fence,
		true, point);
	if (chain == NULL)
		return B_NO_MEMORY;
	syncobj_fence* old = object->fence;
	object->fence = chain;
	fence_release(old);
	changed_locked();
	return B_OK;
}


//	#pragma mark - change notification


/*!	Wakes waiters, fires due SYNCOBJ_EVENTFD watches and notifies select()
	on signalled sync_files. Called with sLock held after any change.
*/
static void
changed_locked()
{
	sChanged.NotifyAll();

	EventfdWatch** link = &sEventfdWatches;
	while (*link != NULL) {
		EventfdWatch* watch = *link;
		syncobj_fence* fence = object_find_locked(watch->object,
			watch->point);
		bool due = fence != NULL
			&& ((watch->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0
				|| fence->signaled);
		if (due) {
			*link = watch->next;
			eventfd_signal_descriptor(watch->eventfd, 1);
			put_fd(watch->eventfd);
			free(watch);
		} else
			link = &watch->next;
	}

	for (SyncFile* file = sSyncFiles; file != NULL; file = file->next) {
		if (file->selectPool != NULL && file->fence->signaled)
			notify_select_event_pool(file->selectPool, B_SELECT_READ);
	}
}


//	#pragma mark - file handles


static status_t
file_open(syncobj_file** _file)
{
	syncobj_file* file = (syncobj_file*)calloc(1, sizeof(syncobj_file));
	if (file == NULL)
		return B_NO_MEMORY;
	*_file = file;
	return B_OK;
}


static void
file_close(syncobj_file* file)
{
	if (file == NULL)
		return;
	RecursiveLocker locker(sLock);
	for (uint32 i = 0; i < file->capacity; i++) {
		if (file->handles[i] != NULL)
			object_release_locked(file->handles[i]);
	}

	// as Linux's sw_sync does when its timeline goes away
	while (file->swPoints != NULL) {
		SwPoint* point = file->swPoints;
		file->swPoints = point->next;
		fence_signal_locked(point->fence, ENOENT);
		fence_release(point->fence);
		free(point);
	}
	free(file->handles);
	free(file);
}


static SyncObj*
file_lookup_locked(syncobj_file* file, uint32 handle)
{
	if (handle == 0 || handle > file->capacity)
		return NULL;
	return file->handles[handle - 1];
}


//! Takes over the caller's reference on success.
static status_t
file_add_locked(syncobj_file* file, SyncObj* object, uint32* _handle)
{
	for (uint32 i = 0; i < file->capacity; i++) {
		if (file->handles[i] == NULL) {
			file->handles[i] = object;
			*_handle = i + 1;
			return B_OK;
		}
	}

	if (file->capacity >= kMaxHandles)
		return B_NO_MEMORY;
	uint32 capacity = file->capacity == 0 ? 16 : file->capacity * 2;
	SyncObj** handles = (SyncObj**)realloc(file->handles,
		capacity * sizeof(SyncObj*));
	if (handles == NULL)
		return B_NO_MEMORY;
	memset(handles + file->capacity, 0,
		(capacity - file->capacity) * sizeof(SyncObj*));
	handles[file->capacity] = object;
	*_handle = file->capacity + 1;
	file->handles = handles;
	file->capacity = capacity;
	return B_OK;
}


//	#pragma mark - descriptors


static status_t
syncobj_fd_close(file_descriptor* descriptor)
{
	return B_OK;
}


static void
syncobj_fd_free(file_descriptor* descriptor)
{
	RecursiveLocker locker(sLock);
	object_release_locked((SyncObj*)descriptor->cookie);
}


static status_t
plain_read_stat(file_descriptor* descriptor, struct stat* stat)
{
	memset(stat, 0, sizeof(*stat));
	stat->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	stat->st_nlink = 1;
	return B_OK;
}


static status_t
plain_set_flags(file_descriptor* descriptor, int flags)
{
	return B_OK;
}


//! An exported sync object (HANDLE_TO_FD without EXPORT_SYNC_FILE).
static struct fd_ops sSyncObjFDOps = {
	&syncobj_fd_close,
	&syncobj_fd_free,
	NULL, NULL, NULL, NULL, NULL,	// read, write, readv, writev, seek
	NULL,							// ioctl
	&plain_set_flags,
	NULL, NULL,						// select, deselect
	NULL, NULL,						// read_dir, rewind_dir
	&plain_read_stat,
	NULL,
};


static status_t
sync_file_close(file_descriptor* descriptor)
{
	return B_OK;
}


static void
sync_file_free(file_descriptor* descriptor)
{
	SyncFile* file = (SyncFile*)descriptor->cookie;
	RecursiveLocker locker(sLock);
	SyncFile** link = &sSyncFiles;
	while (*link != NULL) {
		if (*link == file) {
			*link = file->next;
			break;
		}
		link = &(*link)->next;
	}
	delete_select_sync_pool(file->selectPool);
	fence_release(file->fence);
	free(file);
}


static status_t
sync_file_select(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	SyncFile* file = (SyncFile*)descriptor->cookie;
	RecursiveLocker locker(sLock);
	status_t status = add_select_sync_pool_entry(&file->selectPool, sync,
		event);
	if (status != B_OK)
		return status;
	if (event == B_SELECT_READ && file->fence->signaled)
		return notify_select_event(sync, event);
	return B_OK;
}


static status_t
sync_file_deselect(file_descriptor* descriptor, uint8 event,
	selectsync* sync)
{
	SyncFile* file = (SyncFile*)descriptor->cookie;
	RecursiveLocker locker(sLock);
	remove_select_sync_pool_entry(&file->selectPool, sync, event);
	return B_OK;
}


static status_t sync_file_ioctl(file_descriptor* descriptor, ulong op,
	void* buffer, size_t length);


static struct fd_ops sSyncFileFDOps = {
	&sync_file_close,
	&sync_file_free,
	NULL, NULL, NULL, NULL, NULL,
	&sync_file_ioctl,
	&plain_set_flags,
	&sync_file_select,
	&sync_file_deselect,
	NULL, NULL,
	&plain_read_stat,
	NULL,
};


/*!	Installs \a cookie with \a ops as a new descriptor of the current team.
	On failure the cookie stays the caller's.
*/
static int
install_fd(struct fd_ops* ops, void* cookie, bool kernel, bool closeOnExec)
{
	file_descriptor* descriptor = alloc_fd();
	if (descriptor == NULL)
		return B_NO_MEMORY;
	descriptor->ops = ops;
	descriptor->cookie = cookie;
	descriptor->open_mode = O_RDWR;

	io_context* context = get_current_io_context(kernel);
	rw_lock_write_lock(&context->lock);
	int fd = new_fd(context, descriptor);
	if (fd < 0) {
		rw_lock_write_unlock(&context->lock);
		descriptor->ops = NULL;
		descriptor->cookie = NULL;
		put_fd(descriptor);
		return fd;
	}
	fd_set_close_on_exec(context, fd, closeOnExec);
	rw_lock_write_unlock(&context->lock);
	return fd;
}


/*!	A new sync_file descriptor for \a fence (which gets its own reference).
	Called without sLock: installing a descriptor takes the team's
	descriptor table lock, and descriptor tables call our free hooks, which
	take sLock, so sLock is never held around it. The caller keeps \a fence
	alive during the call.
*/
static int
create_sync_file(syncobj_fence* fence, const char* name, bool kernel,
	bool closeOnExec)
{
	SyncFile* file = (SyncFile*)calloc(1, sizeof(SyncFile));
	if (file == NULL)
		return B_NO_MEMORY;
	fence_acquire(fence);
	file->fence = fence;
	strlcpy(file->name, name != NULL ? name : "", sizeof(file->name));

	// linked first: once installed, another thread may close it
	{
		RecursiveLocker locker(sLock);
		file->next = sSyncFiles;
		sSyncFiles = file;
	}

	int fd = install_fd(&sSyncFileFDOps, file, kernel, closeOnExec);
	if (fd < 0) {
		RecursiveLocker locker(sLock);
		SyncFile** link = &sSyncFiles;
		while (*link != file)
			link = &(*link)->next;
		*link = file->next;
		locker.Unlock();
		fence_release(fence);
		free(file);
	}
	return fd;
}


//! Returns the descriptor with a reference if it has \a ops.
static file_descriptor*
get_typed_fd(int fd, struct fd_ops* ops, bool kernel)
{
	file_descriptor* descriptor = get_fd(get_current_io_context(kernel), fd);
	if (descriptor == NULL)
		return NULL;
	if (descriptor->ops != ops) {
		put_fd(descriptor);
		return NULL;
	}
	return descriptor;
}


static status_t
sync_file_ioctl(file_descriptor* descriptor, ulong op, void* buffer,
	size_t length)
{
	SyncFile* file = (SyncFile*)descriptor->cookie;
	bool kernel = !IS_USER_ADDRESS(buffer);

	switch ((uint32)op) {
		case SYNC_IOC_MERGE:
		{
			sync_merge_data data;
			if (copy_from(&data, buffer, sizeof(data), !kernel) != B_OK)
				return B_BAD_ADDRESS;
			if (data.flags != 0 || data.pad != 0)
				return B_BAD_VALUE;
			data.name[sizeof(data.name) - 1] = '\0';

			file_descriptor* other = get_typed_fd(data.fd2, &sSyncFileFDOps,
				kernel);
			if (other == NULL)
				return B_BAD_VALUE;

			RecursiveLocker locker(sLock);
			syncobj_fence* merged = composite_create_locked(file->fence,
				((SyncFile*)other->cookie)->fence, false, 0);
			locker.Unlock();
			if (merged == NULL) {
				put_fd(other);
				return B_NO_MEMORY;
			}
			int fd = create_sync_file(merged, data.name, kernel, true);
			fence_release(merged);
			put_fd(other);
			if (fd < 0)
				return fd;

			data.fence = fd;
			if (copy_to(buffer, &data, sizeof(data), !kernel) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case SYNC_IOC_FILE_INFO:
		{
			sync_file_info info;
			if (copy_from(&info, buffer, sizeof(info), !kernel) != B_OK)
				return B_BAD_ADDRESS;
			if (info.flags != 0 || info.pad != 0)
				return B_BAD_VALUE;

			RecursiveLocker locker(sLock);
			syncobj_fence* fence = file->fence;
			sync_fence_info fenceInfo;
			memset(&fenceInfo, 0, sizeof(fenceInfo));
			strlcpy(fenceInfo.obj_name, fence->timelineName,
				sizeof(fenceInfo.obj_name));
			strlcpy(fenceInfo.driver_name, fence->driverName,
				sizeof(fenceInfo.driver_name));
			fenceInfo.status = fence_status_locked(fence);
			fenceInfo.timestamp_ns = fence->signaled
				? (uint64)fence->timestamp * 1000 : 0;
			strlcpy(info.name, file->name, sizeof(info.name));
			info.status = fenceInfo.status;
			locker.Unlock();

			// One fence per file: merges are kept as one composite.
			if (info.num_fences != 0) {
				if (copy_to((void*)(addr_t)info.sync_fence_info, &fenceInfo,
						sizeof(fenceInfo), !kernel) != B_OK) {
					return B_BAD_ADDRESS;
				}
			}
			info.num_fences = 1;
			if (copy_to(buffer, &info, sizeof(info), !kernel) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case SYNC_IOC_SET_DEADLINE:
			return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - waits


/*!	Waits until \a count (object, point) pairs are ready, all of them or
	(without WAIT_ALL) any. "Ready" is: signalled, or with WAIT_AVAILABLE a
	fence exists. Without WAIT_FOR_SUBMIT a missing fence is B_BAD_VALUE
	right away, as on Linux.
*/
static status_t
wait_objects(SyncObj** objects, const uint64* points, uint32 count,
	uint32 flags, bigtime_t absoluteTimeout, uint32* _firstSignaled)
{
	RecursiveLocker locker(sLock);

	if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) == 0) {
		for (uint32 i = 0; i < count; i++) {
			if (object_find_locked(objects[i], points != NULL ? points[i] : 0)
					== NULL) {
				return B_BAD_VALUE;
			}
		}
	}

	bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	bool available = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0;

	while (true) {
		uint32 ready = 0;
		uint32 first = UINT32_MAX;
		for (uint32 i = 0; i < count; i++) {
			syncobj_fence* fence = object_find_locked(objects[i],
				points != NULL ? points[i] : 0);
			if (fence != NULL && (available || fence->signaled)) {
				ready++;
				if (first == UINT32_MAX)
					first = i;
			}
		}
		if (all ? ready == count : ready > 0) {
			*_firstSignaled = first == UINT32_MAX ? 0 : first;
			return B_OK;
		}

		if (absoluteTimeout <= system_time())
			return ETIME;

		status_t status = sChanged.Wait(&sLock,
			B_CAN_INTERRUPT | B_ABSOLUTE_TIMEOUT, absoluteTimeout);
		if (status == B_INTERRUPTED)
			return B_INTERRUPTED;
		// B_TIMED_OUT: one more check above, then ETIME
	}
}


//! Linux's absolute CLOCK_MONOTONIC nanoseconds to system_time().
static bigtime_t
timeout_from_nsec(int64 nsec)
{
	if (nsec <= 0)
		return 0;
	if (nsec / 1000 >= B_INFINITE_TIMEOUT)
		return B_INFINITE_TIMEOUT;
	return nsec / 1000;
}


//	#pragma mark - ioctls


struct HandleArray {
	SyncObj**	objects;
	uint64*		points;
	uint32		count;

	HandleArray() : objects(NULL), points(NULL), count(0) {}

	~HandleArray()
	{
		if (objects != NULL) {
			RecursiveLocker locker(sLock);
			for (uint32 i = 0; i < count; i++) {
				if (objects[i] != NULL)
					object_release_locked(objects[i]);
			}
		}
		free(objects);
		free(points);
	}

	//! Copies the handles (and points) and takes a reference to each.
	status_t Init(syncobj_file* file, uint64 userHandles, uint64 userPoints,
		uint32 handleCount, bool withPoints, bool user)
	{
		if (handleCount == 0 || handleCount > kMaxWaitHandles)
			return B_BAD_VALUE;
		uint32* handles = (uint32*)malloc(handleCount * sizeof(uint32));
		objects = (SyncObj**)calloc(handleCount, sizeof(SyncObj*));
		if (handles == NULL || objects == NULL) {
			free(handles);
			return B_NO_MEMORY;
		}
		if (copy_from(handles, (void*)(addr_t)userHandles,
				handleCount * sizeof(uint32), user) != B_OK) {
			free(handles);
			return B_BAD_ADDRESS;
		}
		if (withPoints) {
			points = (uint64*)malloc(handleCount * sizeof(uint64));
			if (points == NULL) {
				free(handles);
				return B_NO_MEMORY;
			}
			if (copy_from(points, (void*)(addr_t)userPoints,
					handleCount * sizeof(uint64), user) != B_OK) {
				free(handles);
				return B_BAD_ADDRESS;
			}
		}

		RecursiveLocker locker(sLock);
		count = handleCount;
		for (uint32 i = 0; i < handleCount; i++) {
			objects[i] = file_lookup_locked(file, handles[i]);
			if (objects[i] == NULL) {
				free(handles);
				return B_ENTRY_NOT_FOUND;
			}
			objects[i]->refCount++;
		}
		free(handles);
		return B_OK;
	}
};


static status_t
ioctl_create(syncobj_file* file, drm_syncobj_create& data)
{
	if ((data.flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) != 0)
		return B_BAD_VALUE;
	RecursiveLocker locker(sLock);
	SyncObj* object = object_create(
		(data.flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0);
	if (object == NULL)
		return B_NO_MEMORY;
	status_t status = file_add_locked(file, object, &data.handle);
	if (status != B_OK)
		object_release_locked(object);
	return status;
}


static status_t
ioctl_destroy(syncobj_file* file, drm_syncobj_destroy& data)
{
	if (data.pad != 0)
		return B_BAD_VALUE;
	RecursiveLocker locker(sLock);
	SyncObj* object = file_lookup_locked(file, data.handle);
	if (object == NULL)
		return B_BAD_VALUE;
	file->handles[data.handle - 1] = NULL;
	object_release_locked(object);
	return B_OK;
}


static status_t
ioctl_handle_to_fd(syncobj_file* file, drm_syncobj_handle& data, bool kernel)
{
	if (data.pad != 0)
		return B_BAD_VALUE;
	uint32 known = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE
		| DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE;
	if ((data.flags & ~known) != 0)
		return B_BAD_VALUE;
	bool exportSyncFile
		= (data.flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) != 0;
	bool timeline = (data.flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE) != 0;
	if (timeline && !exportSyncFile)
		return B_BAD_VALUE;

	RecursiveLocker locker(sLock);
	SyncObj* object = file_lookup_locked(file, data.handle);
	if (object == NULL)
		return B_ENTRY_NOT_FOUND;

	// descriptors are installed without sLock (see create_sync_file())
	int fd;
	if (exportSyncFile) {
		syncobj_fence* fence = object_find_locked(object,
			timeline ? data.point : 0);
		if (fence == NULL)
			return B_BAD_VALUE;
		fence_acquire(fence);
		locker.Unlock();
		fd = create_sync_file(fence, "syncobj", kernel, true);
		fence_release(fence);
	} else {
		object->refCount++;
		locker.Unlock();
		fd = install_fd(&sSyncObjFDOps, object, kernel, true);
		if (fd < 0) {
			locker.Lock();
			object_release_locked(object);
		}
	}
	if (fd < 0)
		return fd;
	data.fd = fd;
	return B_OK;
}


static status_t
ioctl_fd_to_handle(syncobj_file* file, drm_syncobj_handle& data, bool kernel)
{
	if (data.pad != 0)
		return B_BAD_VALUE;
	uint32 known = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE
		| DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE;
	if ((data.flags & ~known) != 0)
		return B_BAD_VALUE;
	bool importSyncFile
		= (data.flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) != 0;
	bool timeline = (data.flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE) != 0;
	if (timeline && !importSyncFile)
		return B_BAD_VALUE;

	if (importSyncFile) {
		file_descriptor* descriptor = get_typed_fd(data.fd, &sSyncFileFDOps,
			kernel);
		if (descriptor == NULL)
			return B_BAD_VALUE;
		RecursiveLocker locker(sLock);
		SyncObj* object = file_lookup_locked(file, data.handle);
		status_t status = B_ENTRY_NOT_FOUND;
		if (object != NULL) {
			syncobj_fence* fence = ((SyncFile*)descriptor->cookie)->fence;
			if (timeline && data.point != 0)
				status = object_add_point_locked(object, fence, data.point);
			else {
				object_replace_locked(object, fence);
				status = B_OK;
			}
		}
		locker.Unlock();
		put_fd(descriptor);
		return status;
	}

	file_descriptor* descriptor = get_typed_fd(data.fd, &sSyncObjFDOps,
		kernel);
	if (descriptor == NULL)
		return B_BAD_VALUE;
	RecursiveLocker locker(sLock);
	SyncObj* object = (SyncObj*)descriptor->cookie;
	object->refCount++;
	status_t status = file_add_locked(file, object, &data.handle);
	if (status != B_OK)
		object_release_locked(object);
	locker.Unlock();
	put_fd(descriptor);
	return status;
}


static status_t
ioctl_wait(syncobj_file* file, uint64 handles, uint64 points, uint32 count,
	uint32& flags, int64 timeoutNsec, uint32& firstSignaled, bool timeline,
	bool user)
{
	uint32 known = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL
		| DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT
		| DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE
		| (timeline ? DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE : 0);
	if ((flags & ~known) != 0)
		return B_BAD_VALUE;

	HandleArray array;
	status_t status = array.Init(file, handles, points, count, timeline,
		user);
	if (status != B_OK)
		return status;
	return wait_objects(array.objects, array.points, array.count, flags,
		timeout_from_nsec(timeoutNsec), &firstSignaled);
}


static status_t
ioctl_reset_signal(syncobj_file* file, drm_syncobj_array& data, bool signal,
	bool user)
{
	if (data.pad != 0)
		return B_BAD_VALUE;
	HandleArray array;
	status_t status = array.Init(file, data.handles, 0, data.count_handles,
		false, user);
	if (status != B_OK)
		return status;

	RecursiveLocker locker(sLock);
	syncobj_fence* stub = NULL;
	if (signal) {
		stub = signaled_stub();
		if (stub == NULL)
			return B_NO_MEMORY;
	}
	for (uint32 i = 0; i < array.count; i++)
		object_replace_locked(array.objects[i], stub);
	fence_release(stub);
	return B_OK;
}


static status_t
ioctl_timeline_signal(syncobj_file* file, drm_syncobj_timeline_array& data,
	bool user)
{
	if (data.flags != 0)
		return B_BAD_VALUE;
	HandleArray array;
	status_t status = array.Init(file, data.handles, data.points,
		data.count_handles, true, user);
	if (status != B_OK)
		return status;

	RecursiveLocker locker(sLock);
	syncobj_fence* stub = signaled_stub();
	if (stub == NULL)
		return B_NO_MEMORY;
	for (uint32 i = 0; i < array.count; i++) {
		if (array.points[i] == 0)
			object_replace_locked(array.objects[i], stub);
		else {
			status = object_add_point_locked(array.objects[i], stub,
				array.points[i]);
			if (status != B_OK)
				break;
		}
	}
	fence_release(stub);
	return status;
}


static status_t
ioctl_query(syncobj_file* file, drm_syncobj_timeline_array& data,
	bool user)
{
	if ((data.flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) != 0)
		return B_BAD_VALUE;
	HandleArray array;
	status_t status = array.Init(file, data.handles, data.points,
		data.count_handles, true, user);
	if (status != B_OK)
		return status;

	RecursiveLocker locker(sLock);
	for (uint32 i = 0; i < array.count; i++) {
		uint64 value = 0;
		syncobj_fence* fence = array.objects[i]->fence;
		if (fence != NULL && fence->isChain) {
			if ((data.flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) != 0)
				value = fence->seqno;
			else {
				// the newest signalled point; everything before it is too
				for (syncobj_fence* chain = fence; chain != NULL
						&& chain->isChain; chain = chain->chainPrevious) {
					if (chain->signaled) {
						value = chain->seqno;
						break;
					}
				}
			}
		}
		array.points[i] = value;
	}
	locker.Unlock();

	if (copy_to((void*)(addr_t)data.points, array.points,
			array.count * sizeof(uint64), user) != B_OK) {
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


static status_t
ioctl_transfer(syncobj_file* file, drm_syncobj_transfer& data)
{
	if (data.pad != 0
		|| (data.flags & ~DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0) {
		return B_BAD_VALUE;
	}

	RecursiveLocker locker(sLock);
	SyncObj* source = file_lookup_locked(file, data.src_handle);
	SyncObj* target = file_lookup_locked(file, data.dst_handle);
	if (source == NULL || target == NULL)
		return B_ENTRY_NOT_FOUND;

	if ((data.flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0) {
		source->refCount++;
		uint32 first;
		status_t status = wait_objects(&source, &data.src_point, 1,
			DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT
				| DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
			B_INFINITE_TIMEOUT, &first);
		object_release_locked(source);
		if (status != B_OK)
			return status;
		// the objects may have been destroyed while we waited
		source = file_lookup_locked(file, data.src_handle);
		target = file_lookup_locked(file, data.dst_handle);
		if (source == NULL || target == NULL)
			return B_ENTRY_NOT_FOUND;
	}

	syncobj_fence* fence = object_find_locked(source, data.src_point);
	if (fence == NULL)
		return B_BAD_VALUE;
	if (data.dst_point != 0)
		return object_add_point_locked(target, fence, data.dst_point);
	object_replace_locked(target, fence);
	return B_OK;
}


static status_t
ioctl_eventfd(syncobj_file* file, drm_syncobj_eventfd& data, bool kernel)
{
	if (data.pad != 0
		|| (data.flags & ~DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0) {
		return B_BAD_VALUE;
	}

	file_descriptor* eventfd = get_fd(get_current_io_context(kernel),
		data.fd);
	if (eventfd == NULL)
		return B_FILE_ERROR;
	if (!eventfd_is_descriptor(eventfd)) {
		put_fd(eventfd);
		return B_BAD_VALUE;
	}

	EventfdWatch* watch = (EventfdWatch*)calloc(1, sizeof(EventfdWatch));
	if (watch == NULL) {
		put_fd(eventfd);
		return B_NO_MEMORY;
	}

	RecursiveLocker locker(sLock);
	SyncObj* object = file_lookup_locked(file, data.handle);
	if (object == NULL) {
		locker.Unlock();
		put_fd(eventfd);
		free(watch);
		return B_ENTRY_NOT_FOUND;
	}
	watch->object = object;
	watch->point = data.point;
	watch->flags = data.flags;
	watch->eventfd = eventfd;
	watch->next = sEventfdWatches;
	sEventfdWatches = watch;

	// fires right away if the point is already there
	changed_locked();
	return B_OK;
}


struct IoctlEntry {
	uint8	number;
	uint16	size;		// our structure's
};

static const IoctlEntry kIoctls[] = {
	{ 0xBF, sizeof(drm_syncobj_create) },
	{ 0xC0, sizeof(drm_syncobj_destroy) },
	{ 0xC1, sizeof(drm_syncobj_handle) },
	{ 0xC2, sizeof(drm_syncobj_handle) },
	{ 0xC3, sizeof(drm_syncobj_wait) },
	{ 0xC4, sizeof(drm_syncobj_array) },
	{ 0xC5, sizeof(drm_syncobj_array) },
	{ 0xCA, sizeof(drm_syncobj_timeline_wait) },
	{ 0xCB, sizeof(drm_syncobj_timeline_array) },
	{ 0xCC, sizeof(drm_syncobj_transfer) },
	{ 0xCD, sizeof(drm_syncobj_timeline_array) },
	{ 0xCF, sizeof(drm_syncobj_eventfd) },
};


static const IoctlEntry*
find_ioctl(uint32 op)
{
	if (_LINUX_IOC_TYPE(op) != RENGA_DRM_IOCTL_BASE
		|| _LINUX_IOC_DIR(op) != (_LINUX_IOC_READ | _LINUX_IOC_WRITE)) {
		return NULL;
	}
	for (size_t i = 0; i < B_COUNT_OF(kIoctls); i++) {
		if (kIoctls[i].number == _LINUX_IOC_NR(op))
			return &kIoctls[i];
	}
	return NULL;
}


static status_t
sw_sync_ioctl(syncobj_file* file, uint32 op, void* buffer)
{
	bool kernel = !IS_USER_ADDRESS(buffer);

	if (op == SW_SYNC_IOC_CREATE_FENCE) {
		sw_sync_create_fence_data data;
		if (copy_from(&data, buffer, sizeof(data), !kernel) != B_OK)
			return B_BAD_ADDRESS;
		data.name[sizeof(data.name) - 1] = '\0';

		syncobj_fence* fence = fence_create("sw_sync", data.name);
		SwPoint* point = (SwPoint*)calloc(1, sizeof(SwPoint));
		if (fence == NULL || point == NULL) {
			fence_release(fence);
			free(point);
			return B_NO_MEMORY;
		}

		RecursiveLocker locker(sLock);
		if (data.value <= file->swValue) {
			fence->signaled = true;
			fence->timestamp = system_time();
			free(point);
		} else {
			fence_acquire(fence);
			point->fence = fence;
			point->value = data.value;
			point->next = file->swPoints;
			file->swPoints = point;
		}
		locker.Unlock();

		int fd = create_sync_file(fence, data.name, kernel, true);
		fence_release(fence);
		if (fd < 0)
			return fd;
		data.fence = fd;
		if (copy_to(buffer, &data, sizeof(data), !kernel) != B_OK)
			return B_BAD_ADDRESS;
		return B_OK;
	}

	if (op == SW_SYNC_IOC_INC) {
		uint32 increment;
		if (copy_from(&increment, buffer, sizeof(increment), !kernel) != B_OK)
			return B_BAD_ADDRESS;

		RecursiveLocker locker(sLock);
		if (UINT32_MAX - file->swValue < increment)
			return B_BAD_VALUE;
		file->swValue += increment;

		SwPoint** link = &file->swPoints;
		while (*link != NULL) {
			SwPoint* point = *link;
			if (point->value <= file->swValue) {
				*link = point->next;
				fence_signal_locked(point->fence, B_OK);
				fence_release(point->fence);
				free(point);
			} else
				link = &point->next;
		}
		return B_OK;
	}

	return B_DEV_INVALID_IOCTL;
}


static bool
is_syncobj_ioctl(uint32 op)
{
	return find_ioctl(op) != NULL;
}


/*!	The DRM_IOCTL_SYNCOBJ_* dispatcher. Like Linux's drm_ioctl(), it accepts
	the structure sizes of older and newer headers: it copies in what the
	program sent, zero-fills the rest, and copies back no more than that.
*/
static status_t
syncobj_ioctl(syncobj_file* file, uint32 op, void* buffer, size_t length)
{
	if (file == NULL)
		return B_BAD_VALUE;
	if (op == SW_SYNC_IOC_CREATE_FENCE || op == SW_SYNC_IOC_INC)
		return sw_sync_ioctl(file, op, buffer);

	const IoctlEntry* entry = find_ioctl(op);
	if (entry == NULL)
		return B_DEV_INVALID_IOCTL;

	union {
		drm_syncobj_create			create;
		drm_syncobj_destroy			destroy;
		drm_syncobj_handle			handle;
		drm_syncobj_wait			wait;
		drm_syncobj_array			array;
		drm_syncobj_timeline_wait	timelineWait;
		drm_syncobj_timeline_array	timelineArray;
		drm_syncobj_transfer		transfer;
		drm_syncobj_eventfd			eventfd;
	} data;
	memset(&data, 0, sizeof(data));

	size_t size = _LINUX_IOC_SIZE(op);
	if (size > entry->size)
		size = entry->size;
	bool kernel = !IS_USER_ADDRESS(buffer);
	if (buffer == NULL || copy_from(&data, buffer, size, !kernel) != B_OK)
		return B_BAD_ADDRESS;
	status_t status;
	switch (entry->number) {
		case 0xBF:
			status = ioctl_create(file, data.create);
			break;
		case 0xC0:
			status = ioctl_destroy(file, data.destroy);
			break;
		case 0xC1:
			status = ioctl_handle_to_fd(file, data.handle, kernel);
			break;
		case 0xC2:
			status = ioctl_fd_to_handle(file, data.handle, kernel);
			break;
		case 0xC3:
			status = ioctl_wait(file, data.wait.handles, 0,
				data.wait.count_handles, data.wait.flags,
				data.wait.timeout_nsec, data.wait.first_signaled, false,
				!kernel);
			break;
		case 0xC4:
			status = ioctl_reset_signal(file, data.array, false, !kernel);
			break;
		case 0xC5:
			status = ioctl_reset_signal(file, data.array, true, !kernel);
			break;
		case 0xCA:
			status = ioctl_wait(file, data.timelineWait.handles,
				data.timelineWait.points, data.timelineWait.count_handles,
				data.timelineWait.flags, data.timelineWait.timeout_nsec,
				data.timelineWait.first_signaled, true, !kernel);
			break;
		case 0xCB:
			status = ioctl_query(file, data.timelineArray, !kernel);
			break;
		case 0xCC:
			status = ioctl_transfer(file, data.transfer);
			break;
		case 0xCD:
			status = ioctl_timeline_signal(file, data.timelineArray, !kernel);
			break;
		case 0xCF:
			status = ioctl_eventfd(file, data.eventfd, kernel);
			break;
		default:
			status = B_DEV_INVALID_IOCTL;
			break;
	}

	if (status == B_OK && copy_to(buffer, &data, size, !kernel) != B_OK)
		return B_BAD_ADDRESS;
	TRACE("ioctl %#" B_PRIx32 ": %s\n", op, strerror(status));
	return status;
}


//	#pragma mark - kernel API


static status_t
api_signal_fence(syncobj_fence* fence, status_t error)
{
	if (fence == NULL)
		return B_BAD_VALUE;
	RecursiveLocker locker(sLock);
	return fence_signal_locked(fence, error);
}


static status_t
api_fence_status(syncobj_fence* fence)
{
	if (fence == NULL)
		return B_BAD_VALUE;
	RecursiveLocker locker(sLock);
	return fence_status_locked(fence);
}


static status_t
api_wait_fence(syncobj_fence* fence, uint32 flags, bigtime_t absoluteTimeout)
{
	if (fence == NULL)
		return B_BAD_VALUE;
	RecursiveLocker locker(sLock);
	while (!fence->signaled) {
		if (absoluteTimeout <= system_time())
			return B_TIMED_OUT;
		status_t status = sChanged.Wait(&sLock, B_ABSOLUTE_TIMEOUT
				| ((flags & B_SYNCOBJ_WAIT_INTERRUPTIBLE) != 0
					? B_CAN_INTERRUPT : 0),
			absoluteTimeout);
		if (status == B_INTERRUPTED)
			return status;
	}
	return B_OK;
}


static status_t
api_add_fence_callback(syncobj_fence* fence, syncobj_fence_callback* callback,
	syncobj_fence_func func, void* data)
{
	if (fence == NULL || callback == NULL || func == NULL
		|| func == composite_parent_signaled) {
		return B_BAD_VALUE;
	}
	RecursiveLocker locker(sLock);
	if (fence->signaled)
		return B_NOT_ALLOWED;
	callback->func = func;
	callback->data = data;
	callback->_next = fence->callbacks;
	fence->callbacks = callback;
	return B_OK;
}


static bool
api_remove_fence_callback(syncobj_fence* fence,
	syncobj_fence_callback* callback)
{
	if (fence == NULL || callback == NULL)
		return false;
	RecursiveLocker locker(sLock);
	return unlink_callback_locked(fence, callback);
}


static status_t
api_find_fence(syncobj_file* file, uint32 handle, uint64 point, uint32 flags,
	bigtime_t absoluteTimeout, syncobj_fence** _fence)
{
	if (file == NULL || _fence == NULL)
		return B_BAD_VALUE;
	RecursiveLocker locker(sLock);
	SyncObj* object = file_lookup_locked(file, handle);
	if (object == NULL)
		return B_ENTRY_NOT_FOUND;

	if ((flags & B_SYNCOBJ_FIND_WAIT_FOR_SUBMIT) != 0) {
		object->refCount++;
		uint32 first;
		status_t status = wait_objects(&object, &point, 1,
			DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT
				| DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
			absoluteTimeout, &first);
		syncobj_fence* fence = status == B_OK
			? object_find_locked(object, point) : NULL;
		fence_acquire(fence);
		object_release_locked(object);
		if (status != B_OK)
			return status == ETIME ? B_TIMED_OUT : status;
		*_fence = fence;
		return B_OK;
	}

	syncobj_fence* fence = object_find_locked(object, point);
	if (fence == NULL)
		return B_BAD_VALUE;
	fence_acquire(fence);
	*_fence = fence;
	return B_OK;
}


static status_t
api_replace_fence(syncobj_file* file, uint32 handle, uint64 point,
	syncobj_fence* fence)
{
	if (file == NULL || (point != 0 && fence == NULL))
		return B_BAD_VALUE;
	RecursiveLocker locker(sLock);
	SyncObj* object = file_lookup_locked(file, handle);
	if (object == NULL)
		return B_ENTRY_NOT_FOUND;
	if (point != 0)
		return object_add_point_locked(object, fence, point);
	object_replace_locked(object, fence);
	return B_OK;
}


static status_t
api_get_sync_file_fence(int fd, syncobj_fence** _fence)
{
	if (_fence == NULL)
		return B_BAD_VALUE;
	file_descriptor* descriptor = get_typed_fd(fd, &sSyncFileFDOps, false);
	if (descriptor == NULL)
		return B_BAD_VALUE;
	syncobj_fence* fence = ((SyncFile*)descriptor->cookie)->fence;
	fence_acquire(fence);
	put_fd(descriptor);
	*_fence = fence;
	return B_OK;
}


static int
api_create_sync_file(syncobj_fence* fence, const char* name, bool closeOnExec)
{
	if (fence == NULL)
		return B_BAD_VALUE;
	return create_sync_file(fence, name, false, closeOnExec);
}


//	#pragma mark - module


static status_t
syncobj_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			recursive_lock_init(&sLock, "syncobj");
			sChanged.Init(&sChanged, "syncobj changed");
			sEventfdWatches = NULL;
			sSyncFiles = NULL;
			sFenceCount = 0;
			return B_OK;

		case B_MODULE_UNINIT:
			// B_KEEP_LOADED: only reached at shutdown
			if (sFenceCount != 0) {
				dprintf("syncobj: %" B_PRId32 " fences still alive\n",
					sFenceCount);
			}
			recursive_lock_destroy(&sLock);
			return B_OK;
	}
	return B_ERROR;
}


static syncobj_module_info sSyncObjModule = {
	{
		B_SYNCOBJ_MODULE_NAME,
		B_KEEP_LOADED,
			// descriptors and drivers' objects point into this module
		syncobj_std_ops
	},

	file_open,
	file_close,
	is_syncobj_ioctl,
	syncobj_ioctl,

	fence_create,
	fence_acquire,
	fence_release,
	api_signal_fence,
	api_fence_status,
	api_wait_fence,
	api_add_fence_callback,
	api_remove_fence_callback,

	api_find_fence,
	api_replace_fence,

	api_get_sync_file_fence,
	api_create_sync_file
};


module_info* modules[] = {
	(module_info*)&sSyncObjModule,
	NULL
};
