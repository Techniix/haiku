/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest conformance test for memfd_create() (libgnu).

	Checks the behaviour Linux documents in memfd_create(2) that real users
	depend on: Wayland clients and compositors (shared pixel buffers passed
	over a UNIX socket), Chromium/Electron IPC shared memory, Mesa, glib and
	systemd-style services. Output format: see 3rdparty/ce/README.md.
*/


#include <sys/mman.h>

#include <OS.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "memfd"


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


//!	Number of entries in the shared memory directory that look like ours.
static int
count_memfd_entries()
{
	DIR* dir = opendir("/var/shared_memory");
	if (dir == NULL)
		return -1;
	int count = 0;
	while (dirent* entry = readdir(dir)) {
		if (strncmp(entry->d_name, "memfd-", 6) == 0)
			count++;
	}
	closedir(dir);
	return count;
}


static void
test_arguments()
{
	errno = 0;
	int fd = memfd_create(NULL, 0);
	report("null_name_efault", fd < 0 && errno == EFAULT, "fd %d errno %s",
		fd, strerror(errno));

	char longName[300];
	memset(longName, 'x', sizeof(longName) - 1);
	longName[sizeof(longName) - 1] = '\0';
	errno = 0;
	fd = memfd_create(longName, 0);
	report("long_name_einval", fd < 0 && errno == EINVAL, "fd %d errno %s",
		fd, strerror(errno));

	longName[249] = '\0';
	fd = memfd_create(longName, 0);
	report("max_name_ok", fd >= 0, "fd %d errno %s", fd,
		fd < 0 ? strerror(errno) : "-");
	if (fd >= 0)
		close(fd);

	errno = 0;
	fd = memfd_create("bad", 0x80000000U);
	report("bad_flags_einval", fd < 0 && errno == EINVAL, "fd %d errno %s",
		fd, strerror(errno));

	errno = 0;
	fd = memfd_create("huge", MFD_HUGETLB);
	report("hugetlb_einval", fd < 0 && errno == EINVAL, "fd %d errno %s",
		fd, strerror(errno));

	errno = 0;
	fd = memfd_create("both", MFD_NOEXEC_SEAL | MFD_EXEC);
	report("noexec_exec_einval", fd < 0 && errno == EINVAL,
		"fd %d errno %s", fd, strerror(errno));

	fd = memfd_create("sealing", MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
	report("allow_sealing_accepted", fd >= 0, "fd %d errno %s", fd,
		fd < 0 ? strerror(errno) : "-");
	if (fd >= 0)
		close(fd);

	// Names with slashes and odd characters are fine (Linux: any bytes).
	fd = memfd_create("wayland/shm pool #1", 0);
	report("odd_name_ok", fd >= 0, "fd %d errno %s", fd,
		fd < 0 ? strerror(errno) : "-");
	if (fd >= 0)
		close(fd);
}


static void
test_basic()
{
	int before = count_memfd_entries();
	int fd = memfd_create("ce-test", 0);
	int after = count_memfd_entries();
	report("create", fd >= 0, "fd %d (%s)", fd,
		fd < 0 ? strerror(errno) : "ok");
	if (fd < 0)
		return;

	// The file is anonymous: no name is left in the shared memory volume.
	report("anonymous", before >= 0 && after == before,
		"memfd entries before %d after %d", before, after);

	struct stat st;
	int statResult = fstat(fd, &st);
	report("fstat_empty_regular", statResult == 0 && S_ISREG(st.st_mode)
			&& st.st_size == 0,
		"mode 0%o size %lld", (unsigned)st.st_mode, (long long)st.st_size);

	report("not_cloexec", (fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0,
		"F_GETFD 0x%x", fcntl(fd, F_GETFD));
	report("rdwr", (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR,
		"F_GETFL 0x%x", fcntl(fd, F_GETFL));

	// write() grows the file; read() and pread() see the data.
	const char message[] = "hello from memfd";
	ssize_t written = write(fd, message, sizeof(message));
	char buffer[64] = {};
	ssize_t got = pread(fd, buffer, sizeof(buffer), 0);
	fstat(fd, &st);
	report("write_read", written == (ssize_t)sizeof(message)
			&& got == (ssize_t)sizeof(message)
			&& memcmp(buffer, message, sizeof(message)) == 0
			&& st.st_size == (off_t)sizeof(message),
		"wrote %zd read %zd size %lld", written, got, (long long)st.st_size);

	// ftruncate() + two MAP_SHARED mappings see each other's stores, and
	// read() sees what was stored through a mapping.
	const size_t kSize = 256 * 1024;
	int truncResult = ftruncate(fd, kSize);
	fstat(fd, &st);
	uint8_t* first = (uint8_t*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	uint8_t* second = (uint8_t*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	bool mapped = first != MAP_FAILED && second != MAP_FAILED;
	bool coherent = false;
	bool readSees = false;
	bool zeroTail = false;
	if (mapped) {
		zeroTail = first[kSize - 1] == 0 && first[kSize / 2] == 0;
		for (size_t i = 0; i < kSize; i += 4096)
			first[i] = (uint8_t)(i / 4096 + 1);
		coherent = true;
		for (size_t i = 0; i < kSize; i += 4096) {
			if (second[i] != (uint8_t)(i / 4096 + 1))
				coherent = false;
		}
		uint8_t byte = 0;
		readSees = pread(fd, &byte, 1, 8192) == 1 && byte == 3;
	}
	report("ftruncate_mmap_shared", truncResult == 0
			&& st.st_size == (off_t)kSize && mapped && zeroTail && coherent
			&& readSees,
		"size %lld mapped %d zero-filled %d coherent %d read-sees %d",
		(long long)st.st_size, mapped, zeroTail, coherent, readSees);

	// Shrinking works too (sealing is what would forbid it).
	if (mapped) {
		munmap(second, kSize);
		munmap(first, kSize);
	}
	truncResult = ftruncate(fd, 4096);
	fstat(fd, &st);
	report("shrink", truncResult == 0 && st.st_size == 4096, "size %lld",
		(long long)st.st_size);

	// fork(): the child writes through its own mapping, the parent sees it.
	uint8_t* shared = (uint8_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	pid_t child = fork();
	if (child == 0) {
		shared[100] = 0x5a;
		_exit(0);
	}
	int status = -1;
	waitpid(child, &status, 0);
	report("fork_shared", shared != MAP_FAILED && child > 0
			&& WIFEXITED(status) && shared[100] == 0x5a,
		"byte 0x%02x", shared != MAP_FAILED ? shared[100] : 0);
	if (shared != MAP_FAILED)
		munmap(shared, 4096);

	close(fd);
}


static void
test_cloexec()
{
	int fd = memfd_create("cloexec", MFD_CLOEXEC);
	report("cloexec", fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0,
		"fd %d F_GETFD 0x%x", fd, fd >= 0 ? fcntl(fd, F_GETFD) : -1);
	if (fd >= 0)
		close(fd);
}


//!	The Wayland pattern: pass the fd over a UNIX socket and map it there.
static void
test_scm_rights()
{
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
		report("scm_rights", false, "socketpair: %s", strerror(errno));
		return;
	}

	int fd = memfd_create("wl_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	ftruncate(fd, 4096);

	pid_t child = fork();
	if (child == 0) {
		close(sockets[0]);
		char data;
		char control[CMSG_SPACE(sizeof(int))];
		iovec iov = { &data, 1 };
		msghdr message = {};
		message.msg_iov = &iov;
		message.msg_iovlen = 1;
		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		if (recvmsg(sockets[1], &message, 0) != 1)
			_exit(2);
		cmsghdr* header = CMSG_FIRSTHDR(&message);
		if (header == NULL || header->cmsg_type != SCM_RIGHTS)
			_exit(3);
		int received;
		memcpy(&received, CMSG_DATA(header), sizeof(int));
		uint8_t* pixels = (uint8_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			MAP_SHARED, received, 0);
		if (pixels == MAP_FAILED)
			_exit(4);
		pixels[0] = 0xc3;
		pixels[4095] = 0x3c;
		_exit(0);
	}

	close(sockets[1]);
	char data = 'x';
	char control[CMSG_SPACE(sizeof(int))] = {};
	iovec iov = { &data, 1 };
	msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	cmsghdr* header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(header), &fd, sizeof(int));
	ssize_t sent = sendmsg(sockets[0], &message, 0);
	close(fd);
		// The parent's descriptor is gone; the child's keeps the file.

	int status = -1;
	waitpid(child, &status, 0);
	close(sockets[0]);

	report("scm_rights", sent == 1 && WIFEXITED(status)
			&& WEXITSTATUS(status) == 0,
		"sent %zd child status 0x%x", sent, status);
}


//!	The file outlives its descriptor while mapped, and goes away after.
static void
test_lifetime()
{
	int fd = memfd_create("lifetime", 0);
	ftruncate(fd, 8192);
	uint8_t* map = (uint8_t*)mmap(NULL, 8192, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	close(fd);
	bool alive = false;
	if (map != MAP_FAILED) {
		map[0] = 1;
		map[8191] = 2;
		alive = map[0] == 1 && map[8191] == 2;
		munmap(map, 8192);
	}
	report("mapping_outlives_fd", alive, "map %p",
		map == MAP_FAILED ? NULL : map);

}


//!	Creates, fills and closes "count" 1 MiB memfds; returns how many worked.
static int
churn(int count)
{
	const size_t kSize = 1024 * 1024;
	int created = 0;
	for (int i = 0; i < count; i++) {
		int fd = memfd_create("churn", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, kSize) != 0) {
			if (fd >= 0)
				close(fd);
			break;
		}
		uint8_t* pages = (uint8_t*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
		if (pages == MAP_FAILED) {
			close(fd);
			break;
		}
		memset(pages, i + 1, kSize);
		munmap(pages, kSize);
		close(fd);
		created++;
	}
	return created;
}


static int64
used_kib()
{
	system_info info;
	get_system_info(&info);
	return (int64)info.used_pages * (B_PAGE_SIZE / 1024);
}


//!	Memory of closed memfds is returned to the system.
static void
test_memory_returned()
{
	// The first round pays one-time costs (heap growth, ramfs and VM
	// bookkeeping); it is not part of the measurement.
	churn(20);

	int64 before = used_kib();
	bigtime_t start = system_time();
	int first = churn(100);
	int64 afterFirst = used_kib();
	int second = churn(100);
	int64 afterSecond = used_kib();
	bigtime_t elapsed = system_time() - start;

	// Kept pages would add about 100 MiB per round. Measured background
	// noise of used_pages under this load is about +-8 MiB and up to 15 MiB
	// (it goes both ways; 100 empty memfds cost under 20 KiB), so allow
	// 32 MiB: a real leak cannot hide below that, and noise does not reach
	// it.
	int64 growthFirst = afterFirst - before;
	int64 growthSecond = afterSecond - afterFirst;
	report("churn_200_mib_freed", first == 100 && second == 100
			&& count_memfd_entries() == 0 && growthFirst < 32 * 1024
			&& growthSecond < 32 * 1024,
		"%d+%d MiB in %lld us, used memory %+lld KiB then %+lld KiB, "
		"leftover entries %d", first, second, (long long)elapsed,
		(long long)growthFirst, (long long)growthSecond,
		count_memfd_entries());
}


struct ThreadArgs {
	int		created;
	int		failed;
};


static status_t
create_thread(void* data)
{
	ThreadArgs* args = (ThreadArgs*)data;
	for (int i = 0; i < 100; i++) {
		int fd = memfd_create("race", MFD_CLOEXEC);
		if (fd < 0) {
			args->failed++;
			continue;
		}
		args->created++;
		close(fd);
	}
	return B_OK;
}


static void
test_threads()
{
	const int kThreads = 4;
	ThreadArgs args[kThreads] = {};
	thread_id threads[kThreads];
	for (int i = 0; i < kThreads; i++) {
		threads[i] = spawn_thread(create_thread, "memfd race",
			B_NORMAL_PRIORITY, &args[i]);
		resume_thread(threads[i]);
	}
	int created = 0;
	int failed = 0;
	for (int i = 0; i < kThreads; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
		created += args[i].created;
		failed += args[i].failed;
	}
	report("concurrent_create", created == kThreads * 100 && failed == 0,
		"created %d failed %d", created, failed);
}


// #pragma mark - sealing


static int
sealable_memfd(size_t size)
{
	int fd = memfd_create("sealed", MFD_ALLOW_SEALING);
	if (fd >= 0)
		ftruncate(fd, size);
	return fd;
}


static void
test_sealing()
{
	// Without MFD_ALLOW_SEALING: F_SEAL_SEAL from the start.
	int fd = memfd_create("unsealable", 0);
	int seals = fcntl(fd, F_GET_SEALS);
	errno = 0;
	int result = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE);
	report("seal_without_allow", seals == F_SEAL_SEAL && result == -1
			&& errno == EPERM,
		"F_GET_SEALS %#x (want F_SEAL_SEAL), F_ADD_SEALS %d %s", seals,
		result, strerror(errno));
	close(fd);

	// With it: no seals yet; seals accumulate.
	fd = sealable_memfd(8192);
	seals = fcntl(fd, F_GET_SEALS);
	int added = fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
	int added2 = fcntl(fd, F_ADD_SEALS, F_SEAL_GROW);
	int after = fcntl(fd, F_GET_SEALS);
	report("seals_accumulate", seals == 0 && added == 0 && added2 == 0
			&& after == (F_SEAL_SHRINK | F_SEAL_GROW),
		"initial %#x, add %d %d, now %#x", seals, added, added2, after);

	// F_SEAL_SHRINK / F_SEAL_GROW.
	errno = 0;
	int shrink = ftruncate(fd, 4096);
	int shrinkErrno = errno;
	errno = 0;
	int grow = ftruncate(fd, 16384);
	int growErrno = errno;
	int same = ftruncate(fd, 8192);
	struct stat st;
	fstat(fd, &st);
	report("shrink_grow_sealed", shrink == -1 && shrinkErrno == EPERM
			&& grow == -1 && growErrno == EPERM && same == 0
			&& st.st_size == 8192,
		"shrink %d %s, grow %d %s, same size %d, size %lld", shrink,
		strerror(shrinkErrno), grow, strerror(growErrno), same,
		(long long)st.st_size);

	char data[4096];
	memset(data, 'x', sizeof(data));
	ssize_t inside = pwrite(fd, data, 4096, 4096);
	errno = 0;
	ssize_t beyond = pwrite(fd, data, 4096, 6144);
	int beyondErrno = errno;
	report("grow_sealed_write", inside == 4096 && beyond == -1
			&& beyondErrno == EPERM,
		"write inside %zd, write past the end %zd %s", inside, beyond,
		strerror(beyondErrno));

	errno = 0;
	int fallocated = posix_fallocate(fd, 0, 65536);
	report("grow_sealed_fallocate", fallocated == EPERM,
		"posix_fallocate %d %s", fallocated, strerror(fallocated));

	// Mapping the file privately is fine and doesn't change it.
	void* map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	report("map_shared_before_write_seal", map != MAP_FAILED, "%s",
		map == MAP_FAILED ? strerror(errno) : "mapped");

	// F_SEAL_WRITE fails with EBUSY while a writable shared mapping exists.
	errno = 0;
	result = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE);
	report("write_seal_ebusy", result == -1 && errno == EBUSY,
		"result %d %s", result, strerror(errno));
	munmap(map, 8192);

	// A read-only shared mapping does not stop it.
	void* readMap = mmap(NULL, 8192, PROT_READ, MAP_SHARED, fd, 0);
	result = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE);
	report("write_seal_with_readonly_map", result == 0
			&& (fcntl(fd, F_GET_SEALS) & F_SEAL_WRITE) != 0,
		"result %d %s", result, result != 0 ? strerror(errno) : "-");

	errno = 0;
	ssize_t written = pwrite(fd, data, 16, 0);
	int writeErrno = errno;
	report("write_sealed_write", written == -1 && writeErrno == EPERM,
		"pwrite %zd %s", written, strerror(writeErrno));

	errno = 0;
	void* writable = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		0);
	int mapErrno = errno;
	report("write_sealed_map_shared", writable == MAP_FAILED
			&& mapErrno == EPERM,
		"mmap %s", writable == MAP_FAILED ? strerror(mapErrno) : "succeeded");
	if (writable != MAP_FAILED)
		munmap(writable, 8192);

	errno = 0;
	result = mprotect(readMap, 4096, PROT_READ | PROT_WRITE);
	report("write_sealed_mprotect", result == -1,
		"mprotect of a read-only shared mapping to writable: %d %s", result,
		strerror(errno));
	munmap(readMap, 8192);

	// Private writable mappings are copies and still allowed (Linux too).
	void* privateMap = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE,
		fd, 0);
	bool privateOK = privateMap != MAP_FAILED;
	if (privateOK) {
		((char*)privateMap)[0] = 'p';
		char check = 0;
		pread(fd, &check, 1, 0);
		privateOK = check != 'p';
		munmap(privateMap, 8192);
	}
	report("write_sealed_map_private", privateOK, "%s",
		privateMap == MAP_FAILED ? strerror(errno) : "copy-on-write");

	// The content is still readable.
	char back[4];
	ssize_t readBack = pread(fd, back, 4, 4096);
	report("write_sealed_read", readBack == 4 && memcmp(back, "xxxx", 4) == 0,
		"pread %zd", readBack);

	// F_SEAL_SEAL ends it; seals are shared by all descriptors and
	// processes.
	fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL);
	errno = 0;
	result = fcntl(fd, F_ADD_SEALS, F_SEAL_EXEC);
	int sealErrno = errno;
	int dupped = dup(fd);
	int dupSeals = fcntl(dupped, F_GET_SEALS);
	close(dupped);
	pid_t child = fork();
	if (child == 0) {
		int childSeals = fcntl(fd, F_GET_SEALS);
		ssize_t childWrite = pwrite(fd, "c", 1, 0);
		_exit(childSeals == (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE
			| F_SEAL_SEAL) && childWrite == -1 ? 0 : 1);
	}
	int status = 0;
	waitpid(child, &status, 0);
	report("seal_seal_and_sharing", result == -1 && sealErrno == EPERM
			&& dupSeals == (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE
				| F_SEAL_SEAL)
			&& WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"add after F_SEAL_SEAL %d %s, dup seals %#x, fork child %d", result,
		strerror(sealErrno), dupSeals,
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	close(fd);

	// F_SEAL_FUTURE_WRITE: existing writable mappings keep working, no new
	// ones and no write().
	fd = sealable_memfd(4096);
	char* shared = (char*)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	result = fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE);
	shared[0] = 'f';
	char check = 0;
	pread(fd, &check, 1, 0);
	errno = 0;
	written = pwrite(fd, "w", 1, 1);
	writeErrno = errno;
	errno = 0;
	writable = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	mapErrno = errno;
	report("future_write", result == 0 && check == 'f' && written == -1
			&& writeErrno == EPERM && writable == MAP_FAILED
			&& mapErrno == EPERM,
		"add %d, old mapping wrote '%c', write %zd %s, new mapping %s",
		result, check ? check : '?', written, strerror(writeErrno),
		writable == MAP_FAILED ? strerror(mapErrno) : "succeeded");
	munmap(shared, 4096);
	close(fd);

	// F_SEAL_EXEC (MFD_NOEXEC_SEAL): the execute bits cannot be set.
	fd = memfd_create("noexec", MFD_NOEXEC_SEAL);
	seals = fcntl(fd, F_GET_SEALS);
	errno = 0;
	result = fchmod(fd, 0700);
	int chmodErrno = errno;
	int plain = fchmod(fd, 0640);
	report("noexec_seal", (seals & F_SEAL_EXEC) != 0
			&& (seals & F_SEAL_SEAL) == 0 && result == -1
			&& chmodErrno == EPERM && plain == 0,
		"seals %#x, chmod 0700 %d %s, chmod 0640 %d", seals, result,
		strerror(chmodErrno), plain);
	close(fd);

	// Errors: unknown seals, a read-only descriptor, non-memfd files.
	fd = sealable_memfd(4096);
	errno = 0;
	result = fcntl(fd, F_ADD_SEALS, 0x4000);
	int unknownErrno = errno;
	report("unknown_seal_einval", result == -1 && unknownErrno == EINVAL,
		"%d %s", result, strerror(unknownErrno));
	close(fd);

	int file = open("/boot/system/lib/libroot.so", O_RDONLY);
	errno = 0;
	int fileGet = fcntl(file, F_GET_SEALS);
	int fileGetErrno = errno;
	errno = 0;
	int fileAdd = fcntl(file, F_ADD_SEALS, F_SEAL_WRITE);
	int fileAddErrno = errno;
	close(file);
	int pipes[2];
	pipe(pipes);
	errno = 0;
	int pipeGet = fcntl(pipes[0], F_GET_SEALS);
	int pipeErrno = errno;
	close(pipes[0]);
	close(pipes[1]);
	report("non_memfd_einval", fileGet == -1 && fileGetErrno == EINVAL
			&& fileAdd == -1 && (fileAddErrno == EINVAL
				|| fileAddErrno == EPERM)
			&& pipeGet == -1 && pipeErrno == EINVAL,
		"file get %d %s, add %d %s; pipe get %d %s", fileGet,
		strerror(fileGetErrno), fileAdd, strerror(fileAddErrno), pipeGet,
		strerror(pipeErrno));

	// The private marking command is refused for a named file.
	char path[64];
	snprintf(path, sizeof(path), "/tmp/ce-memfd-seal-%d", (int)getpid());
	file = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	errno = 0;
	result = fcntl(file, 0x40000000, 0);
	int markErrno = errno;
	int namedGet = fcntl(file, F_GET_SEALS);
	close(file);
	unlink(path);
	report("named_file_not_sealable", result == -1 && namedGet == -1,
		"mark %d %s, F_GET_SEALS %d", result, strerror(markErrno), namedGet);
}


int
main()
{
	test_arguments();
	test_basic();
	test_cloexec();
	test_scm_rights();
	test_lifetime();
	test_memory_returned();
	test_threads();
	test_sealing();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
