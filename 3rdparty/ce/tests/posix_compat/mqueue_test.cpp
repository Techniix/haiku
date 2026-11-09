/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest conformance test for POSIX message queues (<mqueue.h>,
	libroot + kernel). Checks POSIX.1-2017 behaviour plus the Linux/FreeBSD
	extension that a queue descriptor works with poll()/select().
	Output format: see 3rdparty/ce/README.md.
*/


#include <mqueue.h>

#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


#define SUITE "mqueue"


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


static const char*
queue_name(const char* tag)
{
	static char names[8][64];
	static int next = 0;
	char* name = names[next++ % 8];
	snprintf(name, 64, "/ce-mq-%d-%s", (int)getpid(), tag);
	return name;
}


static mqd_t
create_queue(const char* name, long maxMessages, long messageSize,
	int extraFlags = 0)
{
	struct mq_attr attr = {};
	attr.mq_maxmsg = maxMessages;
	attr.mq_msgsize = messageSize;
	return mq_open(name, O_CREAT | O_EXCL | O_RDWR | extraFlags, 0600, &attr);
}


static struct timespec
realtime_in(bigtime_t micros)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	bigtime_t total = (bigtime_t)now.tv_nsec / 1000 + micros;
	now.tv_sec += total / 1000000;
	now.tv_nsec = (total % 1000000) * 1000;
	return now;
}


static void
test_open()
{
	const char* name = queue_name("open");
	mqd_t queue = create_queue(name, 4, 64);
	report("create", queue >= 0, "mqd %d (%s)", queue,
		queue < 0 ? strerror(errno) : "ok");
	if (queue < 0)
		return;

	struct mq_attr attr = {};
	int result = mq_getattr(queue, &attr);
	report("getattr", result == 0 && attr.mq_maxmsg == 4
			&& attr.mq_msgsize == 64 && attr.mq_curmsgs == 0
			&& attr.mq_flags == 0,
		"maxmsg %ld msgsize %ld curmsgs %ld flags 0x%lx", attr.mq_maxmsg,
		attr.mq_msgsize, attr.mq_curmsgs, attr.mq_flags);

	errno = 0;
	mqd_t again = create_queue(name, 4, 64);
	report("excl_eexist", again < 0 && errno == EEXIST, "result %d errno %s",
		again, strerror(errno));

	mqd_t second = mq_open(name, O_RDONLY);
	report("open_existing", second >= 0, "mqd %d", second);
	if (second >= 0)
		mq_close(second);

	report("cloexec", (fcntl(queue, F_GETFD) & FD_CLOEXEC) != 0,
		"F_GETFD 0x%x", fcntl(queue, F_GETFD));

	struct stat st;
	result = fstat(queue, &st);
	report("fstat", result == 0 && S_ISREG(st.st_mode)
			&& (st.st_mode & 0777) == 0600 && st.st_uid == geteuid(),
		"mode 0%o uid %d", (unsigned)st.st_mode, (int)st.st_uid);

	mq_close(queue);
	mq_unlink(name);

	errno = 0;
	queue = mq_open(queue_name("missing"), O_RDWR);
	report("open_missing_enoent", queue < 0 && errno == ENOENT, "errno %s",
		strerror(errno));

	// Default attributes: Linux's 10 x 8192.
	name = queue_name("defaults");
	queue = mq_open(name, O_CREAT | O_RDWR, 0600, NULL);
	memset(&attr, 0, sizeof(attr));
	mq_getattr(queue, &attr);
	report("default_attr", queue >= 0 && attr.mq_maxmsg == 10
			&& attr.mq_msgsize == 8192,
		"maxmsg %ld msgsize %ld", attr.mq_maxmsg, attr.mq_msgsize);
	mq_close(queue);
	mq_unlink(name);
}


