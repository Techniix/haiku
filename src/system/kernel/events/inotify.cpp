/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	inotify: Linux's file system event notification (inotify(7)). The user
	API lives in libgnu (headers/compatibility/gnu/sys/inotify.h); this is
	the kernel object behind it.

	Where the events come from
	--------------------------
	Haiku's node monitoring (fs/node_monitor.cpp) reports name changes
	(entry created, removed, moved, with names) and attribute changes, from
	the file systems themselves. inotify uses it for those: every watch is
	a kernel NotificationListener registered with add_node_listener() on
	the watched node, with B_WATCH_DIRECTORY for directories (entries
	created, removed, moved in and out), B_WATCH_NAME (the node's own
	removal and moves), B_WATCH_ATTR and, for directories,
	B_WATCH_CHILDREN (attribute changes of entries).

	Node monitoring has no events for reads, writes, opens and closes, and
	its stat-change events are rate-limited by the file systems (BFS sends
	an interim one at most every few seconds while a file grows). Linux
	reports IN_MODIFY for every write, so the VFS reports those directly
	(vfs.cpp calls inotify_node_event()): IN_ACCESS on read, IN_MODIFY on
	write and truncation, IN_ATTRIB on other stat changes, IN_OPEN,
	IN_CLOSE_WRITE and IN_CLOSE_NOWRITE. These hooks cost one atomic read
	while no watch exists anywhere.

	Names of directory entries
	--------------------------
	A VFS hook knows only the vnode; inotify has to report events of a
	watched directory's entries with the entry's name. Every directory
	watch therefore keeps the names of its entries: read once when the
	watch is added (vfs_read_dir_entries(), which does not create a file
	descriptor, so it causes no events itself) and kept current by the
	create/remove/move events. All watched nodes and entries are in one
	hash table keyed by (device, node), so a VFS hook finds the watch on
	the node itself and the directory watches that list it in one lookup.

	Lifetime and locking
	--------------------
	 - sInotifyLock (global mutex): the hash table, every watch's entry
	   list and state. Taken by node monitoring listeners, which run with
	   the node monitor's lock and file system locks held, so nothing
	   below it calls into a file system or node monitoring.
	 - Instance::fLock: the event queue, waiters and select pool.
	 Order: node monitor lock -> sInotifyLock -> Instance::fLock.
	 - A watch ends (IN_IGNORED) on inotify_rm_watch(), after IN_ONESHOT,
	   or when the watched node is deleted. It is unhashed at once, but its
	   node listener is removed later, from a syscall of the instance (or
	   its close), since listeners cannot be removed while node monitoring
	   is delivering to them. remove_node_listener() waits for a delivery in
	   progress, after which the watch can be freed.

	Queue
	-----
	Linux's rules: an event identical to the last queued one is merged
	into it; at most kMaxQueuedEvents are queued, and then a single
	IN_Q_OVERFLOW (wd -1) takes the last place and later events are
	dropped until read() makes room. Moves are reported as IN_MOVED_FROM
	and IN_MOVED_TO with one cookie: node monitoring delivers one message
	per move to all listeners and then calls AllListenersNotified(), so the
	cookie is allocated on the first delivery and reset after the last.
*/


#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>

#include <AutoDeleter.h>
#include <AutoDeleterDrivers.h>
#include <condition_variable.h>
#include <fs/fd.h>
#include <fs/node_monitor.h>
#include <fs/select_sync_pool.h>
#include <inotify.h>
#include <kernel.h>
#include <fs/KPath.h>
#include <lock.h>
#include <Notifications.h>
#include <NodeMonitor.h>
#include <syscalls.h>
#include <syscall_restart.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <util/KMessage.h>
#include <vfs.h>
#include <wait_for_objects.h>

// The user ABI is the Linux one, defined once in the libgnu header (which
// only needs <fcntl.h> and <stdint.h>); see epoll.cpp for why it is
// included by path.
#include "../../../../headers/compatibility/gnu/sys/inotify.h"


static_assert(INOTIFY_ACCESS == IN_ACCESS && INOTIFY_MODIFY == IN_MODIFY
	&& INOTIFY_ATTRIB == IN_ATTRIB && INOTIFY_CLOSE_WRITE == IN_CLOSE_WRITE
	&& INOTIFY_CLOSE_NOWRITE == IN_CLOSE_NOWRITE && INOTIFY_OPEN == IN_OPEN,
	"VFS event values must match the Linux ones");


