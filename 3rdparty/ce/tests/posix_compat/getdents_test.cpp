/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for getdents64() and getdents() (libgnu): Linux record
	layouts, every entry exactly once across small buffers, d_type, errors.
	Output format: see 3rdparty/ce/README.md.
*/


#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <set>
#include <string>


#define SUITE "getdents"


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


static const char* kDirectory = "/tmp/ce-getdents";
static const int kFileCount = 150;


static void
remove_tree()
{
	char path[256];
	for (int i = 0; i < kFileCount; i++) {
		snprintf(path, sizeof(path), "%s/file-%03d-with-a-longer-name", kDirectory,
			i);
		unlink(path);
	}
	const char* others[] = { "subdir", "link", "fifo", "socket" };
	for (const char* other : others) {
		snprintf(path, sizeof(path), "%s/%s", kDirectory, other);
		if (unlink(path) != 0)
			rmdir(path);
	}
	rmdir(kDirectory);
}


static void
make_tree()
{
	remove_tree();
	mkdir(kDirectory, 0755);
	char path[256];
	for (int i = 0; i < kFileCount; i++) {
		snprintf(path, sizeof(path), "%s/file-%03d-with-a-longer-name", kDirectory,
			i);
		close(open(path, O_CREAT | O_WRONLY, 0644));
	}
	snprintf(path, sizeof(path), "%s/subdir", kDirectory);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/link", kDirectory);
	symlink("subdir", path);
	snprintf(path, sizeof(path), "%s/fifo", kDirectory);
	mkfifo(path, 0644);

	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s/socket",
		kDirectory);
	bind(sock, (struct sockaddr*)&address, sizeof(address));
	close(sock);
}


struct Collected {
	std::set<std::string>	names;
	int						duplicates;
	bool					layoutOK;
	char					layoutDetail[128];
	int						calls;
	unsigned char			typeOf[5];
		// subdir, link, fifo, socket, a regular file
};


static void
note_type(Collected& collected, const char* name, unsigned char type)
{
	if (strcmp(name, "subdir") == 0)
		collected.typeOf[0] = type;
	else if (strcmp(name, "link") == 0)
		collected.typeOf[1] = type;
	else if (strcmp(name, "fifo") == 0)
		collected.typeOf[2] = type;
	else if (strcmp(name, "socket") == 0)
		collected.typeOf[3] = type;
	else if (strncmp(name, "file-", 5) == 0)
		collected.typeOf[4] = type;
}