static void
test_bad_arguments()
{
	struct {
		const char*	name;
		int			error;
	} names[] = {
		{ "noslash", EINVAL },
		{ "/a/b", EINVAL },
		{ "/", EINVAL },
	};
	bool namesOk = true;
	char detail[200] = "";
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		errno = 0;
		mqd_t queue = mq_open(names[i].name, O_CREAT | O_RDWR, 0600, NULL);
		if (queue >= 0 || errno != names[i].error) {
			namesOk = false;
			snprintf(detail + strlen(detail), sizeof(detail) - strlen(detail),
				"%s: %d/%s ", names[i].name, queue, strerror(errno));
			if (queue >= 0) {
				mq_close(queue);
				mq_unlink(names[i].name);
			}
		}
	}
	char longName[400];
	longName[0] = '/';
	memset(longName + 1, 'x', sizeof(longName) - 2);
	longName[sizeof(longName) - 1] = '\0';
	errno = 0;
	mqd_t queue = mq_open(longName, O_CREAT | O_RDWR, 0600, NULL);
	bool longOk = queue < 0 && errno == ENAMETOOLONG;
	report("bad_names", namesOk && longOk, "%slong: %s",
		detail[0] != '\0' ? detail : "slash rules ok, ", strerror(errno));

	errno = 0;
	queue = create_queue(queue_name("zero"), 0, 64);
	int zeroErrno = errno;
	errno = 0;
	mqd_t huge = create_queue(queue_name("huge"), 4, 2 * 1024 * 1024);
	report("bad_attr_einval", queue < 0 && zeroErrno == EINVAL && huge < 0
			&& errno == EINVAL,
		"maxmsg 0: %s, msgsize 2 MiB: %s", strerror(zeroErrno),
		strerror(errno));

	struct mq_attr attr;
	errno = 0;
	int result = mq_getattr(9999, &attr);
	int badErrno = errno;
	int pipeFDs[2];
	pipe(pipeFDs);
	errno = 0;
	int pipeResult = mq_close(pipeFDs[0]);
	int pipeErrno = errno;
	bool pipeStillOpen = fcntl(pipeFDs[0], F_GETFD) >= 0;
	close(pipeFDs[0]);
	close(pipeFDs[1]);
	report("not_a_queue_ebadf", result < 0 && badErrno == EBADF
			&& pipeResult < 0 && pipeErrno == EBADF && pipeStillOpen,
		"bad fd: %s, pipe: %s, pipe left open %d", strerror(badErrno),
		strerror(pipeErrno), pipeStillOpen);
}


static void
test_messages()
{
	const char* name = queue_name("msgs");
	mqd_t queue = create_queue(name, 8, 64);

	// Highest priority first, FIFO within a priority.
	struct { const char* text; unsigned priority; } sent[] = {
		{ "a1", 1 }, { "b5", 5 }, { "c5", 5 }, { "d0", 0 }, { "e1", 1 },
		{ "f9", 9 },
	};
	for (size_t i = 0; i < sizeof(sent) / sizeof(sent[0]); i++)
		mq_send(queue, sent[i].text, strlen(sent[i].text) + 1, sent[i].priority);

	const char* expected[] = { "f9", "b5", "c5", "a1", "e1", "d0" };
	unsigned expectedPriority[] = { 9, 5, 5, 1, 1, 0 };
	char order[64] = "";
	bool ordered = true;
	for (int i = 0; i < 6; i++) {
		char buffer[64];
		unsigned priority = 999;
		ssize_t length = mq_receive(queue, buffer, sizeof(buffer), &priority);
		if (length != 3 || strcmp(buffer, expected[i]) != 0
			|| priority != expectedPriority[i]) {
			ordered = false;
		}
		strlcat(order, length > 0 ? buffer : "?", sizeof(order));
		strlcat(order, " ", sizeof(order));
	}
	report("priority_order", ordered, "received %s", order);

	// Zero-length messages are valid.
	int sendResult = mq_send(queue, "", 0, 3);
	char buffer[64];
	unsigned priority = 0;
	ssize_t length = mq_receive(queue, buffer, sizeof(buffer), &priority);
	report("zero_length", sendResult == 0 && length == 0 && priority == 3,
		"send %d receive %zd prio %u", sendResult, length, priority);

	char big[65];
	memset(big, 'x', sizeof(big));
	errno = 0;
	int result = mq_send(queue, big, sizeof(big), 0);
	int sendErrno = errno;
	mq_send(queue, big, 64, 0);
	errno = 0;
	length = mq_receive(queue, buffer, 63, NULL);
	int receiveErrno = errno;
	struct mq_attr attr;
	mq_getattr(queue, &attr);
	report("emsgsize", result < 0 && sendErrno == EMSGSIZE && length < 0
			&& receiveErrno == EMSGSIZE && attr.mq_curmsgs == 1,
		"send 65: %s, receive into 63: %s, still queued %ld",
		strerror(sendErrno), strerror(receiveErrno), attr.mq_curmsgs);
	mq_receive(queue, buffer, sizeof(buffer), NULL);

	errno = 0;
	result = mq_send(queue, "x", 1, MQ_PRIO_MAX);
	report("prio_max_einval", result < 0 && errno == EINVAL
			&& MQ_PRIO_MAX >= 32 && sysconf(_SC_MQ_PRIO_MAX) == MQ_PRIO_MAX,
		"MQ_PRIO_MAX %d sysconf %ld errno %s", MQ_PRIO_MAX,
		sysconf(_SC_MQ_PRIO_MAX), strerror(errno));

	report("posix_option", sysconf(_SC_MESSAGE_PASSING) > 0,
		"_SC_MESSAGE_PASSING %ld", sysconf(_SC_MESSAGE_PASSING));

	mq_close(queue);
	mq_unlink(name);
}