//#define TRACE_INOTIFY
#ifdef TRACE_INOTIFY
#	define TRACE(x...) dprintf("inotify: " x)
#else
#	define TRACE(x...) do {} while (false)
#endif


// Linux defaults (/proc/sys/fs/inotify/max_queued_events and the old
// max_user_watches).
static const uint32 kMaxQueuedEvents = 16384;
static const int32 kMaxWatchesPerInstance = 8192;
// Names a directory watch learns when it is added, at most.
static const int32 kMaxSeededEntries = 100000;
// ... of which so many are stat()ed to tell directories apart.
static const int32 kMaxStattedEntries = 10000;

static const uint32 kWatchFlags = IN_ALL_EVENTS | IN_ONLYDIR | IN_DONT_FOLLOW
	| IN_EXCL_UNLINK | IN_MASK_CREATE | IN_MASK_ADD | IN_ONESHOT;

static const size_t kHashSize = 1024;


int32 gInotifyWatchCount = 0;


class InotifyInstance;
class InotifyWatch;


struct InotifyEvent : DoublyLinkedListLinkImpl<InotifyEvent> {
	int32		wd;
	uint32		mask;
	uint32		cookie;
	uint32		length;		// name bytes including padding (0: no name)
	char		name[];

	size_t RecordSize() const
		{ return sizeof(struct inotify_event) + length; }
};

typedef DoublyLinkedList<InotifyEvent> InotifyEventList;


/*!	A (device, node) a watch is interested in: the watched node itself
	(name == NULL), or an entry of a watched directory.
*/
struct InotifyNodeRef {
	InotifyNodeRef*	hashNext;
	dev_t			device;
	ino_t			node;
	InotifyWatch*	watch;
	char*			name;
	bool			isDirectory;
		// for entries: known to be a directory (for IN_ISDIR)
	DoublyLinkedListLink<InotifyNodeRef> watchLink;
};

typedef DoublyLinkedList<InotifyNodeRef,
	DoublyLinkedListMemberGetLink<InotifyNodeRef, &InotifyNodeRef::watchLink> >
	InotifyNodeRefList;


static mutex sInotifyLock = MUTEX_INITIALIZER("inotify");
static InotifyNodeRef* sNodeRefs[kHashSize];
static uint32 sNextMoveCookie = 1;
static uint32 sCurrentMoveCookie = 0;
static int32 sMonitorsReserved = 0;


static inline size_t
node_hash(dev_t device, ino_t node)
{
	return ((uint64)node * 31 + (uint32)device) % kHashSize;
}


static void
hash_insert(InotifyNodeRef* ref)
{
	size_t index = node_hash(ref->device, ref->node);
	ref->hashNext = sNodeRefs[index];
	sNodeRefs[index] = ref;
}


static void
hash_remove(InotifyNodeRef* ref)
{
	InotifyNodeRef** link = &sNodeRefs[node_hash(ref->device, ref->node)];
	while (*link != NULL && *link != ref)
		link = &(*link)->hashNext;
	if (*link == ref)
		*link = ref->hashNext;
}


class InotifyWatch : public NotificationListener,
	public DoublyLinkedListLinkImpl<InotifyWatch> {
public:
								InotifyWatch(InotifyInstance* instance,
									int32 wd, dev_t device, ino_t node,
									bool isDirectory, uint32 mask);
	virtual						~InotifyWatch();

	virtual	void				EventOccurred(NotificationService& service,
									const KMessage* event);
	virtual	void				AllListenersNotified(
									NotificationService& service);

			// all sInotifyLock
			void				Activate();
			void				End(bool notify);
			bool				AddEntry(ino_t node, const char* name,
									bool isDirectory);
			bool				RemoveEntry(ino_t node, const char* name);
			void				Deliver(uint32 mask, uint32 cookie,
									const char* name);

			InotifyInstance*	instance;
			int32				wd;
			dev_t				device;
			ino_t				node;
			bool				isDirectory;
			uint32				mask;
			bool				active;
				// delivering events (sInotifyLock)
			bool				ended;
				// no more events or entry updates (sInotifyLock)
			bool				listening;
				// registered with node monitoring
			int32				entryCount;

private:
			InotifyNodeRef		fSelf;
			InotifyNodeRefList	fEntries;
};

typedef DoublyLinkedList<InotifyWatch> InotifyWatchList;


class InotifyInstance {
public:
								InotifyInstance();
								~InotifyInstance();

