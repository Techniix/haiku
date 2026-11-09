/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	POSIX message queues (<mqueue.h>): the kernel objects behind mq_open(),
	mq_send(), mq_receive(), mq_getattr()/mq_setattr(), mq_notify() and
	mq_unlink(). The libroot side is src/system/libroot/posix/mqueue.cpp.

	Model (the one Linux and FreeBSD use):
	 - A message queue descriptor (mqd_t) is a file descriptor. select(),
	   poll() and wait_for_objects() report B_SELECT_READ while messages are
	   queued and B_SELECT_WRITE while there is room, so event loops can wait
	   for queues next to sockets. Descriptors are inherited by fork() and
	   always closed on exec(), as POSIX requires.
	 - Queues live in one global namespace ("/name"), with owner, group and
	   mode checked on open, like named POSIX semaphores
	   (realtime_sem.cpp, whose name table this follows). mq_unlink() removes
	   the name at once; the queue itself lives until the last descriptor is
	   closed.
	 - Messages are kept sorted: highest priority first, oldest first within
	   a priority, so mq_receive() always takes the head.
	 - Blocking sends and receives wait on condition variables with
	   restartable absolute timeouts (CLOCK_REALTIME for mq_timed*()). They
	   are restarted after a signal handler installed with SA_RESTART, as on
	   Linux.
	 - mq_notify() reuses the kernel's user event objects (UserEvent.h),
	   the same ones POSIX timers use, for both SIGEV_SIGNAL (si_code
	   SI_MESGQ) and SIGEV_THREAD. The notification fires once, when a
	   message arrives in an empty queue and no receiver is waiting, and is
	   then removed. It is also removed when the registering descriptor is
	   closed, and silently dropped if the registering team has died.

	Limits (per queue, checked when creating): up to kMaxMessages messages
	of up to kMaxMessageSize bytes, and at most kMaxQueueBytes of message
	space in total; kMaxQueues queues system-wide. Message memory is
	allocated per message, so an empty queue costs a few hundred bytes no
	matter how large its limits are.
*/


#include <posix/mqueue.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mqueue.h>
#include <new>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <AutoDeleter.h>
#include <condition_variable.h>
#include <fs/fd.h>
#include <fs/select_sync_pool.h>
#include <kernel.h>
#include <ksignal.h>
#include <lock.h>
#include <Referenceable.h>
#include <syscall_restart.h>
#include <team.h>
#include <thread.h>
#include <UserEvent.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <util/OpenHashTable.h>
#include <util/StringHash.h>
#include <vfs.h>
#include <wait_for_objects.h>


//#define TRACE_MQUEUE
#ifdef TRACE_MQUEUE
#	define TRACE(x...) dprintf("mqueue: " x)
#else
#	define TRACE(x...) do {} while (false)
#endif


static const long kDefaultMaxMessages = 10;		// as Linux
static const long kDefaultMessageSize = 8192;	// as Linux
static const long kMaxMessages = 1024;
static const long kMaxMessageSize = 1024 * 1024;
static const size_t kMaxQueueBytes = 64 * 1024 * 1024;
static const int32 kMaxQueues = 1024;


struct QueuedMessage : DoublyLinkedListLinkImpl<QueuedMessage> {
	uint32	priority;
	size_t	size;
	uint8	data[];
};

typedef DoublyLinkedList<QueuedMessage> MessageList;


class MessageQueue : public BReferenceable {
public:
	MessageQueue(long maxMessages, long messageSize, mode_t mode)
		:
		fName(NULL),
		fUID(geteuid()),
		fGID(getegid()),
		fMode(mode & 0777),
		fMaxMessages(maxMessages),
		fMessageSize(messageSize),
		fCount(0),
		fWaitingReceivers(0),
		fSelectPool(NULL),
		fNotifyEvent(NULL),
		fNotifyDescriptor(NULL)
	{
		mutex_init(&fLock, "mqueue");
		fReadCondition.Init(this, "mqueue read");
		fWriteCondition.Init(this, "mqueue write");
	}