static short
poll_events(mqd_t queue)
{
	struct pollfd pfd = { queue, POLLIN | POLLOUT, 0 };
	if (poll(&pfd, 1, 0) < 0)
		return -1;
	return pfd.revents;
}


static void
test_nonblocking_and_poll()
{
	const char* name = queue_name("nb");
	mqd_t queue = create_queue(name, 3, 16, O_NONBLOCK);

	struct mq_attr attr;
	mq_getattr(queue, &attr);
	bool flagSet = attr.mq_flags == O_NONBLOCK;

	short emptyEvents = poll_events(queue);
	int sent = 0;
	for (int i = 0; i < 5; i++) {
		if (mq_send(queue, "m", 1, 0) == 0)
			sent++;
	}
	int fullErrno = errno;
	short fullEvents = poll_events(queue);
	report("full_eagain", sent == 3 && fullErrno == EAGAIN && flagSet,
		"sent %d of 5, errno %s, mq_flags 0x%lx", sent, strerror(fullErrno),
		attr.mq_flags);

	char buffer[16];
	mq_receive(queue, buffer, sizeof(buffer), NULL);
	short partialEvents = poll_events(queue);
	while (mq_receive(queue, buffer, sizeof(buffer), NULL) >= 0)
		;
	int emptyErrno = errno;
	report("empty_eagain", emptyErrno == EAGAIN, "errno %s",
		strerror(emptyErrno));

	report("poll", emptyEvents == POLLOUT
			&& fullEvents == POLLIN
			&& partialEvents == (POLLIN | POLLOUT),
		"empty 0x%x full 0x%x partial 0x%x", emptyEvents, fullEvents,
		partialEvents);

	// mq_setattr changes only O_NONBLOCK and reports the old attributes.
	struct mq_attr newAttr = {};
	newAttr.mq_flags = 0;
	newAttr.mq_maxmsg = 99;
	struct mq_attr oldAttr = {};
	int result = mq_setattr(queue, &newAttr, &oldAttr);
	mq_getattr(queue, &attr);
	report("setattr", result == 0 && oldAttr.mq_flags == O_NONBLOCK
			&& attr.mq_flags == 0 && attr.mq_maxmsg == 3
			&& (fcntl(queue, F_GETFL) & O_NONBLOCK) == 0,
		"old flags 0x%lx new flags 0x%lx maxmsg %ld", oldAttr.mq_flags,
		attr.mq_flags, attr.mq_maxmsg);

	mq_close(queue);
	mq_unlink(name);
}