			// fLock
			void				Queue(int32 wd, uint32 mask, uint32 cookie,
									const char* name);
			status_t			Read(void* buffer, size_t* _length,
									bool nonBlocking);
			status_t			Select(uint8 event, selectsync* sync);
			status_t			Deselect(uint8 event, selectsync* sync);
			size_t				QueuedBytes();

			// syscalls
			status_t			AddWatch(const char* userPath, uint32 mask,
									int32* _wd);
			status_t			RemoveWatch(int32 wd);
			void				Close();
			void				ReapWatches();

			// sInotifyLock
			InotifyWatch*		WatchFor(dev_t device, ino_t node);

private:
			mutex				fLock;
			ConditionVariable	fCondition;
			InotifyEventList	fEvents;
			uint32				fQueuedCount;
			size_t				fQueuedBytes;
			select_sync_pool*	fSelectPool;
			bool				fClosing;

			// sInotifyLock
			InotifyWatchList	fWatches;
			int32				fWatchCount;
			int32				fNextWD;
			mutex				fControlLock;
				// serializes add/remove/reap/close
};


// #pragma mark - InotifyWatch


InotifyWatch::InotifyWatch(InotifyInstance* instance, int32 wd, dev_t device,
	ino_t node, bool isDirectory, uint32 mask)
	:
	instance(instance),
	wd(wd),
	device(device),
	node(node),
	isDirectory(isDirectory),
	mask(mask),
	active(false),
	ended(false),
	listening(false),
	entryCount(0)
{
	fSelf.hashNext = NULL;
	fSelf.device = device;
	fSelf.node = node;
	fSelf.watch = this;
	fSelf.name = NULL;
	fSelf.isDirectory = isDirectory;
}


InotifyWatch::~InotifyWatch()
{
	ASSERT(!active);
	while (InotifyNodeRef* ref = fEntries.RemoveHead()) {
		free(ref->name);
		delete ref;
	}
}


void
InotifyWatch::Activate()
{
	hash_insert(&fSelf);
	for (InotifyNodeRefList::Iterator it = fEntries.GetIterator();
			it.HasNext();) {
		hash_insert(it.Next());
	}
	active = true;
	atomic_add(&gInotifyWatchCount, 1);
}


/*!	Ends the watch: no more events, and IN_IGNORED if \a notify. Its node
	listener is removed later by InotifyInstance::ReapWatches().
*/
void
InotifyWatch::End(bool notify)
{
	if (!active)
		return;

	active = false;
	ended = true;
	atomic_add(&gInotifyWatchCount, -1);
	hash_remove(&fSelf);
	for (InotifyNodeRefList::Iterator it = fEntries.GetIterator();
			it.HasNext();) {
		hash_remove(it.Next());
	}
	if (notify)
		instance->Queue(wd, IN_IGNORED, 0, NULL);
}


bool
InotifyWatch::AddEntry(ino_t entryNode, const char* name, bool entryIsDir)
{
	for (InotifyNodeRefList::Iterator it = fEntries.GetIterator();
			it.HasNext();) {
		InotifyNodeRef* ref = it.Next();
		if (ref->node == entryNode && strcmp(ref->name, name) == 0) {
			ref->isDirectory |= entryIsDir;
			return true;
		}
	}

	InotifyNodeRef* ref = new(std::nothrow) InotifyNodeRef;
	if (ref == NULL)
		return false;
	ref->name = strdup(name);
	if (ref->name == NULL) {
		delete ref;
		return false;
	}
	ref->hashNext = NULL;
	ref->device = device;
	ref->node = entryNode;
	ref->watch = this;
	ref->isDirectory = entryIsDir;
	fEntries.Add(ref);
	entryCount++;
	if (active)
		hash_insert(ref);
	return true;
}


/*!	Returns whether the entry was known to be a directory. */
bool
InotifyWatch::RemoveEntry(ino_t entryNode, const char* name)
{
	for (InotifyNodeRefList::Iterator it = fEntries.GetIterator();
			it.HasNext();) {
		InotifyNodeRef* ref = it.Next();
		if (ref->node != entryNode || strcmp(ref->name, name) != 0)
			continue;

		it.Remove();
		if (active)
			hash_remove(ref);
		bool wasDirectory = ref->isDirectory;
		free(ref->name);
		delete ref;
		entryCount--;
		return wasDirectory;
	}
	return false;
}


