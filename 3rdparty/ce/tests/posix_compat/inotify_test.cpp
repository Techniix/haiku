/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest conformance test for inotify (kernel object + libgnu API).

	The expected results are Linux's (inotify(7)), which file watchers in
	Chromium/Electron (base::FilePathWatcher), Node.js (fs.watch through
	libuv), glib/GIO, Qt's QFileSystemWatcher and build tools depend on.
	Output format: see 3rdparty/ce/README.md.
*/


#include <sys/epoll.h>
#include <sys/inotify.h>

#include <OS.h>
#include <fs_attr.h>
#include <TypeConstants.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>


#define SUITE "inotify"


static int sFailures = 0;
static char sBase[256];


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[384];
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


struct Event {
	int			wd;
	uint32_t	mask;
	uint32_t	cookie;
	std::string	name;
};


//! Reads everything that arrives within \a timeout ms of the last event.
static std::vector<Event>
collect(int fd, int timeout = 300)
{
	std::vector<Event> events;
	char buffer[65536] __attribute__((aligned(8)));
	while (true) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		if (poll(&pfd, 1, timeout) <= 0)
			break;
		ssize_t bytes = read(fd, buffer, sizeof(buffer));
		if (bytes <= 0)
			break;
		for (ssize_t offset = 0; offset < bytes;) {
			struct inotify_event* event
				= (struct inotify_event*)(buffer + offset);
			Event copy;
			copy.wd = event->wd;
			copy.mask = event->mask;
			copy.cookie = event->cookie;
			if (event->len > 0)
				copy.name = event->name;
			events.push_back(copy);
			offset += sizeof(struct inotify_event) + event->len;
		}
	}
	return events;
}


static std::string
describe(const std::vector<Event>& events)
{
	std::string text;
	char item[96];
	for (size_t i = 0; i < events.size() && i < 8; i++) {
		snprintf(item, sizeof(item), "%s[%d %#x %u '%s']", i > 0 ? " " : "",
			events[i].wd, events[i].mask, events[i].cookie,
			events[i].name.c_str());
		text += item;
	}
	if (events.size() > 8) {
		snprintf(item, sizeof(item), " ...(%zu)", events.size());
		text += item;
	}
	return text.empty() ? "none" : text;
}


//! Index of the first event with this mask and name after \a from, or -1.
static int
find(const std::vector<Event>& events, uint32_t mask, const char* name,
	int wd = -2, size_t from = 0)
{
	for (size_t i = from; i < events.size(); i++) {
		if (events[i].mask == mask && events[i].name == name
			&& (wd == -2 || events[i].wd == wd)) {
			return (int)i;
		}
	}
	return -1;
}


static std::string
path(const char* leaf)
{
	return std::string(sBase) + "/" + leaf;
}