static void
test_timeouts()
{
	const char* name = queue_name("timeout");
	mqd_t queue = create_queue(name, 1, 16);
	char buffer[16];

	struct timespec deadline = realtime_in(100000);
	bigtime_t start = system_time();
	errno = 0;
	ssize_t length = mq_timedreceive(queue, buffer, sizeof(buffer), NULL,
		&deadline);
	bigtime_t elapsed = system_time() - start;
	report("timedreceive_etimedout", length < 0 && errno == ETIMEDOUT
			&& elapsed >= 90000 && elapsed < 5000000,
		"errno %s after %lld us", strerror(errno), (long long)elapsed);

	mq_send(queue, "x", 1, 0);
	deadline = realtime_in(100000);
	start = system_time();
	errno = 0;
	int result = mq_timedsend(queue, "y", 1, 0, &deadline);
	elapsed = system_time() - start;
	report("timedsend_etimedout", result < 0 && errno == ETIMEDOUT
			&& elapsed >= 90000 && elapsed < 5000000,
		"errno %s after %lld us", strerror(errno), (long long)elapsed);

	// An invalid timeout only matters when the call would block (POSIX).
	struct timespec invalid = { 0, 2000000000 };
	length = mq_timedreceive(queue, buffer, sizeof(buffer), NULL, &invalid);
	errno = 0;
	ssize_t blocked = mq_timedreceive(queue, buffer, sizeof(buffer), NULL,
		&invalid);
	report("invalid_timeout", length == 1 && blocked < 0 && errno == EINVAL,
		"non-blocking %zd, blocking errno %s", length, strerror(errno));

	// A deadline in the past still delivers a queued message.
	mq_send(queue, "z", 1, 0);
	struct timespec past = { 1, 0 };
	length = mq_timedreceive(queue, buffer, sizeof(buffer), NULL, &past);
	report("past_deadline_delivers", length == 1, "result %zd", length);

	mq_close(queue);
	mq_unlink(name);
}


struct PeerArgs {
	mqd_t		queue;
	bigtime_t	delay;
	int			count;
	int			id;
};


static status_t
sender_thread(void* data)
{
	PeerArgs* args = (PeerArgs*)data;
	snooze(args->delay);
	for (int i = 0; i < args->count; i++) {
		char message[16];
		snprintf(message, sizeof(message), "%d:%d", args->id, i);
		if (mq_send(args->queue, message, strlen(message) + 1, 0) != 0)
			return errno;
	}
	return B_OK;
}


static status_t
receiver_thread(void* data)
{
	PeerArgs* args = (PeerArgs*)data;
	snooze(args->delay);
	char buffer[64];
	for (int i = 0; i < args->count; i++) {
		if (mq_receive(args->queue, buffer, sizeof(buffer), NULL) < 0)
			return errno;
	}
	return B_OK;
}


static void
test_blocking()
{
	const char* name = queue_name("block");
	mqd_t queue = create_queue(name, 2, 64);

	PeerArgs args = { queue, 50000, 1, 0 };
	thread_id thread = spawn_thread(sender_thread, "mq sender",
		B_NORMAL_PRIORITY, &args);
	resume_thread(thread);
	char buffer[64];
	bigtime_t start = system_time();
	ssize_t length = mq_receive(queue, buffer, sizeof(buffer), NULL);
	bigtime_t elapsed = system_time() - start;
	status_t threadResult;
	wait_for_thread(thread, &threadResult);
	report("blocking_receive", length == 4 && strcmp(buffer, "0:0") == 0
			&& elapsed >= 40000,
		"got %zd '%s' after %lld us", length, length > 0 ? buffer : "",
		(long long)elapsed);

	mq_send(queue, "a", 2, 0);
	mq_send(queue, "b", 2, 0);
	args.count = 1;
	thread = spawn_thread(receiver_thread, "mq receiver", B_NORMAL_PRIORITY,
		&args);
	resume_thread(thread);
	start = system_time();
	int result = mq_send(queue, "c", 2, 0);
	elapsed = system_time() - start;
	wait_for_thread(thread, &threadResult);
	report("blocking_send", result == 0 && elapsed >= 40000,
		"result %d after %lld us", result, (long long)elapsed);

	mq_close(queue);
	mq_unlink(name);
}


static void
test_access()
{
	const char* name = queue_name("access");
	mqd_t queue = create_queue(name, 2, 16);
	mqd_t reader = mq_open(name, O_RDONLY);
	mqd_t writer = mq_open(name, O_WRONLY);

	errno = 0;
	int sendResult = mq_send(reader, "x", 1, 0);
	int sendErrno = errno;
	char buffer[16];
	errno = 0;
	ssize_t receiveResult = mq_receive(writer, buffer, sizeof(buffer), NULL);
	int receiveErrno = errno;
	bool crossOk = mq_send(writer, "y", 1, 0) == 0
		&& mq_receive(reader, buffer, sizeof(buffer), NULL) == 1;
	report("access_mode_ebadf", sendResult < 0 && sendErrno == EBADF
			&& receiveResult < 0 && receiveErrno == EBADF && crossOk,
		"send on O_RDONLY: %s, receive on O_WRONLY: %s, proper use %d",
		strerror(sendErrno), strerror(receiveErrno), crossOk);

	// Mode 0600 created by us: we may reopen; mode 0 denies a non-root
	// opener, but these tests run as root on Haiku, so only check that
	// root is allowed.
	mq_close(reader);
	mq_close(writer);
	mq_close(queue);
	mq_unlink(name);
}