/*!	Queues an event of this watch, if it asked for it. */
void
InotifyWatch::Deliver(uint32 event, uint32 cookie, const char* name)
{
	if (!active || (mask & event & IN_ALL_EVENTS) == 0)
		return;

	instance->Queue(wd, event, cookie, name);

	if ((mask & IN_ONESHOT) != 0)
		End(true);
}


void
InotifyWatch::EventOccurred(NotificationService& service,
	const KMessage* event)
{
	int32 opcode = event->GetInt32("opcode", -1);
	ino_t eventNode = event->GetInt64("node", -1);
	bool entryIsDirectory = false;

	if (opcode == B_ENTRY_CREATED || opcode == B_ENTRY_REMOVED
		|| opcode == B_ENTRY_MOVED) {
		// Looks only at the vnode cache: the entry's node is there while
		// it is created, removed or moved.
		dev_t nodeDevice = event->GetInt32("node device",
			event->GetInt32("device", -1));
		entryIsDirectory = vfs_cached_vnode_is_directory(nodeDevice,
			eventNode);
	}
	uint32 isDir = entryIsDirectory ? IN_ISDIR : 0;

	MutexLocker locker(sInotifyLock);
	if (ended)
		return;
	// Before activation (while AddWatch() reads the directory) the entry
	// list is kept current, but Deliver() reports nothing.

	switch (opcode) {
		case B_ENTRY_CREATED:
		{
			const char* name = event->GetString("name", NULL);
			if (event->GetInt64("directory", -1) != node || name == NULL)
				break;
			AddEntry(eventNode, name, entryIsDirectory);
			Deliver(IN_CREATE | isDir, 0, name);
			break;
		}

		case B_ENTRY_REMOVED:
		{
			const char* name = event->GetString("name", NULL);
			if (event->GetInt64("directory", -1) == node && name != NULL) {
				// The node is usually gone from the vnode cache by now;
				// the entry list remembers what it was.
				if (RemoveEntry(eventNode, name))
					isDir = IN_ISDIR;
				Deliver(IN_DELETE | isDir, 0, name);
			}
			if (eventNode == node) {
				Deliver(IN_DELETE_SELF, 0, NULL);
				End(true);
			}
			break;
		}

		case B_ENTRY_MOVED:
		{
			const char* fromName = event->GetString("from name", NULL);
			const char* toName = event->GetString("name", NULL);
			if (fromName == NULL || toName == NULL)
				break;
			if (sCurrentMoveCookie == 0) {
				sCurrentMoveCookie = sNextMoveCookie++;
				if (sNextMoveCookie == 0)
					sNextMoveCookie = 1;
			}
			uint32 cookie = sCurrentMoveCookie;

			if (event->GetInt64("from directory", -1) == node) {
				if (RemoveEntry(eventNode, fromName))
					isDir = IN_ISDIR;
				Deliver(IN_MOVED_FROM | isDir, cookie, fromName);
			}
			if (event->GetInt64("to directory", -1) == node) {
				AddEntry(eventNode, toName, isDir != 0);
				Deliver(IN_MOVED_TO | isDir, cookie, toName);
			}
			if (eventNode == node)
				Deliver(IN_MOVE_SELF, 0, NULL);
			break;
		}

		case B_ATTR_CHANGED:
		{
			if (eventNode == node) {
				Deliver(IN_ATTRIB | (isDirectory ? IN_ISDIR : 0), 0, NULL);
				break;
			}
			if (!isDirectory)
				break;
			// An entry's attribute: report it under each of its names.
			for (InotifyNodeRef* ref = sNodeRefs[node_hash(device,
					eventNode)]; ref != NULL; ref = ref->hashNext) {
				if (ref->watch == this && ref->node == eventNode
					&& ref->name != NULL) {
					Deliver(IN_ATTRIB, 0, ref->name);
					break;
				}
			}
			break;
		}
	}
}


void
InotifyWatch::AllListenersNotified(NotificationService& service)
{
	// One message has been delivered to every listener: the next move
	// gets a new cookie.
	MutexLocker locker(sInotifyLock);
	sCurrentMoveCookie = 0;
}


// #pragma mark - InotifyInstance


InotifyInstance::InotifyInstance()
	:
	fQueuedCount(0),
	fQueuedBytes(0),
	fSelectPool(NULL),
	fClosing(false),
	fWatchCount(0),
	fNextWD(1)
{
	mutex_init(&fLock, "inotify instance");
	mutex_init(&fControlLock, "inotify control");
	fCondition.Init(this, "inotify read");
}