static bool
collect(bool large, size_t bufferSize, Collected& collected)
{
	collected.names.clear();
	collected.duplicates = 0;
	collected.layoutOK = true;
	collected.layoutDetail[0] = '\0';
	collected.calls = 0;
	memset(collected.typeOf, 0xff, sizeof(collected.typeOf));

	int fd = open(kDirectory, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		return false;

	char* buffer = (char*)malloc(bufferSize);
	while (true) {
		ssize_t bytes = large ? getdents64(fd, buffer, bufferSize)
			: getdents(fd, buffer, bufferSize);
		collected.calls++;
		if (bytes < 0) {
			snprintf(collected.layoutDetail, sizeof(collected.layoutDetail),
				"call %d failed: %s", collected.calls, strerror(errno));
			collected.layoutOK = false;
			break;
		}
		if (bytes == 0)
			break;

		for (ssize_t offset = 0; offset < bytes;) {
			const char* name;
			unsigned short reclen;
			unsigned char type;
			if (large) {
				const linux_dirent64* entry
					= (const linux_dirent64*)(buffer + offset);
				name = entry->d_name;
				reclen = entry->d_reclen;
				type = entry->d_type;
				if (entry->d_ino == 0 || entry->d_off == 0)
					collected.layoutOK = false;
			} else {
				const linux_dirent* entry
					= (const linux_dirent*)(buffer + offset);
				name = entry->d_name;
				reclen = entry->d_reclen;
				type = ((const char*)entry)[reclen - 1];
				if (entry->d_ino == 0 || entry->d_off == 0)
					collected.layoutOK = false;
			}
			size_t nameOffset = large ? offsetof(linux_dirent64, d_name)
				: offsetof(linux_dirent, d_name);
			if (reclen % 8 != 0 || reclen < nameOffset + strlen(name) + 1
				|| offset + reclen > bytes) {
				collected.layoutOK = false;
				snprintf(collected.layoutDetail,
					sizeof(collected.layoutDetail),
					"bad record \"%s\" reclen %u at %zd of %zd", name, reclen,
					offset, bytes);
			}
			if (!collected.names.insert(name).second)
				collected.duplicates++;
			note_type(collected, name, type);
			offset += reclen;
		}
	}
	free(buffer);
	close(fd);
	return true;
}


static void
check_listing(const char* name, bool large, size_t bufferSize)
{
	Collected collected;
	if (!collect(large, bufferSize, collected)) {
		report(name, false, "open: %s", strerror(errno));
		return;
	}
	// The files, subdir, link, fifo, socket, "." and "..".
	size_t expected = kFileCount + 4 + 2;
	report(name, collected.layoutOK && collected.duplicates == 0
			&& collected.names.size() == expected
			&& collected.names.count(".") == 1
			&& collected.names.count("..") == 1,
		"%zu names (want %zu), %d duplicates, %d calls, layout %s %s",
		collected.names.size(), expected, collected.duplicates,
		collected.calls, collected.layoutOK ? "ok" : "BAD",
		collected.layoutDetail);

	if (large && bufferSize >= 4096) {
		// DT_DIR 4, DT_LNK 10, DT_FIFO 1, DT_SOCK 12, DT_REG 8
		report("d_type", collected.typeOf[0] == 4 && collected.typeOf[1] == 10
				&& collected.typeOf[2] == 1 && collected.typeOf[3] == 12
				&& collected.typeOf[4] == 8,
			"dir %u link %u fifo %u socket %u file %u (want 4 10 1 12 8)",
			collected.typeOf[0], collected.typeOf[1], collected.typeOf[2],
			collected.typeOf[3], collected.typeOf[4]);
	}
	if (!large && bufferSize >= 4096) {
		report("d_type_getdents", collected.typeOf[0] == 4
				&& collected.typeOf[4] == 8,
			"dir %u file %u (want 4 8, last byte of the record)",
			collected.typeOf[0], collected.typeOf[4]);
	}
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	make_tree();

	report("layout", offsetof(linux_dirent64, d_off) == 8
			&& offsetof(linux_dirent64, d_reclen) == 16
			&& offsetof(linux_dirent64, d_type) == 18
			&& offsetof(linux_dirent64, d_name) == 19
			&& offsetof(linux_dirent, d_reclen) == 16
			&& offsetof(linux_dirent, d_name) == 18,
		"linux_dirent64 d_name at %zu, linux_dirent d_name at %zu",
		offsetof(linux_dirent64, d_name), offsetof(linux_dirent, d_name));

	check_listing("getdents64_large", true, 32768);
	check_listing("getdents64_small_buffer", true, 64);
	check_listing("getdents64_odd_buffer", true, 333);
	check_listing("getdents_large", false, 32768);
	check_listing("getdents_small_buffer", false, 64);

	// Errors as on Linux.
	int fd = open(kDirectory, O_RDONLY | O_DIRECTORY);
	char tiny[16];
	errno = 0;
	ssize_t result = getdents64(fd, tiny, sizeof(tiny));
	report("einval_tiny_buffer", result == -1 && errno == EINVAL,
		"result %zd, errno %s", result, strerror(errno));
	close(fd);

	char buffer[1024];
	errno = 0;
	result = getdents64(-1, buffer, sizeof(buffer));
	report("ebadf", result == -1 && errno == EBADF, "result %zd, errno %s",
		result, strerror(errno));

	char path[256];
	snprintf(path, sizeof(path), "%s/file-000-with-a-longer-name", kDirectory);
	fd = open(path, O_RDONLY);
	errno = 0;
	result = getdents64(fd, buffer, sizeof(buffer));
	report("enotdir", result == -1 && errno == ENOTDIR,
		"result %zd, errno %s", result, strerror(errno));
	close(fd);

	// End of directory stays at 0; an empty directory has only . and ..
	snprintf(path, sizeof(path), "%s/subdir", kDirectory);
	fd = open(path, O_RDONLY | O_DIRECTORY);
	ssize_t first = getdents64(fd, buffer, sizeof(buffer));
	int entries = 0;
	for (ssize_t offset = 0; offset < first;
			offset += ((linux_dirent64*)(buffer + offset))->d_reclen) {
		entries++;
	}
	ssize_t second = getdents64(fd, buffer, sizeof(buffer));
	ssize_t third = getdents64(fd, buffer, sizeof(buffer));
	report("empty_dir_eof", entries == 2 && second == 0 && third == 0,
		"%d entries, then %zd, %zd", entries, second, third);
	close(fd);

	remove_tree();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