static void
test_processes()
{
	const char* name = queue_name("proc");
	mqd_t queue = create_queue(name, 4, 64);

	// Inherited descriptor.
	pid_t child = fork();
	if (child == 0) {
		_exit(mq_send(queue, "from child", 11, 7) == 0 ? 0 : 1);
	}
	int status = -1;
	waitpid(child, &status, 0);
	char buffer[64] = "";
	unsigned priority = 0;
	ssize_t length = mq_receive(queue, buffer, sizeof(buffer), &priority);
	report("fork_inherited", WIFEXITED(status) && WEXITSTATUS(status) == 0
			&& length == 11 && strcmp(buffer, "from child") == 0
			&& priority == 7,
		"child status 0x%x got '%s' prio %u", status, buffer, priority);

	// Opened by name in another process.
	child = fork();
	if (child == 0) {
		mqd_t own = mq_open(name, O_WRONLY);
		if (own < 0)
			_exit(2);
		_exit(mq_send(own, "by name", 8, 1) == 0 ? 0 : 3);
	}
	waitpid(child, &status, 0);
	length = mq_receive(queue, buffer, sizeof(buffer), &priority);
	report("open_by_name_other_process", WIFEXITED(status)
			&& WEXITSTATUS(status) == 0 && length == 8
			&& strcmp(buffer, "by name") == 0,
		"child status 0x%x got '%s'", status, length > 0 ? buffer : "");

	// Unlink: the name goes away, open descriptors keep working, and the
	// name can be reused for a new, empty queue.
	mq_send(queue, "kept", 5, 0);
	int unlinkResult = mq_unlink(name);
	errno = 0;
	mqd_t gone = mq_open(name, O_RDWR);
	int goneErrno = errno;
	length = mq_receive(queue, buffer, sizeof(buffer), NULL);
	mqd_t fresh = create_queue(name, 4, 64);
	struct mq_attr attr = {};
	mq_getattr(fresh, &attr);
	errno = 0;
	int unlinkAgain = mq_unlink(queue_name("never-existed"));
	report("unlink", unlinkResult == 0 && gone < 0 && goneErrno == ENOENT
			&& length == 5 && fresh >= 0 && attr.mq_curmsgs == 0
			&& unlinkAgain < 0 && errno == ENOENT,
		"unlink %d, reopen %s, old fd read %zd, fresh curmsgs %ld",
		unlinkResult, strerror(goneErrno), length, attr.mq_curmsgs);

	mq_close(fresh);
	mq_unlink(name);
	mq_close(queue);
}


static volatile int32 sSignalCount = 0;
static volatile int sSignalCode = 0;
static volatile int sSignalValue = 0;


static void
notify_handler(int signal, siginfo_t* info, void*)
{
	sSignalCode = info->si_code;
	sSignalValue = info->si_value.sival_int;
	atomic_add((int32*)&sSignalCount, 1);
}


static volatile int32 sThreadNotifications = 0;
static volatile int sThreadValue = 0;


static void
notify_function(union sigval value)
{
	sThreadValue = value.sival_int;
	atomic_add((int32*)&sThreadNotifications, 1);
}


static bool
wait_for(volatile int32* counter, int32 value, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (*counter < value && system_time() < end)
		snooze(5000);
	return *counter >= value;
}