InotifyInstance::~InotifyInstance()
{
	while (InotifyEvent* event = fEvents.RemoveHead())
		free(event);
	delete_select_sync_pool(fSelectPool);
	mutex_destroy(&fLock);
	mutex_destroy(&fControlLock);
}


void
InotifyInstance::Queue(int32 wd, uint32 mask, uint32 cookie, const char* name)
{
	MutexLocker locker(fLock);
	if (fClosing)
		return;

	size_t nameLength = name != NULL ? strlen(name) : 0;
	// Linux pads the name (with its NUL) to a multiple of the record size.
	uint32 length = nameLength == 0 ? 0
		: (nameLength + 1 + sizeof(struct inotify_event) - 1)
			/ sizeof(struct inotify_event) * sizeof(struct inotify_event);

	// Merge with an identical last event, as Linux does.
	InotifyEvent* last = fEvents.Last();
	if (last != NULL && last->wd == wd && last->mask == mask
		&& last->cookie == cookie && last->length == length
		&& (length == 0 || strcmp(last->name, name) == 0)) {
		return;
	}

	bool overflow = false;
	if (fQueuedCount >= kMaxQueuedEvents)
		return;
	if (fQueuedCount == kMaxQueuedEvents - 1) {
		// The last place goes to the overflow event.
		overflow = true;
		wd = -1;
		mask = IN_Q_OVERFLOW;
		cookie = 0;
		length = 0;
	}

	InotifyEvent* event = (InotifyEvent*)malloc(sizeof(InotifyEvent)
		+ length);
	if (event == NULL) {
		// Nothing better to do than to drop it; tell the reader.
		if (last == NULL || last->mask != IN_Q_OVERFLOW) {
			event = (InotifyEvent*)malloc(sizeof(InotifyEvent));
			if (event == NULL)
				return;
			overflow = true;
			wd = -1;
			mask = IN_Q_OVERFLOW;
			cookie = 0;
			length = 0;
		} else
			return;
	}

	event->wd = wd;
	event->mask = mask;
	event->cookie = cookie;
	event->length = length;
	if (length > 0) {
		memset(event->name, 0, length);
		memcpy(event->name, name, nameLength);
	}
	(void)overflow;

	fEvents.Add(event);
	fQueuedCount++;
	fQueuedBytes += event->RecordSize();

	fCondition.NotifyAll();
	notify_select_event_pool(fSelectPool, B_SELECT_READ);
}


status_t
InotifyInstance::Read(void* buffer, size_t* _length, bool nonBlocking)
{
	MutexLocker locker(fLock);

	while (fEvents.IsEmpty()) {
		if (fClosing)
			return B_FILE_ERROR;
		if (nonBlocking)
			return B_WOULD_BLOCK;
		status_t status = fCondition.Wait(&fLock, B_CAN_INTERRUPT);
		if (status != B_OK)
			return status;
	}

	// Whole records only; the first one must fit (EINVAL otherwise, as on
	// Linux).
	size_t size = *_length;
	size_t done = 0;
	while (InotifyEvent* event = fEvents.Head()) {
		size_t recordSize = event->RecordSize();
		if (done + recordSize > size)
			break;

		struct inotify_event header;
		header.wd = event->wd;
		header.mask = event->mask;
		header.cookie = event->cookie;
		header.len = event->length;
		uint8* target = (uint8*)buffer + done;
		if (user_memcpy(target, &header, sizeof(header)) != B_OK
			|| (event->length > 0
				&& user_memcpy(target + sizeof(header), event->name,
					event->length) != B_OK)) {
			if (done == 0)
				return B_BAD_ADDRESS;
			break;
		}

		fEvents.Remove(event);
		fQueuedCount--;
		fQueuedBytes -= recordSize;
		free(event);
		done += recordSize;
	}

	if (done == 0)
		return B_BAD_VALUE;

	*_length = done;
	return B_OK;
}


size_t
InotifyInstance::QueuedBytes()
{
	MutexLocker locker(fLock);
	return fQueuedBytes;
}


status_t
InotifyInstance::Select(uint8 event, selectsync* sync)
{
	MutexLocker locker(fLock);
	status_t status = add_select_sync_pool_entry(&fSelectPool, sync, event);
	if (status != B_OK)
		return status;
	if (event == B_SELECT_READ && !fEvents.IsEmpty())
		return notify_select_event(sync, event);
	return B_OK;
}