	~MessageQueue()
	{
		while (QueuedMessage* message = fMessages.RemoveHead())
			free(message);
		_ClearNotification();
		delete_select_sync_pool(fSelectPool);
		mutex_destroy(&fLock);
		free(fName);
	}

	status_t SetName(const char* name)
	{
		fName = strdup(name);
		return fName != NULL ? B_OK : B_NO_MEMORY;
	}

	const char* Name() const		{ return fName; }
	MessageQueue*& HashLink()		{ return fHashLink; }

	//!	Whether the current team may open the queue with "accessMode".
	bool HasAccess(int accessMode) const
	{
		uid_t uid = geteuid();
		if (uid == 0)
			return true;

		mode_t needed = 0;
		if (accessMode == O_RDONLY || accessMode == O_RDWR)
			needed |= S_IROTH;
		if (accessMode == O_WRONLY || accessMode == O_RDWR)
			needed |= S_IWOTH;

		mode_t granted;
		if (uid == fUID)
			granted = fMode >> 6;
		else if (getegid() == fGID)
			granted = fMode >> 3;
		else
			granted = fMode;

		return (granted & needed) == needed;
	}

	//!	Only the owner (or root) may unlink: /dev/mqueue is sticky on Linux.
	bool MayUnlink() const
	{
		uid_t uid = geteuid();
		return uid == 0 || uid == fUID;
	}

	status_t Send(const void* userBuffer, size_t length, uint32 priority,
		bool nonBlocking, uint32 flags, bigtime_t timeout);
	ssize_t Receive(void* userBuffer, size_t length, uint32* userPriority,
		bool nonBlocking, uint32 flags, bigtime_t timeout);

	void GetAttributes(mq_attr& attr);

	status_t SetNotification(file_descriptor* descriptor,
		const struct sigevent& event,
		const ThreadCreationAttributes* threadAttributes);
	void RemoveNotification(file_descriptor* descriptor, bool onClose);

	status_t Select(uint8 event, selectsync* sync);
	void Deselect(uint8 event, selectsync* sync);

	void Stat(struct stat& st);

private:
	status_t _WaitLocked(ConditionVariable& condition, uint32 flags,
		bigtime_t timeout);
	void _ClearNotification();

private:
	mutex				fLock;
	ConditionVariable	fReadCondition;		// a message arrived
	ConditionVariable	fWriteCondition;	// room became available

	char*				fName;
	uid_t				fUID;
	gid_t				fGID;
	mode_t				fMode;

	long				fMaxMessages;
	long				fMessageSize;
	long				fCount;
	MessageList			fMessages;	// highest priority first, then FIFO
	int32				fWaitingReceivers;

	select_sync_pool*	fSelectPool;

	// mq_notify() registration
	UserEvent*			fNotifyEvent;
	BReference<Team>	fNotifyTeam;
	file_descriptor*	fNotifyDescriptor;

	MessageQueue*		fHashLink;
};


status_t
MessageQueue::_WaitLocked(ConditionVariable& condition, uint32 flags,
	bigtime_t timeout)
{
	// An absolute timeout of -1 is libroot's way of passing on an invalid
	// timespec: POSIX only reports it (EINVAL) when the call has to block.
	if ((flags & (B_ABSOLUTE_TIMEOUT | B_RELATIVE_TIMEOUT)) != 0
		&& timeout < 0) {
		return B_BAD_VALUE;
	}

	return condition.Wait(&fLock, flags | B_CAN_INTERRUPT, timeout);
}