static void
test_notify()
{
	const char* name = queue_name("notify");
	mqd_t queue = create_queue(name, 4, 16);
	char buffer[16];

	struct sigaction action = {};
	action.sa_sigaction = notify_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &action, NULL);

	struct sigevent event = {};
	event.sigev_notify = SIGEV_SIGNAL;
	event.sigev_signo = SIGUSR1;
	event.sigev_value.sival_int = 4242;
	int result = mq_notify(queue, &event);
	mq_send(queue, "1", 1, 0);
	bool fired = wait_for(&sSignalCount, 1, 2000000);
	// One-shot: a second message to the (now non-empty) queue, and one to
	// an empty queue after draining, must not signal again.
	mq_send(queue, "2", 1, 0);
	mq_receive(queue, buffer, sizeof(buffer), NULL);
	mq_receive(queue, buffer, sizeof(buffer), NULL);
	mq_send(queue, "3", 1, 0);
	snooze(100000);
	report("notify_signal", result == 0 && fired && sSignalCount == 1
			&& sSignalCode == SI_MESGQ && sSignalValue == 4242,
		"register %d, signals %d, si_code %d, value %d", result,
		(int)sSignalCount, sSignalCode, sSignalValue);
	mq_receive(queue, buffer, sizeof(buffer), NULL);

	// Registration slot: one process at a time.
	result = mq_notify(queue, &event);
	pid_t child = fork();
	if (child == 0) {
		struct sigevent other = {};
		other.sigev_notify = SIGEV_NONE;
		int childResult = mq_notify(queue, &other);
		_exit(childResult < 0 && errno == EBUSY ? 0 : 1);
	}
	int status = -1;
	waitpid(child, &status, 0);
	// mq_notify(NULL) removes ours; then nothing fires.
	int removeResult = mq_notify(queue, NULL);
	int32 before = sSignalCount;
	mq_send(queue, "4", 1, 0);
	snooze(100000);
	report("notify_busy_and_remove", result == 0 && WIFEXITED(status)
			&& WEXITSTATUS(status) == 0 && removeResult == 0
			&& sSignalCount == before,
		"child status 0x%x, remove %d, signals after removal %d", status,
		removeResult, (int)(sSignalCount - before));
	mq_receive(queue, buffer, sizeof(buffer), NULL);

	// No notification when a receiver is already waiting (POSIX); the
	// registration stays in place.
	result = mq_notify(queue, &event);
	PeerArgs args = { queue, 0, 1, 0 };
	thread_id receiver = spawn_thread(receiver_thread, "mq waiting receiver",
		B_NORMAL_PRIORITY, &args);
	resume_thread(receiver);
	snooze(100000);
	before = sSignalCount;
	mq_send(queue, "5", 1, 0);
	status_t receiverResult;
	wait_for_thread(receiver, &receiverResult);
	snooze(100000);
	struct sigevent again = event;
	errno = 0;
	int stillRegistered = mq_notify(queue, &again);
	int stillErrno = errno;
	report("notify_skipped_with_waiter", result == 0 && receiverResult == B_OK
			&& sSignalCount == before && stillRegistered < 0
			&& stillErrno == EBUSY,
		"receiver %s, signals %d, re-register: %s",
		strerror(receiverResult), (int)(sSignalCount - before),
		strerror(stillErrno));
	mq_notify(queue, NULL);

	// SIGEV_THREAD: the function runs in a new thread with the value.
	event.sigev_notify = SIGEV_THREAD;
	event.sigev_notify_function = notify_function;
	event.sigev_notify_attributes = NULL;
	event.sigev_value.sival_int = 777;
	result = mq_notify(queue, &event);
	mq_send(queue, "6", 1, 0);
	fired = wait_for(&sThreadNotifications, 1, 2000000);
	report("notify_thread", result == 0 && fired && sThreadValue == 777,
		"register %d, calls %d, value %d", result,
		(int)sThreadNotifications, sThreadValue);
	mq_receive(queue, buffer, sizeof(buffer), NULL);

	// Closing the registering descriptor removes the registration.
	event.sigev_notify = SIGEV_NONE;
	mqd_t extra = mq_open(name, O_RDWR);
	result = mq_notify(extra, &event);
	mq_close(extra);
	errno = 0;
	int afterClose = mq_notify(queue, &event);
	report("notify_removed_on_close", result == 0 && afterClose == 0,
		"register %d, after close %d (%s)", result, afterClose,
		afterClose < 0 ? strerror(errno) : "ok");
	mq_notify(queue, NULL);

	signal(SIGUSR1, SIG_DFL);
	mq_close(queue);
	mq_unlink(name);
}


static volatile int32 sInterrupts = 0;


static void
interrupt_handler(int)
{
	atomic_add((int32*)&sInterrupts, 1);
}


struct SignalArgs {
	thread_id	target;
	bigtime_t	delay;
	mqd_t		queue;
	bool		sendAfter;
};


static status_t
signal_thread(void* data)
{
	SignalArgs* args = (SignalArgs*)data;
	snooze(args->delay);
	send_signal(args->target, SIGUSR2);
	if (args->sendAfter) {
		snooze(args->delay);
		mq_send(args->queue, "late", 5, 0);
	}
	return B_OK;
}