status_t
InotifyInstance::Deselect(uint8 event, selectsync* sync)
{
	MutexLocker locker(fLock);
	remove_select_sync_pool_entry(&fSelectPool, sync, event);
	return B_OK;
}


InotifyWatch*
InotifyInstance::WatchFor(dev_t device, ino_t node)
{
	for (InotifyWatchList::Iterator it = fWatches.GetIterator();
			it.HasNext();) {
		InotifyWatch* watch = it.Next();
		if (watch->active && watch->device == device && watch->node == node)
			return watch;
	}
	return NULL;
}


/*!	Removes the node listeners of ended watches and frees them. Must be
	called without any inotify lock held, from a syscall of the instance.
*/
void
InotifyInstance::ReapWatches()
{
	InotifyWatchList ended;
	{
		MutexLocker locker(sInotifyLock);
		InotifyWatchList::Iterator it = fWatches.GetIterator();
		while (InotifyWatch* watch = it.Next()) {
			if (!watch->active) {
				it.Remove();
				fWatchCount--;
				ended.Add(watch);
			}
		}
	}

	while (InotifyWatch* watch = ended.RemoveHead()) {
		// Waits for a delivery to this listener that is in progress.
		if (watch->listening)
			remove_node_listener(watch->device, watch->node, *watch);
		delete watch;
	}
}


struct SeedContext {
	InotifyWatch*	watch;
	int32			count;
};


static bool
seed_entry(void* data, const struct dirent* entry)
{
	SeedContext* context = (SeedContext*)data;
	if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
		return true;
	if (++context->count > kMaxSeededEntries)
		return false;

	// Remember which entries are directories, for IN_ISDIR on their
	// removal (by then they are no longer in the vnode cache). Stat only
	// so many; in larger directories the rest go without.
	bool isDirectory = false;
	struct stat st;
	if (context->count <= kMaxStattedEntries
		&& vfs_stat_node_ref(context->watch->device, entry->d_ino, &st)
			== B_OK) {
		isDirectory = S_ISDIR(st.st_mode);
	}

	MutexLocker locker(sInotifyLock);
	return context->watch->AddEntry(entry->d_ino, entry->d_name,
		isDirectory);
}


status_t
InotifyInstance::AddWatch(const char* userPath, uint32 mask, int32* _wd)
{
	if ((mask & ~kWatchFlags) != 0 || (mask & IN_ALL_EVENTS) == 0)
		return B_BAD_VALUE;
	if ((mask & (IN_MASK_ADD | IN_MASK_CREATE))
			== (IN_MASK_ADD | IN_MASK_CREATE)) {
		return B_BAD_VALUE;
	}

	if (userPath == NULL || !IS_USER_ADDRESS(userPath))
		return B_BAD_ADDRESS;
	KPath path;
	if (path.InitCheck() != B_OK)
		return B_NO_MEMORY;
	ssize_t pathLength = user_strlcpy(path.LockBuffer(), userPath,
		path.BufferSize());
	if (pathLength < 0)
		return B_BAD_ADDRESS;
	if ((size_t)pathLength >= path.BufferSize())
		return B_NAME_TOO_LONG;
	path.UnlockBuffer();

	struct stat st;
	status_t status = vfs_read_stat(-1, path.Path(),
		(mask & IN_DONT_FOLLOW) == 0, &st, false);
	if (status != B_OK)
		return status;
	if ((mask & IN_ONLYDIR) != 0 && !S_ISDIR(st.st_mode))
		return B_NOT_A_DIRECTORY;

	// Linux needs read permission on the watched node.
	status = _user_access(-1, userPath, R_OK, true);
	if (status != B_OK)
		return status;

	MutexLocker controlLocker(fControlLock);
	if (fClosing)
		return B_FILE_ERROR;

	{
		MutexLocker locker(sInotifyLock);
		if (InotifyWatch* existing = WatchFor(st.st_dev, st.st_ino)) {
			if ((mask & IN_MASK_CREATE) != 0)
				return B_FILE_EXISTS;
			uint32 newMask = mask & (IN_ALL_EVENTS | IN_ONESHOT
				| IN_EXCL_UNLINK);
			if ((mask & IN_MASK_ADD) != 0)
				newMask |= existing->mask;
			existing->mask = newMask;
			*_wd = existing->wd;
			return B_OK;
		}
		if (fWatchCount >= kMaxWatchesPerInstance)
			return ENOSPC;
	}

	if (!atomic_test_and_set(&sMonitorsReserved, 1, 0)) {
		// All node listeners of the kernel count against the kernel team's
		// node monitor limit; make room for inotify watches.
		vfs_reserve_kernel_node_monitors(MAX_NODE_MONITORS);
	}

	InotifyWatch* watch = new(std::nothrow) InotifyWatch(this, fNextWD,
		st.st_dev, st.st_ino, S_ISDIR(st.st_mode),
		mask & (IN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK));
	if (watch == NULL)
		return B_NO_MEMORY;

	uint32 flags = B_WATCH_NAME | B_WATCH_ATTR;
	if (watch->isDirectory)
		flags |= B_WATCH_DIRECTORY | B_WATCH_CHILDREN;
	status = add_node_listener(st.st_dev, st.st_ino, flags, *watch);
	if (status != B_OK) {
		delete watch;
		return status == B_NO_MEMORY ? ENOSPC : status;
	}
	watch->listening = true;

	// Learn the entries' names before events can arrive for them; entry
	// events that arrive meanwhile are not delivered (the watch is not
	// active yet) but do update the list.
	if (watch->isDirectory) {
		SeedContext context = { watch, 0 };
		vfs_read_dir_entries(st.st_dev, st.st_ino, &seed_entry, &context);
	}

	MutexLocker locker(sInotifyLock);
	watch->wd = fNextWD++;
	if (fNextWD == INT32_MAX)
		fNextWD = 1;
	fWatches.Add(watch);
	fWatchCount++;
	watch->Activate();
	*_wd = watch->wd;

	TRACE("%p: wd %" B_PRId32 " on %" B_PRIdDEV ":%" B_PRIdINO ", mask %#"
		B_PRIx32 ", %" B_PRId32 " entries\n", this, watch->wd, st.st_dev,
		st.st_ino, mask, watch->entryCount);
	return B_OK;
}