status_t
MessageQueue::Send(const void* userBuffer, size_t length, uint32 priority,
	bool nonBlocking, uint32 flags, bigtime_t timeout)
{
	if (length > (size_t)fMessageSize)
		return EMSGSIZE;

	// Copy the message in before taking the lock: a page fault on the user
	// buffer must not stall the queue.
	QueuedMessage* message
		= (QueuedMessage*)malloc(sizeof(QueuedMessage) + length);
	if (message == NULL)
		return B_NO_MEMORY;
	MemoryDeleter messageDeleter(message);

	message->priority = priority;
	message->size = length;
	if (length > 0 && user_memcpy(message->data, userBuffer, length) != B_OK)
		return B_BAD_ADDRESS;

	MutexLocker locker(fLock);

	while (fCount >= fMaxMessages) {
		if (nonBlocking)
			return B_WOULD_BLOCK;

		status_t status = _WaitLocked(fWriteCondition, flags, timeout);
		if (status != B_OK)
			return status;
	}

	// Insert after the last message with the same or higher priority.
	QueuedMessage* before = fMessages.Tail();
	while (before != NULL && before->priority < priority)
		before = fMessages.GetPrevious(before);
	messageDeleter.Detach();
	if (before == NULL)
		fMessages.InsertBefore(fMessages.Head(), message);
	else
		fMessages.InsertAfter(before, message);

	bool wasEmpty = fCount == 0;
	fCount++;

	fReadCondition.NotifyAll();
	notify_select_event_pool(fSelectPool, B_SELECT_READ);

	// POSIX: notify only on the empty -> non-empty transition, and only if
	// no thread is already blocked in mq_receive().
	if (wasEmpty && fWaitingReceivers == 0 && fNotifyEvent != NULL) {
		Team* team = fNotifyTeam.Get();
		if (team->state < TEAM_STATE_SHUTDOWN) {
			TRACE("notifying team %" B_PRId32 "\n", team->id);
			fNotifyEvent->Fire();
		}
		_ClearNotification();
	}

	return B_OK;
}


ssize_t
MessageQueue::Receive(void* userBuffer, size_t length, uint32* userPriority,
	bool nonBlocking, uint32 flags, bigtime_t timeout)
{
	if (length < (size_t)fMessageSize)
		return EMSGSIZE;

	MutexLocker locker(fLock);

	while (fCount == 0) {
		if (nonBlocking)
			return B_WOULD_BLOCK;

		fWaitingReceivers++;
		status_t status = _WaitLocked(fReadCondition, flags, timeout);
		fWaitingReceivers--;
		if (status != B_OK)
			return status;
	}

	// Copy out while holding the lock, so that a bad buffer leaves the
	// message in the queue for the next receiver instead of losing it.
	QueuedMessage* message = fMessages.Head();
	if ((message->size > 0
			&& user_memcpy(userBuffer, message->data, message->size) != B_OK)
		|| (userPriority != NULL
			&& user_memcpy(userPriority, &message->priority,
				sizeof(uint32)) != B_OK)) {
		return B_BAD_ADDRESS;
	}

	fMessages.Remove(message);
	bool wasFull = fCount == fMaxMessages;
	fCount--;
	ssize_t size = message->size;
	free(message);

	if (wasFull) {
		fWriteCondition.NotifyAll();
		notify_select_event_pool(fSelectPool, B_SELECT_WRITE);
	}

	return size;
}


void
MessageQueue::GetAttributes(mq_attr& attr)
{
	MutexLocker locker(fLock);
	attr.mq_flags = 0;
	attr.mq_maxmsg = fMaxMessages;
	attr.mq_msgsize = fMessageSize;
	attr.mq_curmsgs = fCount;
}


void
MessageQueue::_ClearNotification()
{
	if (fNotifyEvent != NULL)
		fNotifyEvent->ReleaseReference();
	fNotifyEvent = NULL;
	fNotifyTeam.Unset();
	fNotifyDescriptor = NULL;
}