static void
write_file(const char* leaf, const char* text)
{
	int fd = open(path(leaf).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	write(fd, text, strlen(text));
	close(fd);
}


static const char*
errno_name(int error)
{
	switch (error) {
		case 0: return "0";
		case EBADF: return "EBADF";
		case EINVAL: return "EINVAL";
		case EEXIST: return "EEXIST";
		case ENOENT: return "ENOENT";
		case ENOTDIR: return "ENOTDIR";
		case EAGAIN: return "EAGAIN";
		case EFAULT: return "EFAULT";
	}
	return strerror(error);
}


#define EXPECT_ERRNO(name, expected, call) \
	do { \
		errno = 0; \
		int _result = (call); \
		report(name, _result == -1 && errno == (expected), \
			"result %d, errno %s (want %s)", _result, errno_name(errno), \
			errno_name(expected)); \
	} while (false)


// #pragma mark -


static void
test_arguments()
{
	int fd = inotify_init();
	report("init", fd >= 0, "%s", fd >= 0 ? "" : strerror(errno));
	EXPECT_ERRNO("init1_bad_flags", EINVAL, inotify_init1(0x10));

	int flagged = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	char buffer[256];
	errno = 0;
	ssize_t bytes = read(flagged, buffer, sizeof(buffer));
	report("init1_flags", (fcntl(flagged, F_GETFD) & FD_CLOEXEC) != 0
			&& (fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0 && bytes == -1
			&& errno == EAGAIN,
		"read %zd: %s", bytes, errno_name(errno));
	close(flagged);

	EXPECT_ERRNO("add_bad_fd", EBADF, inotify_add_watch(9999, sBase,
		IN_CREATE));
	int pipes[2];
	pipe(pipes);
	EXPECT_ERRNO("add_not_inotify", EINVAL, inotify_add_watch(pipes[0], sBase,
		IN_CREATE));
	close(pipes[0]);
	close(pipes[1]);
	EXPECT_ERRNO("add_missing_path", ENOENT, inotify_add_watch(fd,
		path("does-not-exist").c_str(), IN_CREATE));
	EXPECT_ERRNO("add_no_events", EINVAL, inotify_add_watch(fd, sBase,
		IN_ONLYDIR));
	EXPECT_ERRNO("add_mask_add_and_create", EINVAL, inotify_add_watch(fd,
		sBase, IN_CREATE | IN_MASK_ADD | IN_MASK_CREATE));
	EXPECT_ERRNO("add_bad_path_pointer", EFAULT, inotify_add_watch(fd,
		(const char*)8, IN_CREATE));

	write_file("plain", "x");
	EXPECT_ERRNO("onlydir_on_file", ENOTDIR, inotify_add_watch(fd,
		path("plain").c_str(), IN_MODIFY | IN_ONLYDIR));

	int wd = inotify_add_watch(fd, sBase, IN_CREATE);
	int again = inotify_add_watch(fd, sBase, IN_DELETE);
	report("same_node_same_wd", wd > 0 && again == wd, "%d, %d", wd, again);
	EXPECT_ERRNO("mask_create_existing", EEXIST, inotify_add_watch(fd, sBase,
		IN_CREATE | IN_MASK_CREATE));

	EXPECT_ERRNO("rm_bad_wd", EINVAL, inotify_rm_watch(fd, 12345));
	int removed = inotify_rm_watch(fd, wd);
	std::vector<Event> events = collect(fd);
	report("rm_watch_ignored", removed == 0 && events.size() == 1
			&& events[0].wd == wd && events[0].mask == IN_IGNORED,
		"%d: %s", removed, describe(events).c_str());
	EXPECT_ERRNO("rm_twice", EINVAL, inotify_rm_watch(fd, wd));

	struct stat st;
	report("fstat", fstat(fd, &st) == 0, "mode 0%o", (unsigned)st.st_mode);
	errno = 0;
	bytes = write(fd, "x", 1);
	report("write_fails", bytes == -1, "%zd %s", bytes, errno_name(errno));

	unlink(path("plain").c_str());
	close(fd);
}


static void
test_directory_events()
{
	int fd = inotify_init1(IN_NONBLOCK);
	write_file("existing", "old");
	int wd = inotify_add_watch(fd, sBase, IN_ALL_EVENTS);
	report("dir_watch", wd > 0, "%d", wd);

	// Create, write, close.
	int file = open(path("a").c_str(), O_WRONLY | O_CREAT, 0644);
	write(file, "hello", 5);
	close(file);
	std::vector<Event> events = collect(fd);
	int create = find(events, IN_CREATE, "a", wd);
	int opened = find(events, IN_OPEN, "a", wd);
	int modify = find(events, IN_MODIFY, "a", wd);
	int closeWrite = find(events, IN_CLOSE_WRITE, "a", wd);
	report("create_write_close", create >= 0 && opened > create
			&& modify > opened && closeWrite > modify,
		"%s", describe(events).c_str());

	// Read-only access.
	file = open(path("a").c_str(), O_RDONLY);
	char buffer[16];
	read(file, buffer, sizeof(buffer));
	close(file);
	events = collect(fd);
	report("open_read_close", find(events, IN_OPEN, "a") >= 0
			&& find(events, IN_ACCESS, "a") >= 0
			&& find(events, IN_CLOSE_NOWRITE, "a") >= 0,
		"%s", describe(events).c_str());

	// An entry that existed before the watch is reported by name.
	file = open(path("existing").c_str(), O_WRONLY | O_APPEND);
	write(file, "+", 1);
	close(file);
	events = collect(fd);
	report("existing_entry_named", find(events, IN_MODIFY, "existing") >= 0,
		"%s", describe(events).c_str());

	// Metadata: chmod, truncate, attributes.
	chmod(path("a").c_str(), 0600);
	events = collect(fd);
	report("chmod_attrib", find(events, IN_ATTRIB, "a") >= 0, "%s",
		describe(events).c_str());
	truncate(path("a").c_str(), 2);
	events = collect(fd);
	report("truncate_modify", find(events, IN_MODIFY, "a") >= 0, "%s",
		describe(events).c_str());
	file = open(path("a").c_str(), O_RDONLY);
	fs_write_attr(file, "renga:test", B_STRING_TYPE, 0, "v", 2);
	close(file);
	events = collect(fd);
	report("xattr_attrib", find(events, IN_ATTRIB, "a") >= 0, "%s",
		describe(events).c_str());

	// Coalescing: identical consecutive events merge.
	file = open(path("a").c_str(), O_WRONLY);
	for (int i = 0; i < 100; i++)
		write(file, "z", 1);
	close(file);
	events = collect(fd);
	int modifies = 0;
	for (size_t i = 0; i < events.size(); i++) {
		if (events[i].mask == IN_MODIFY)
			modifies++;
	}
	report("coalesce", modifies == 1, "%d IN_MODIFY for 100 writes: %s",
		modifies, describe(events).c_str());

	// Rename within the directory: one cookie for both halves.
	rename(path("a").c_str(), path("b").c_str());
	events = collect(fd);
	int from = find(events, IN_MOVED_FROM, "a", wd);
	int to = find(events, IN_MOVED_TO, "b", wd);
	report("rename_cookie", from >= 0 && to == from + 1
			&& events[from].cookie != 0
			&& events[from].cookie == events[to].cookie,
		"%s", describe(events).c_str());

	// Directories: IN_ISDIR.
	mkdir(path("sub").c_str(), 0755);
	int subWD = inotify_add_watch(fd, path("sub").c_str(), IN_ALL_EVENTS);
	events = collect(fd);
	report("mkdir_isdir", find(events, IN_CREATE | IN_ISDIR, "sub", wd) >= 0
		&& subWD > 0 && subWD != wd, "%s", describe(events).c_str());

	// Move into a watched subdirectory: the cookie pairs the two watches.
	rename(path("b").c_str(), path("sub/c").c_str());
	events = collect(fd);
	from = find(events, IN_MOVED_FROM, "b", wd);
	to = find(events, IN_MOVED_TO, "c", subWD);
	report("move_between_dirs", from >= 0 && to >= 0
			&& events[from].cookie == events[to].cookie
			&& events[from].cookie != 0,
		"%s", describe(events).c_str());
	// ...and the new name is known to the subdirectory's watch.
	file = open(path("sub/c").c_str(), O_WRONLY);
	write(file, "y", 1);
	close(file);
	events = collect(fd);
	report("moved_entry_named", find(events, IN_MODIFY, "c", subWD) >= 0,
		"%s", describe(events).c_str());

	// Delete: entry, then the directory itself.
	unlink(path("sub/c").c_str());
	rmdir(path("sub").c_str());
	events = collect(fd);
	int deleted = find(events, IN_DELETE, "c", subWD);
	int deleteSelf = find(events, IN_DELETE_SELF, "", subWD);
	int ignored = find(events, IN_IGNORED, "", subWD);
	int parentDelete = find(events, IN_DELETE | IN_ISDIR, "sub", wd);
	report("delete_and_delete_self", deleted >= 0 && deleteSelf > deleted
			&& ignored > deleteSelf && parentDelete >= 0,
		"%s", describe(events).c_str());
	EXPECT_ERRNO("deleted_watch_gone", EINVAL, inotify_rm_watch(fd, subWD));

	unlink(path("existing").c_str());
	collect(fd);
	close(fd);
}


static void
test_file_watch_and_flags()
{
	int fd = inotify_init1(IN_NONBLOCK);
	write_file("f", "1");
	int wd = inotify_add_watch(fd, path("f").c_str(),
		IN_MODIFY | IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF);
	write_file("f", "2");
	std::vector<Event> events = collect(fd);
	report("file_watch_modify", find(events, IN_MODIFY, "", wd) >= 0, "%s",
		describe(events).c_str());

	rename(path("f").c_str(), path("g").c_str());
	events = collect(fd);
	report("move_self", find(events, IN_MOVE_SELF, "", wd) >= 0, "%s",
		describe(events).c_str());
	unlink(path("g").c_str());
	events = collect(fd);
	int deleteSelf = find(events, IN_DELETE_SELF, "", wd);
	report("file_delete_self", deleteSelf >= 0
			&& find(events, IN_IGNORED, "", wd) > deleteSelf,
		"%s", describe(events).c_str());

	// IN_ONESHOT: one event, then IN_IGNORED.
	int oneshot = inotify_add_watch(fd, sBase, IN_CREATE | IN_ONESHOT);
	write_file("one", "1");
	write_file("two", "2");
	events = collect(fd);
	report("oneshot", events.size() == 2 && events[0].mask == IN_CREATE
			&& events[0].name == "one" && events[1].mask == IN_IGNORED
			&& events[1].wd == oneshot,
		"%s", describe(events).c_str());

	// Replacing versus adding to the mask.
	int dirWD = inotify_add_watch(fd, sBase, IN_CREATE);
	inotify_add_watch(fd, sBase, IN_DELETE);
	write_file("three", "3");
	unlink(path("three").c_str());
	events = collect(fd);
	bool replaced = find(events, IN_CREATE, "three") < 0
		&& find(events, IN_DELETE, "three", dirWD) >= 0;
	inotify_add_watch(fd, sBase, IN_CREATE | IN_MASK_ADD);
	write_file("four", "4");
	unlink(path("four").c_str());
	events = collect(fd);
	report("mask_replace_and_add", replaced
			&& find(events, IN_CREATE, "four", dirWD) >= 0
			&& find(events, IN_DELETE, "four", dirWD) >= 0,
		"%s", describe(events).c_str());
	inotify_rm_watch(fd, dirWD);
	collect(fd);

	// IN_DONT_FOLLOW watches the link itself.
	write_file("target", "t");
	symlink(path("target").c_str(), path("link").c_str());
	int targetWD = inotify_add_watch(fd, path("target").c_str(), IN_ATTRIB);
	int followed = inotify_add_watch(fd, path("link").c_str(), IN_ATTRIB);
	int linkWD = inotify_add_watch(fd, path("link").c_str(),
		IN_ATTRIB | IN_DONT_FOLLOW);
	report("dont_follow", followed == targetWD && linkWD > 0
		&& linkWD != targetWD, "target %d, followed %d, link %d", targetWD,
		followed, linkWD);
	unlink(path("link").c_str());
	unlink(path("target").c_str());
	unlink(path("one").c_str());
	unlink(path("two").c_str());
	collect(fd);
	close(fd);
}


struct Creator {
	const char*	leaf;
	bigtime_t	delay;
};


static void*
delayed_create(void* data)
{
	Creator* creator = (Creator*)data;
	usleep(creator->delay);
	write_file(creator->leaf, "x");
	return NULL;
}


static void
test_reading_and_polling()
{
	int fd = inotify_init();
	int wd = inotify_add_watch(fd, sBase, IN_CREATE | IN_DELETE);

	// A blocking read is woken by an event from another thread.
	Creator creator = { "blocking", 100000 };
	pthread_t thread;
	pthread_create(&thread, NULL, delayed_create, &creator);
	char buffer[4096] __attribute__((aligned(8)));
	bigtime_t start = system_time();
	ssize_t bytes = read(fd, buffer, sizeof(buffer));
	bigtime_t waited = system_time() - start;
	pthread_join(thread, NULL);
	struct inotify_event* event = (struct inotify_event*)buffer;
	report("blocking_read", bytes >= (ssize_t)sizeof(*event)
			&& event->wd == wd && event->mask == IN_CREATE
			&& strcmp(event->name, "blocking") == 0 && waited >= 80000,
		"%zd bytes after %lld us", bytes, (long long)waited);

	// Record layout: len is the padded name length.
	report("record_layout", event->len % sizeof(struct inotify_event) == 0
			&& event->len > strlen("blocking")
			&& bytes == (ssize_t)(sizeof(*event) + event->len),
		"len %u, %zd bytes", event->len, bytes);

	// poll(), select(), epoll, FIONREAD.
	struct pollfd pfd = { fd, POLLIN, 0 };
	int idle = poll(&pfd, 1, 0);
	unlink(path("blocking").c_str());
	int ready = poll(&pfd, 1, 1000);
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(fd, &readSet);
	struct timeval timeout = { 0, 0 };
	int selected = select(fd + 1, &readSet, NULL, NULL, &timeout);
	int ep = epoll_create1(0);
	struct epoll_event epollEvent = {};
	epollEvent.events = EPOLLIN | EPOLLET;
	epollEvent.data.u64 = 77;
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &epollEvent);
	struct epoll_event got = {};
	int epolled = epoll_wait(ep, &got, 1, 0);
	int queued = -1;
	ioctl(fd, FIONREAD, &queued);
	report("poll_select_epoll_fionread", idle == 0 && ready == 1
			&& selected == 1 && epolled == 1 && got.data.u64 == 77
			&& queued == (int)(sizeof(struct inotify_event) + 16),
		"poll %d/%d, select %d, epoll %d, FIONREAD %d", idle, ready,
		selected, epolled, queued);

	// A buffer too small for the first record fails; an exact one works.
	errno = 0;
	bytes = read(fd, buffer, sizeof(struct inotify_event) + 4);
	int smallErrno = errno;
	bytes = read(fd, buffer, queued);
	report("read_buffer_size", smallErrno == EINVAL && bytes == queued,
		"small: %s, exact %zd", errno_name(smallErrno), bytes);

	// epoll edge-triggered: reported again on the next event.
	epolled = epoll_wait(ep, &got, 1, 0);
	write_file("edge", "x");
	int epolledAgain = epoll_wait(ep, &got, 1, 1000);
	report("epoll_edge", epolled == 0 && epolledAgain == 1, "%d then %d",
		epolled, epolledAgain);
	collect(fd);
	unlink(path("edge").c_str());
	collect(fd);
	close(ep);

	// fork(): the child's events reach the parent's instance, which the
	// child shares.
	pid_t child = fork();
	if (child == 0) {
		write_file("from-child", "c");
		_exit(0);
	}
	int status;
	waitpid(child, &status, 0);
	std::vector<Event> events = collect(fd);
	report("fork", find(events, IN_CREATE, "from-child", wd) >= 0, "%s",
		describe(events).c_str());
	unlink(path("from-child").c_str());
	collect(fd);

	close(fd);
}


static void
test_overflow()
{
	int fd = inotify_init1(IN_NONBLOCK);
	inotify_add_watch(fd, sBase, IN_MODIFY);
	int a = open(path("ov-a").c_str(), O_WRONLY | O_CREAT, 0644);
	int b = open(path("ov-b").c_str(), O_WRONLY | O_CREAT, 0644);

	// Alternating writes defeat merging: 20000 distinct events, more than
	// the queue limit of 16384.
	bigtime_t start = system_time();
	for (int i = 0; i < 10000; i++) {
		write(a, "a", 1);
		write(b, "b", 1);
	}
	bigtime_t elapsed = system_time() - start;
	close(a);
	close(b);

	int count = 0;
	int overflows = 0;
	int lastWD = 0;
	uint32_t lastMask = 0;
	char buffer[65536] __attribute__((aligned(8)));
	while (true) {
		ssize_t bytes = read(fd, buffer, sizeof(buffer));
		if (bytes <= 0)
			break;
		for (ssize_t offset = 0; offset < bytes;) {
			struct inotify_event* event
				= (struct inotify_event*)(buffer + offset);
			count++;
			if (event->mask == IN_Q_OVERFLOW)
				overflows++;
			lastWD = event->wd;
			lastMask = event->mask;
			offset += sizeof(struct inotify_event) + event->len;
		}
	}
	report("overflow", count == 16384 && overflows == 1 && lastWD == -1
			&& lastMask == IN_Q_OVERFLOW,
		"%d events, %d overflow, last wd %d mask %#x (20000 writes in "
		"%lld ms)", count, overflows, lastWD, lastMask,
		(long long)elapsed / 1000);

	// After reading, events are queued again.
	write_file("ov-a", "again");
	std::vector<Event> events = collect(fd);
	report("after_overflow", find(events, IN_MODIFY, "ov-a") >= 0, "%s",
		describe(events).c_str());

	unlink(path("ov-a").c_str());
	unlink(path("ov-b").c_str());
	close(fd);
}


struct Worker {
	int		index;
	int		rounds;
};


static void*
churn_files(void* data)
{
	Worker* worker = (Worker*)data;
	char name[64], renamed[64];
	for (int i = 0; i < worker->rounds; i++) {
		snprintf(name, sizeof(name), "w%d-%d", worker->index, i);
		snprintf(renamed, sizeof(renamed), "r%d-%d", worker->index, i);
		write_file(name, "x");
		rename(path(name).c_str(), path(renamed).c_str());
		unlink(path(renamed).c_str());
	}
	return NULL;
}


static void
test_stress()
{
	int fd = inotify_init1(IN_NONBLOCK);
	int wd = inotify_add_watch(fd, sBase,
		IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
	int ep = epoll_create1(0);
	struct epoll_event epollEvent = {};
	epollEvent.events = EPOLLIN;
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &epollEvent);

	const int kThreads = 4;
	const int kRounds = 300;
	pthread_t threads[kThreads];
	Worker workers[kThreads];
	for (int i = 0; i < kThreads; i++) {
		workers[i].index = i;
		workers[i].rounds = kRounds;
		pthread_create(&threads[i], NULL, churn_files, &workers[i]);
	}

	// Read concurrently, woken through epoll.
	int creates = 0, deletes = 0, froms = 0, tos = 0, matched = 0, other = 0;
	std::vector<uint32_t> pendingCookies;
	char buffer[65536] __attribute__((aligned(8)));
	bigtime_t start = system_time();
	int idle = 0;
	while (system_time() - start < 60000000 && idle < 5) {
		struct epoll_event got;
		if (epoll_wait(ep, &got, 1, 200) == 0) {
			idle++;
			continue;
		}
		idle = 0;
		ssize_t bytes = read(fd, buffer, sizeof(buffer));
		for (ssize_t offset = 0; offset < bytes;) {
			struct inotify_event* event
				= (struct inotify_event*)(buffer + offset);
			offset += sizeof(struct inotify_event) + event->len;
			if (event->wd != wd) {
				other++;
				continue;
			}
			switch (event->mask) {
				case IN_CREATE: creates++; break;
				case IN_DELETE: deletes++; break;
				case IN_MOVED_FROM:
					froms++;
					pendingCookies.push_back(event->cookie);
					break;
				case IN_MOVED_TO:
					tos++;
					for (size_t i = 0; i < pendingCookies.size(); i++) {
						if (pendingCookies[i] == event->cookie) {
							pendingCookies.erase(pendingCookies.begin() + i);
							matched++;
							break;
						}
					}
					break;
				default:
					other++;
			}
		}
	}
	for (int i = 0; i < kThreads; i++)
		pthread_join(threads[i], NULL);

	const int kTotal = kThreads * kRounds;
	report("stress", creates == kTotal && deletes == kTotal && froms == kTotal
			&& tos == kTotal && matched == kTotal && other == 0,
		"create %d delete %d from %d to %d matched %d other %d of %d, "
		"%lld ms", creates, deletes, froms, tos, matched, other, kTotal,
		(long long)(system_time() - start) / 1000);
	close(ep);
	close(fd);

	// Many instances with watches, closed while events are pending.
	int created = 0;
	for (int i = 0; i < 200; i++) {
		int instance = inotify_init();
		if (inotify_add_watch(instance, sBase, IN_ALL_EVENTS) > 0)
			created++;
		if (i % 20 == 0)
			write_file("churn", "x");
		close(instance);
	}
	unlink(path("churn").c_str());
	report("instances_closed", created == 200, "%d of 200", created);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	snprintf(sBase, sizeof(sBase), "/tmp/inotify_test_%d", (int)getpid());
	if (mkdir(sBase, 0755) != 0) {
		snprintf(sBase, sizeof(sBase), "/boot/system/cache/tmp/inotify_%d",
			(int)getpid());
		mkdir(sBase, 0755);
	}

	test_arguments();
	test_directory_events();
	test_file_watch_and_flags();
	test_reading_and_polling();
	test_overflow();
	test_stress();

	rmdir(sBase);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