status_t
InotifyInstance::RemoveWatch(int32 wd)
{
	MutexLocker controlLocker(fControlLock);
	{
		MutexLocker locker(sInotifyLock);
		InotifyWatch* watch = NULL;
		for (InotifyWatchList::Iterator it = fWatches.GetIterator();
				it.HasNext();) {
			InotifyWatch* candidate = it.Next();
			if (candidate->wd == wd && candidate->active) {
				watch = candidate;
				break;
			}
		}
		if (watch == NULL)
			return B_BAD_VALUE;
		watch->End(true);
	}
	ReapWatches();
	return B_OK;
}


void
InotifyInstance::Close()
{
	MutexLocker controlLocker(fControlLock);
	{
		MutexLocker locker(sInotifyLock);
		for (InotifyWatchList::Iterator it = fWatches.GetIterator();
				it.HasNext();) {
			it.Next()->End(false);
		}
	}
	ReapWatches();

	MutexLocker locker(fLock);
	fClosing = true;
	fCondition.NotifyAll(B_FILE_ERROR);
}


// #pragma mark - VFS hook


void
inotify_notify_node(dev_t device, ino_t node, bool isDirectory, uint32 events)
{
	uint32 isDir = isDirectory ? IN_ISDIR : 0;

	MutexLocker locker(sInotifyLock);
	InotifyNodeRef* next;
	for (InotifyNodeRef* ref = sNodeRefs[node_hash(device, node)]; ref != NULL;
			ref = next) {
		next = ref->hashNext;
			// Deliver() may end a one-shot watch, which unhashes ref
		if (ref->device != device || ref->node != node)
			continue;
		for (uint32 event = 1; event <= IN_OPEN; event <<= 1) {
			if ((events & event) != 0)
				ref->watch->Deliver(event | isDir, 0, ref->name);
		}
	}
}


// #pragma mark - fd hooks


static status_t
inotify_close(file_descriptor* descriptor)
{
	((InotifyInstance*)descriptor->cookie)->Close();
	return B_OK;
}


static void
inotify_free(file_descriptor* descriptor)
{
	delete (InotifyInstance*)descriptor->cookie;
}


static status_t
inotify_read(file_descriptor* descriptor, off_t pos, void* buffer,
	size_t* _length)
{
	InotifyInstance* instance = (InotifyInstance*)descriptor->cookie;
	instance->ReapWatches();
	return instance->Read(buffer, _length,
		(descriptor->open_mode & O_NONBLOCK) != 0);
}