status_t
MessageQueue::SetNotification(file_descriptor* descriptor,
	const struct sigevent& event,
	const ThreadCreationAttributes* threadAttributes)
{
	Team* team = thread_get_current_thread()->team;

	MutexLocker locker(fLock);

	// A registration whose team has died since does not block a new one.
	if (fNotifyEvent != NULL
		&& fNotifyTeam.Get()->state >= TEAM_STATE_SHUTDOWN) {
		_ClearNotification();
	}
	if (fNotifyEvent != NULL)
		return B_BUSY;

	UserEvent* userEvent = NULL;
	switch (event.sigev_notify) {
		case SIGEV_NONE:
			// Registers the process without sending anything (POSIX).
			userEvent = NULL;
			break;

		case SIGEV_SIGNAL:
		{
			if (event.sigev_signo <= 0
				|| event.sigev_signo > MAX_SIGNAL_NUMBER) {
				return B_BAD_VALUE;
			}
			TeamSignalEvent* signalEvent = TeamSignalEvent::Create(team,
				event.sigev_signo, SI_MESGQ, 0);
			if (signalEvent == NULL)
				return B_NO_MEMORY;
			signalEvent->SetUserValue(event.sigev_value);
			userEvent = signalEvent;
			break;
		}

		case SIGEV_THREAD:
			if (threadAttributes == NULL)
				return B_BAD_VALUE;
			userEvent = CreateThreadEvent::Create(*threadAttributes);
			if (userEvent == NULL)
				return B_NO_MEMORY;
			break;

		default:
			return B_BAD_VALUE;
	}

	if (userEvent == NULL) {
		// SIGEV_NONE still occupies the registration slot. Use a dummy
		// thread-less event so that the slot logic stays uniform: an event
		// that does nothing when fired.
		struct NoneEvent : UserEvent {
			virtual status_t Fire() { return B_OK; }
		};
		userEvent = new(std::nothrow) NoneEvent;
		if (userEvent == NULL)
			return B_NO_MEMORY;
	}

	fNotifyEvent = userEvent;
	fNotifyTeam.SetTo(team);
	fNotifyDescriptor = descriptor;
	return B_OK;
}


void
MessageQueue::RemoveNotification(file_descriptor* descriptor, bool onClose)
{
	MutexLocker locker(fLock);
	if (fNotifyEvent == NULL)
		return;

	if (onClose) {
		// The registering descriptor is gone.
		if (fNotifyDescriptor == descriptor)
			_ClearNotification();
		return;
	}

	// mq_notify(NULL): only the registered process may remove it.
	if (fNotifyTeam.Get() == thread_get_current_thread()->team)
		_ClearNotification();
}


status_t
MessageQueue::Select(uint8 event, selectsync* sync)
{
	MutexLocker locker(fLock);

	status_t status = add_select_sync_pool_entry(&fSelectPool, sync, event);
	if (status != B_OK)
		return status;

	if ((event == B_SELECT_READ && fCount > 0)
		|| (event == B_SELECT_WRITE && fCount < fMaxMessages)) {
		return notify_select_event(sync, event);
	}
	return B_OK;
}


void
MessageQueue::Deselect(uint8 event, selectsync* sync)
{
	MutexLocker locker(fLock);
	remove_select_sync_pool_entry(&fSelectPool, sync, event);
}


void
MessageQueue::Stat(struct stat& st)
{
	MutexLocker locker(fLock);
	memset(&st, 0, sizeof(st));
	st.st_mode = S_IFREG | fMode;
	st.st_nlink = fName != NULL ? 1 : 0;
	st.st_uid = fUID;
	st.st_gid = fGID;
	st.st_size = fCount;
	st.st_blksize = fMessageSize;
}


//	#pragma mark - name table


struct MessageQueueHashDefinition {
	typedef const char*		KeyType;
	typedef MessageQueue	ValueType;

	size_t HashKey(const char* key) const
		{ return hash_hash_string(key); }
	size_t Hash(MessageQueue* queue) const
		{ return HashKey(queue->Name()); }
	bool Compare(const char* key, MessageQueue* queue) const
		{ return strcmp(key, queue->Name()) == 0; }
	MessageQueue*& GetLink(MessageQueue* queue) const
		{ return queue->HashLink(); }
};

typedef BOpenHashTable<MessageQueueHashDefinition, true> MessageQueueTable;

static mutex sTableLock = MUTEX_INITIALIZER("mqueue table");
static MessageQueueTable* sQueues;
static int32 sQueueCount;


//	#pragma mark - file descriptor ops


static status_t
mqueue_close(file_descriptor* descriptor)
{
	((MessageQueue*)descriptor->cookie)->RemoveNotification(descriptor, true);
	return B_OK;
}