static void
test_interrupts()
{
	const char* name = queue_name("intr");
	mqd_t queue = create_queue(name, 2, 16);
	char buffer[16];

	// Without SA_RESTART: EINTR.
	struct sigaction action = {};
	action.sa_handler = interrupt_handler;
	sigaction(SIGUSR2, &action, NULL);
	SignalArgs args = { find_thread(NULL), 50000, queue, false };
	thread_id thread = spawn_thread(signal_thread, "mq signaller",
		B_NORMAL_PRIORITY, &args);
	resume_thread(thread);
	errno = 0;
	ssize_t length = mq_receive(queue, buffer, sizeof(buffer), NULL);
	int interruptErrno = errno;
	status_t threadResult;
	wait_for_thread(thread, &threadResult);
	report("eintr", length < 0 && interruptErrno == EINTR && sInterrupts == 1,
		"result %zd errno %s handler calls %d", length,
		strerror(interruptErrno), (int)sInterrupts);

	// With SA_RESTART: the receive continues and gets the later message.
	action.sa_flags = SA_RESTART;
	sigaction(SIGUSR2, &action, NULL);
	args.sendAfter = true;
	thread = spawn_thread(signal_thread, "mq signaller", B_NORMAL_PRIORITY,
		&args);
	resume_thread(thread);
	length = mq_receive(queue, buffer, sizeof(buffer), NULL);
	wait_for_thread(thread, &threadResult);
	report("sa_restart", length == 5 && strcmp(buffer, "late") == 0
			&& sInterrupts == 2,
		"result %zd '%s' handler calls %d", length, length > 0 ? buffer : "",
		(int)sInterrupts);

	signal(SIGUSR2, SIG_DFL);
	mq_close(queue);
	mq_unlink(name);
}


static void
test_stress()
{
	// Two producers, one consumer, a small queue: lots of blocking on both
	// sides. Every message must arrive once, and each producer's messages
	// in order (same priority = FIFO).
	const char* name = queue_name("stress");
	mqd_t queue = create_queue(name, 4, 32);
	const int kCount = 2000;
	PeerArgs producers[2] = { { queue, 0, kCount, 0 }, { queue, 0, kCount, 1 } };
	thread_id threads[2];
	for (int i = 0; i < 2; i++) {
		threads[i] = spawn_thread(sender_thread, "mq producer",
			B_NORMAL_PRIORITY, &producers[i]);
		resume_thread(threads[i]);
	}

	int next[2] = { 0, 0 };
	bool inOrder = true;
	int received = 0;
	bigtime_t start = system_time();
	while (received < 2 * kCount && system_time() - start < 30000000) {
		char buffer[32];
		struct timespec deadline = realtime_in(5000000);
		if (mq_timedreceive(queue, buffer, sizeof(buffer), NULL, &deadline)
				< 0) {
			break;
		}
		int producer, index;
		if (sscanf(buffer, "%d:%d", &producer, &index) != 2
			|| producer < 0 || producer > 1 || index != next[producer]) {
			inOrder = false;
		} else
			next[producer]++;
		received++;
	}
	for (int i = 0; i < 2; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
	}
	report("stress_two_producers", received == 2 * kCount && inOrder,
		"%d of %d in %lld us, in order %d", received, 2 * kCount,
		(long long)(system_time() - start), inOrder);

	mq_close(queue);
	mq_unlink(name);

	// Creating and removing many queues must not exhaust the table.
	int created = 0;
	for (int i = 0; i < 1500; i++) {
		char churnName[64];
		snprintf(churnName, sizeof(churnName), "/ce-mq-%d-churn-%d",
			(int)getpid(), i);
		mqd_t churn = create_queue(churnName, 2, 16);
		if (churn < 0)
			break;
		mq_send(churn, "leftover", 9, 0);
		mq_close(churn);
		mq_unlink(churnName);
		created++;
	}
	report("churn_1500_queues", created == 1500, "created %d", created);
}


int
main()
{
	test_open();
	test_bad_arguments();
	test_messages();
	test_nonblocking_and_poll();
	test_timeouts();
	test_blocking();
	test_access();
	test_processes();
	test_notify();
	test_interrupts();
	test_stress();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