static status_t
inotify_write(file_descriptor* descriptor, off_t pos, const void* buffer,
	size_t* _length)
{
	return B_BAD_VALUE;
}


static status_t
inotify_ioctl(file_descriptor* descriptor, ulong op, void* buffer,
	size_t length)
{
	if (op != FIONREAD)
		return B_DEV_INVALID_IOCTL;

	int bytes = (int)min_c(((InotifyInstance*)descriptor->cookie)
		->QueuedBytes(), (size_t)INT_MAX);
	if (is_called_via_syscall()) {
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(buffer, &bytes, sizeof(bytes)) != B_OK) {
			return B_BAD_ADDRESS;
		}
	} else
		*(int*)buffer = bytes;
	return B_OK;
}


static status_t
inotify_set_flags(file_descriptor* descriptor, int flags)
{
	return B_OK;
}


static status_t
inotify_select(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	return ((InotifyInstance*)descriptor->cookie)->Select(event, sync);
}


static status_t
inotify_deselect(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	return ((InotifyInstance*)descriptor->cookie)->Deselect(event, sync);
}


static status_t
inotify_read_stat(file_descriptor* descriptor, struct stat* stat)
{
	memset(stat, 0, sizeof(*stat));
	stat->st_mode = S_IFIFO | S_IRUSR;
	stat->st_nlink = 1;
	return B_OK;
}


static struct fd_ops sInotifyOps = {
	&inotify_close,
	&inotify_free,
	&inotify_read,
	&inotify_write,
	NULL,	// readv
	NULL,	// writev
	NULL,	// seek
	&inotify_ioctl,
	&inotify_set_flags,
	&inotify_select,
	&inotify_deselect,
	NULL,	// read_dir
	NULL,	// rewind_dir
	&inotify_read_stat,
	NULL,	// write_stat
};


// #pragma mark - syscalls


static status_t
get_instance(int fd, file_descriptor*& descriptor, InotifyInstance*& instance)
{
	descriptor = get_fd(get_current_io_context(false), fd);
	if (descriptor == NULL)
		return B_FILE_ERROR;
	if (descriptor->ops != &sInotifyOps) {
		put_fd(descriptor);
		return B_BAD_VALUE;
	}
	instance = (InotifyInstance*)descriptor->cookie;
	return B_OK;
}


int
_user_inotify_init(int openFlags)
{
	if ((openFlags & ~(O_CLOEXEC | O_CLOFORK | O_NONBLOCK)) != 0)
		return B_BAD_VALUE;

	InotifyInstance* instance = new(std::nothrow) InotifyInstance;
	if (instance == NULL)
		return B_NO_MEMORY;

	file_descriptor* descriptor = alloc_fd();
	if (descriptor == NULL) {
		delete instance;
		return B_NO_MEMORY;
	}

	descriptor->ops = &sInotifyOps;
	descriptor->cookie = instance;
	// O_RDWR, so that write() reaches our hook and fails with EINVAL, as
	// on Linux, rather than with EBADF.
	descriptor->open_mode = O_RDWR | (openFlags & O_NONBLOCK);

	io_context* context = get_current_io_context(false);
	rw_lock_write_lock(&context->lock);
	int fd = new_fd(context, descriptor);
	if (fd < 0) {
		rw_lock_write_unlock(&context->lock);
		descriptor->ops = NULL;
		descriptor->cookie = NULL;
		put_fd(descriptor);
		delete instance;
		return fd;
	}

	fd_set_close_on_exec(context, fd, (openFlags & O_CLOEXEC) != 0);
	fd_set_close_on_fork(context, fd, (openFlags & O_CLOFORK) != 0);
	rw_lock_write_unlock(&context->lock);

	return fd;
}


int
_user_inotify_add_watch(int fd, const char* userPath, uint32 mask)
{
	file_descriptor* descriptor;
	InotifyInstance* instance;
	status_t status = get_instance(fd, descriptor, instance);
	if (status != B_OK)
		return status;
	FileDescriptorPutter putter(descriptor);

	instance->ReapWatches();

	int32 wd;
	status = instance->AddWatch(userPath, mask, &wd);
	if (status != B_OK)
		return status;
	return wd;
}


status_t
_user_inotify_rm_watch(int fd, int wd)
{
	file_descriptor* descriptor;
	InotifyInstance* instance;
	status_t status = get_instance(fd, descriptor, instance);
	if (status != B_OK)
		return status;
	FileDescriptorPutter putter(descriptor);

	return instance->RemoveWatch(wd);
}