static void
mqueue_free(file_descriptor* descriptor)
{
	((MessageQueue*)descriptor->cookie)->ReleaseReference();
}


static status_t
mqueue_set_flags(file_descriptor* descriptor, int flags)
{
	// O_NONBLOCK is read from descriptor->open_mode.
	return B_OK;
}


static status_t
mqueue_select(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	return ((MessageQueue*)descriptor->cookie)->Select(event, sync);
}


static status_t
mqueue_deselect(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	((MessageQueue*)descriptor->cookie)->Deselect(event, sync);
	return B_OK;
}


static status_t
mqueue_read_stat(file_descriptor* descriptor, struct stat* stat)
{
	((MessageQueue*)descriptor->cookie)->Stat(*stat);
	return B_OK;
}


static struct fd_ops sMessageQueueOps = {
	&mqueue_close,
	&mqueue_free,
	NULL,	// read
	NULL,	// write
	NULL,	// readv
	NULL,	// writev
	NULL,	// seek
	NULL,	// ioctl
	&mqueue_set_flags,
	&mqueue_select,
	&mqueue_deselect,
	NULL,	// read_dir
	NULL,	// rewind_dir
	&mqueue_read_stat,
	NULL,	// write_stat
};


//	#pragma mark - helpers


//!	Gets a message queue descriptor; with a non-zero "access", it must allow
//	that (O_RDONLY: receive, O_WRONLY: send). Returns EBADF otherwise.
static status_t
get_mqueue(int fd, int access, file_descriptor*& _descriptor,
	MessageQueue*& _queue)
{
	file_descriptor* descriptor = get_fd(get_current_io_context(false), fd);
	if (descriptor == NULL)
		return B_FILE_ERROR;

	int mode = descriptor->open_mode & O_ACCMODE;
	if (descriptor->ops != &sMessageQueueOps
		|| (access == O_RDONLY && mode == O_WRONLY)
		|| (access == O_WRONLY && mode == O_RDONLY)) {
		put_fd(descriptor);
		return B_FILE_ERROR;
	}

	_descriptor = descriptor;
	_queue = (MessageQueue*)descriptor->cookie;
	return B_OK;
}


struct DescriptorPutter {
	DescriptorPutter(file_descriptor* descriptor) : fDescriptor(descriptor) {}
	~DescriptorPutter() { put_fd(fDescriptor); }
	file_descriptor* fDescriptor;
};


/*!	Copies and checks a queue name: "/" followed by 1 to NAME_MAX - 1
	characters, none of them '/'. Linux and FreeBSD apply the same rules.
*/
static status_t
copy_queue_name(const char* userName, char* name, size_t size)
{
	if (userName == NULL || !IS_USER_ADDRESS(userName))
		return B_BAD_ADDRESS;

	ssize_t length = user_strlcpy(name, userName, size);
	if (length < 0)
		return B_BAD_ADDRESS;
	if ((size_t)length >= size)
		return ENAMETOOLONG;
	if (name[0] != '/' || name[1] == '\0' || strchr(name + 1, '/') != NULL)
		return B_BAD_VALUE;
	return B_OK;
}


//	#pragma mark - kernel internal


void
mqueue_init()
{
	sQueues = new(std::nothrow) MessageQueueTable;
	if (sQueues == NULL || sQueues->Init() != B_OK)
		panic("mqueue_init(): no memory for the queue table");
}


//	#pragma mark - syscalls


