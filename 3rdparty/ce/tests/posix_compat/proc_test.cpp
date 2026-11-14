/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test of the small /proc native programs get (Chromium R-1,
	R-2, R-6):

	- /proc/self/fd/N, /proc/<pid>/fd/N and /dev/fd/N opened O_RDONLY give
	  a new open file description for the same file (Chromium's read-only
	  memfd descriptor): its own access mode and position, the same node,
	  the file's seals, PROT_READ MAP_SHARED works and sees writes made
	  through the other descriptor, PROT_WRITE MAP_SHARED and a later
	  mprotect(PROT_WRITE) fail; O_CLOEXEC honoured; O_TRUNC refused on a
	  sealed file; a closed FD is ENOENT, a socket ENXIO;
	- /proc/self/maps in Linux's format, with the program's and libroot's
	  paths and offsets from which the load base can be computed;
	- /proc/sys/fs/inotify/max_user_watches = 8192, read-only;
	- other /proc files (Linux-only) are not offered to native programs.
	Output format: see 3rdparty/ce/README.md.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>
#include <image.h>


#define SUITE "proc"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
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


static ssize_t
read_all(const char* path, char* buffer, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	size_t total = 0;
	while (total < size - 1) {
		ssize_t bytes = read(fd, buffer + total, size - 1 - total);
		if (bytes <= 0)
			break;
		total += bytes;
	}
	buffer[total] = '\0';
	close(fd);
	return total;
}


static void
test_reopen()
{
	int memfd = memfd_create("proc_test", MFD_ALLOW_SEALING | MFD_CLOEXEC);
	char page[4096];
	memset(page, 'a', sizeof(page));
	bool setup = memfd >= 0 && write(memfd, page, sizeof(page))
		== (ssize_t)sizeof(page);
	report("memfd_setup", setup, "memfd %d", memfd);
	if (!setup)
		return;

	char path[64];
	snprintf(path, sizeof(path), "/proc/self/fd/%d", memfd);
	int readOnly = open(path, O_RDONLY | O_CLOEXEC);
	int error = readOnly < 0 ? errno : 0;
	report("self_fd_open", readOnly >= 0, "%s: %s", path, strerror(error));
	if (readOnly < 0)
		return;

	// A new open file description: its own access mode and position.
	int flags = fcntl(readOnly, F_GETFL);
	int originalFlags = fcntl(memfd, F_GETFL);
	report("self_fd_mode", (flags & O_ACCMODE) == O_RDONLY
			&& (originalFlags & O_ACCMODE) == O_RDWR,
		"reopened %#x, original %#x", flags, originalFlags);
	off_t position = lseek(readOnly, 0, SEEK_CUR);
	off_t originalPosition = lseek(memfd, 0, SEEK_CUR);
	report("self_fd_position", position == 0 && originalPosition == 4096,
		"reopened at %lld, original at %lld", (long long)position,
		(long long)originalPosition);
	struct stat a, b;
	fstat(readOnly, &a);
	fstat(memfd, &b);
	report("self_fd_same_file", a.st_dev == b.st_dev && a.st_ino == b.st_ino
			&& a.st_size == 4096, "dev %d/%d, ino %lld/%lld, size %lld",
		(int)a.st_dev, (int)b.st_dev, (long long)a.st_ino,
		(long long)b.st_ino, (long long)a.st_size);
	int fdFlags = fcntl(readOnly, F_GETFD);
	report("self_fd_cloexec", (fdFlags & FD_CLOEXEC) != 0, "F_GETFD %#x",
		fdFlags);
	char byte = 0;
	ssize_t written = write(readOnly, "x", 1);
	error = written < 0 ? errno : 0;
	report("self_fd_write_refused", written < 0 && error == EBADF,
		"write: %zd, %s", written, strerror(error));

	// Mappings: read-only shared works and sees the other FD's writes;
	// writable shared and a later mprotect(PROT_WRITE) do not.
	void* view = mmap(NULL, 4096, PROT_READ, MAP_SHARED, readOnly, 0);
	bool mapped = view != MAP_FAILED;
	if (mapped) {
		pwrite(memfd, "Z", 1, 10);
		byte = ((char*)view)[10];
	}
	report("mmap_read_shared", mapped && byte == 'Z', "mapped %d, byte %c",
		mapped, byte != 0 ? byte : '-');
	void* writable = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
		readOnly, 0);
	error = writable == MAP_FAILED ? errno : 0;
	report("mmap_write_shared_refused", writable == MAP_FAILED,
		"%s", strerror(error));
	if (writable != MAP_FAILED)
		munmap(writable, 4096);
	int protect = mapped ? mprotect(view, 4096, PROT_READ | PROT_WRITE) : 0;
	error = protect != 0 ? errno : 0;
	report("mprotect_write_refused", mapped && protect != 0, "%s",
		strerror(error));
	if (mapped)
		munmap(view, 4096);

	// Seals belong to the file.
	fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
	int seals = fcntl(readOnly, F_GET_SEALS);
	report("seals_shared", seals >= 0 && (seals & F_SEAL_WRITE) != 0
			&& (seals & F_SEAL_SHRINK) != 0, "F_GET_SEALS %#x", seals);
	int truncating = open(path, O_RDWR | O_TRUNC);
	error = truncating < 0 ? errno : 0;
	report("sealed_truncate_refused", truncating < 0 && error == EPERM,
		"O_TRUNC: %d, %s", truncating, strerror(error));
	if (truncating >= 0)
		close(truncating);

	// /dev/fd/N and /proc/<pid>/fd/N do the same.
	snprintf(path, sizeof(path), "/dev/fd/%d", memfd);
	int devFD = open(path, O_RDONLY);
	error = devFD < 0 ? errno : 0;
	flags = devFD >= 0 ? fcntl(devFD, F_GETFL) : -1;
	fdFlags = devFD >= 0 ? fcntl(devFD, F_GETFD) : -1;
	report("dev_fd_open", devFD >= 0 && (flags & O_ACCMODE) == O_RDONLY
			&& (fdFlags & FD_CLOEXEC) == 0,
		"%s: %s, F_GETFL %#x, F_GETFD %#x", path, strerror(error), flags,
		fdFlags);
	if (devFD >= 0)
		close(devFD);
	snprintf(path, sizeof(path), "/proc/%d/fd/%d", (int)getpid(), memfd);
	int pidFD = open(path, O_RDONLY);
	error = pidFD < 0 ? errno : 0;
	report("pid_fd_open", pidFD >= 0, "%s: %s", path, strerror(error));
	if (pidFD >= 0)
		close(pidFD);

	// A file that is not a memfd, read-write.
	char tempPath[] = "/tmp/proc_test_XXXXXX";
	int temp = mkstemp(tempPath);
	write(temp, "hello", 5);
	snprintf(path, sizeof(path), "/proc/self/fd/%d", temp);
	int again = open(path, O_RDWR);
	char text[8] = {};
	ssize_t got = again >= 0 ? read(again, text, 5) : -1;
	report("file_reopen_rdwr", again >= 0 && got == 5
			&& strcmp(text, "hello") == 0, "read %zd \"%s\"", got, text);
	if (again >= 0)
		close(again);
	close(temp);
	unlink(tempPath);

	// Errors: a closed FD, a socket.
	int closed = open("/proc/self/fd/987", O_RDONLY);
	error = closed < 0 ? errno : 0;
	report("closed_fd_enoent", closed < 0 && error == ENOENT, "%s",
		strerror(error));
	int sockets[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	snprintf(path, sizeof(path), "/proc/self/fd/%d", sockets[0]);
	int socketFD = open(path, O_RDONLY);
	error = socketFD < 0 ? errno : 0;
	report("socket_enxio", socketFD < 0 && error == ENXIO, "%s",
		strerror(error));
	close(sockets[0]);
	close(sockets[1]);

	close(readOnly);
	close(memfd);
}