int
_user_mqueue_open(const char* userName, int openFlags, mode_t mode,
	const struct mq_attr* userAttr)
{
	char name[NAME_MAX + 1];
	status_t status = copy_queue_name(userName, name, sizeof(name));
	if (status != B_OK)
		return status;

	int accessMode = openFlags & O_ACCMODE;
	if (accessMode != O_RDONLY && accessMode != O_WRONLY
		&& accessMode != O_RDWR) {
		return B_BAD_VALUE;
	}

	long maxMessages = kDefaultMaxMessages;
	long messageSize = kDefaultMessageSize;
	if ((openFlags & O_CREAT) != 0 && userAttr != NULL) {
		mq_attr attr;
		if (!IS_USER_ADDRESS(userAttr)
			|| user_memcpy(&attr, userAttr, sizeof(attr)) != B_OK) {
			return B_BAD_ADDRESS;
		}
		if (attr.mq_maxmsg <= 0 || attr.mq_msgsize <= 0
			|| attr.mq_maxmsg > kMaxMessages
			|| attr.mq_msgsize > kMaxMessageSize
			|| (size_t)attr.mq_maxmsg * (size_t)attr.mq_msgsize
				> kMaxQueueBytes) {
			return B_BAD_VALUE;
		}
		maxMessages = attr.mq_maxmsg;
		messageSize = attr.mq_msgsize;
	}

	// Find or create the queue.
	MessageQueue* queue;
	{
		MutexLocker tableLocker(sTableLock);

		queue = sQueues->Lookup(name);
		if (queue != NULL) {
			if ((openFlags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
				return EEXIST;
			if (!queue->HasAccess(accessMode))
				return EACCES;
			queue->AcquireReference();
		} else {
			if ((openFlags & O_CREAT) == 0)
				return ENOENT;
			if (sQueueCount >= kMaxQueues)
				return ENOSPC;

			// libroot has already applied the umask to "mode".
			queue = new(std::nothrow) MessageQueue(maxMessages, messageSize,
				mode);
			if (queue == NULL || queue->SetName(name) != B_OK) {
				delete queue;
				return B_NO_MEMORY;
			}
			// The creator gets its descriptor regardless of "mode", as with
			// open(O_CREAT); HasAccess() applies to later opens only.
			queue->AcquireReference();
				// one reference for the table, one for the descriptor
			sQueues->Insert(queue);
			sQueueCount++;
			TRACE("created %s (%ld x %ld bytes)\n", name, maxMessages,
				messageSize);
		}
	}

	file_descriptor* descriptor = alloc_fd();
	if (descriptor == NULL) {
		queue->ReleaseReference();
		return B_NO_MEMORY;
	}

	descriptor->ops = &sMessageQueueOps;
	descriptor->cookie = queue;
	descriptor->open_mode = accessMode | (openFlags & O_NONBLOCK);

	io_context* context = get_current_io_context(false);
	rw_lock_write_lock(&context->lock);
	int fd = new_fd(context, descriptor);
	if (fd < 0) {
		rw_lock_write_unlock(&context->lock);
		descriptor->ops = NULL;
		descriptor->cookie = NULL;
		put_fd(descriptor);
		queue->ReleaseReference();
		return fd;
	}

	// POSIX: message queue descriptors are closed on exec().
	fd_set_close_on_exec(context, fd, true);
	fd_set_close_on_fork(context, fd, false);
	rw_lock_write_unlock(&context->lock);

	return fd;
}


status_t
_user_mqueue_unlink(const char* userName)
{
	char name[NAME_MAX + 1];
	status_t status = copy_queue_name(userName, name, sizeof(name));
	if (status != B_OK)
		return status;

	MutexLocker tableLocker(sTableLock);

	MessageQueue* queue = sQueues->Lookup(name);
	if (queue == NULL)
		return ENOENT;
	if (!queue->MayUnlink())
		return EACCES;

	sQueues->Remove(queue);
	sQueueCount--;
	tableLocker.Unlock();

	queue->ReleaseReference();
		// the table's reference; open descriptors keep the queue alive
	return B_OK;
}


status_t
_user_mqueue_send(int fd, const void* userBuffer, size_t length,
	uint32 priority, uint32 flags, bigtime_t timeout)
{
	if (priority >= MQ_PRIO_MAX)
		return B_BAD_VALUE;
	if (length > 0 && (userBuffer == NULL || !IS_USER_ADDRESS(userBuffer)))
		return B_BAD_ADDRESS;

	flags &= B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT | B_TIMEOUT_REAL_TIME_BASE;
	syscall_restart_handle_timeout_pre(flags, timeout);

	file_descriptor* descriptor;
	MessageQueue* queue;
	status_t status = get_mqueue(fd, O_WRONLY, descriptor, queue);
	if (status != B_OK)
		return status;
	DescriptorPutter putter(descriptor);

	status = queue->Send(userBuffer, length, priority,
		(descriptor->open_mode & O_NONBLOCK) != 0, flags, timeout);

	return syscall_restart_handle_timeout_post(status, timeout);
}


ssize_t
_user_mqueue_receive(int fd, void* userBuffer, size_t length,
	uint32* userPriority, uint32 flags, bigtime_t timeout)
{
	if (userBuffer == NULL || !IS_USER_ADDRESS(userBuffer)
		|| (userPriority != NULL && !IS_USER_ADDRESS(userPriority))) {
		return B_BAD_ADDRESS;
	}

	flags &= B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT | B_TIMEOUT_REAL_TIME_BASE;
	syscall_restart_handle_timeout_pre(flags, timeout);

	file_descriptor* descriptor;
	MessageQueue* queue;
	status_t status = get_mqueue(fd, O_RDONLY, descriptor, queue);
	if (status != B_OK)
		return status;
	DescriptorPutter putter(descriptor);

	ssize_t result = queue->Receive(userBuffer, length, userPriority,
		(descriptor->open_mode & O_NONBLOCK) != 0, flags, timeout);

	if (result < 0)
		return syscall_restart_handle_timeout_post(result, timeout);
	return result;
}


status_t
_user_mqueue_getsetattr(int fd, const struct mq_attr* userNewAttr,
	struct mq_attr* userOldAttr)
{
	mq_attr newAttr;
	if (userNewAttr != NULL
		&& (!IS_USER_ADDRESS(userNewAttr)
			|| user_memcpy(&newAttr, userNewAttr, sizeof(newAttr)) != B_OK)) {
		return B_BAD_ADDRESS;
	}
	if (userOldAttr != NULL && !IS_USER_ADDRESS(userOldAttr))
		return B_BAD_ADDRESS;

	file_descriptor* descriptor;
	MessageQueue* queue;
	status_t status = get_mqueue(fd, 0, descriptor, queue);
	if (status != B_OK)
		return status;
	DescriptorPutter putter(descriptor);

	mq_attr oldAttr;
	queue->GetAttributes(oldAttr);
	oldAttr.mq_flags = descriptor->open_mode & O_NONBLOCK;

	if (userNewAttr != NULL) {
		// Only O_NONBLOCK can change; the other fields are ignored (POSIX).
		descriptor->open_mode = (descriptor->open_mode & ~O_NONBLOCK)
			| (newAttr.mq_flags & O_NONBLOCK);
	}

	if (userOldAttr != NULL
		&& user_memcpy(userOldAttr, &oldAttr, sizeof(oldAttr)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


status_t
_user_mqueue_notify(int fd, const struct sigevent* userEvent,
	const thread_creation_attributes* userThreadAttributes)
{
	struct sigevent event;
	if (userEvent != NULL
		&& (!IS_USER_ADDRESS(userEvent)
			|| user_memcpy(&event, userEvent, sizeof(event)) != B_OK)) {
		return B_BAD_ADDRESS;
	}

	char nameBuffer[B_OS_NAME_LENGTH];
	ThreadCreationAttributes threadAttributes;
	if (userEvent != NULL && event.sigev_notify == SIGEV_THREAD) {
		status_t status = threadAttributes.InitFromUserAttributes(
			userThreadAttributes, nameBuffer);
		if (status != B_OK)
			return status;
	}

	file_descriptor* descriptor;
	MessageQueue* queue;
	status_t status = get_mqueue(fd, 0, descriptor, queue);
	if (status != B_OK)
		return status;
	DescriptorPutter putter(descriptor);

	if (userEvent == NULL) {
		queue->RemoveNotification(descriptor, false);
		return B_OK;
	}

	return queue->SetNotification(descriptor, event,
		event.sigev_notify == SIGEV_THREAD ? &threadAttributes : NULL);
}