static void
test_maps()
{
	static char content[256 * 1024];
	ssize_t length = read_all("/proc/self/maps", content, sizeof(content));
	report("maps_read", length > 0, "%zd bytes", length);
	if (length <= 0)
		return;

	image_info app = {}, libroot = {};
	int32 cookie = 0;
	image_info info;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (info.type == B_APP_IMAGE)
			app = info;
		else if (strstr(info.name, "/libroot.so") != NULL)
			libroot = info;
	}

	// Every line: start-end perms offset dev inode [path].
	int lines = 0, malformed = 0, appExecutable = 0, librootLines = 0;
	bool baseOK = false;
	char firstBad[160] = "-";
	uintptr_t mainAddress = (uintptr_t)&test_maps;
	for (char* line = strtok(content, "\n"); line != NULL;
			line = strtok(NULL, "\n")) {
		lines++;
		unsigned long start, end, offset, inode;
		char perms[5];
		unsigned major, minor;
		int pathStart = 0;
		if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %n", &start, &end, perms,
				&offset, &major, &minor, &inode, &pathStart) < 7
			|| start >= end || strlen(perms) != 4) {
			if (malformed++ == 0)
				strlcpy(firstBad, line, sizeof(firstBad));
			continue;
		}
		const char* path = line + pathStart;
		if (strcmp(path, app.name) == 0 && perms[2] == 'x') {
			appExecutable++;
			if (mainAddress >= start && mainAddress < end) {
				// The load base a symbolizer computes is start - offset;
				// the test program is linked at 0, so it is the address
				// of its first segment.
				baseOK = start - offset == (uintptr_t)app.text
					&& inode == (unsigned long)app.node;
			}
		}
		if (strcmp(path, libroot.name) == 0)
			librootLines++;
	}
	report("maps_format", lines > 5 && malformed == 0,
		"%d lines, %d malformed (first: %s)", lines, malformed, firstBad);
	report("maps_program", appExecutable >= 1 && baseOK,
		"%d executable lines for %s, load base and inode %s", appExecutable,
		app.name, baseOK ? "right" : "wrong");
	report("maps_libroot", librootLines >= 2, "%d lines for %s",
		librootLines, libroot.name);

	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/maps", (int)getpid());
	length = read_all(path, content, sizeof(content));
	report("maps_by_pid", length > 0, "%s: %zd", path, length);
}


static void
test_sys()
{
	char text[64];
	ssize_t length = read_all("/proc/sys/fs/inotify/max_user_watches", text,
		sizeof(text));
	report("inotify_max_user_watches", length > 0
			&& strcmp(text, "8192\n") == 0, "%zd: \"%s\"", length,
		length > 0 ? text : strerror(-length));
	int fd = open("/proc/sys/fs/inotify/max_user_watches", O_WRONLY);
	int error = fd < 0 ? errno : 0;
	report("inotify_read_only", fd < 0 && error == EACCES, "%s",
		strerror(error));
	if (fd >= 0)
		close(fd);

	// Linux-only files are not offered.
	fd = open("/proc/cpuinfo", O_RDONLY);
	error = fd < 0 ? errno : 0;
	report("linux_only_absent", fd < 0 && error == ENOENT, "/proc/cpuinfo: %s",
		strerror(error));
	if (fd >= 0)
		close(fd);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	test_reopen();
	test_maps();
	test_sys();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
