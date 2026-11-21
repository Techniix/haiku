/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux extended attributes on Haiku's file attributes under Renguin
 * (static glibc program). One line per part that worked, then "ok".
 *
 *  set_get    setxattr/getxattr of user.renguin.test, the size query
 *             (size 0), ERANGE for a short buffer
 *  list       listxattr names it (with the size query)
 *  flags      XATTR_CREATE on an existing one is EEXIST, XATTR_REPLACE on
 *             a missing one ENODATA
 *  fd         fsetxattr/fgetxattr/flistxattr on a descriptor
 *  remove     removexattr, then getxattr is ENODATA
 *  errors     a missing file is ENOENT, security.* is EOPNOTSUPP under
 *             Renguin (ENODATA on Linux), a bare "user." is EINVAL
 *  native     leaves /tmp/renguin-xattr-native with user.BEOS:TYPE and
 *             user.renguin.note for renguin_test's native check
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>
#include <unistd.h>


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAIL %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf(" (errno %d %s)\n", errno, strerror(errno)); \
			exit(1); \
		} \
	} while (0)


static const char* kFile = "/tmp/renguin-xattr-test";


static int
list_has(const char* list, ssize_t length, const char* name)
{
	for (ssize_t i = 0; i < length; i += strlen(list + i) + 1) {
		if (strcmp(list + i, name) == 0)
			return 1;
	}
	return 0;
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	unlink(kFile);
	int fd = open(kFile, O_CREAT | O_RDWR | O_TRUNC, 0644);
	CHECK(fd >= 0, "create %s", kFile);

	CHECK(setxattr(kFile, "user.renguin.test", "value-1", 7, 0) == 0,
		"setxattr");
	CHECK(getxattr(kFile, "user.renguin.test", NULL, 0) == 7, "size query");
	char buffer[64];
	memset(buffer, 0, sizeof(buffer));
	CHECK(getxattr(kFile, "user.renguin.test", buffer, sizeof(buffer)) == 7
		&& memcmp(buffer, "value-1", 7) == 0, "getxattr \"%s\"", buffer);
	errno = 0;
	CHECK(getxattr(kFile, "user.renguin.test", buffer, 3) == -1
		&& errno == ERANGE, "short buffer");
	// a shorter value replaces the whole old one
	CHECK(setxattr(kFile, "user.renguin.test", "v2", 2, 0) == 0, "set again");
	CHECK(getxattr(kFile, "user.renguin.test", buffer, sizeof(buffer)) == 2
		&& memcmp(buffer, "v2", 2) == 0, "replaced value");
	printf("set_get\n");

	ssize_t size = listxattr(kFile, NULL, 0);
	CHECK(size > 0, "list size query");
	char list[4096];
	ssize_t length = listxattr(kFile, list, sizeof(list));
	CHECK(length == size && list_has(list, length, "user.renguin.test"),
		"listxattr %zd (query %zd)", length, size);
	printf("list\n");

	errno = 0;
	CHECK(setxattr(kFile, "user.renguin.test", "x", 1, XATTR_CREATE) == -1
		&& errno == EEXIST, "XATTR_CREATE");
	errno = 0;
	CHECK(setxattr(kFile, "user.renguin.none", "x", 1, XATTR_REPLACE) == -1
		&& errno == ENODATA, "XATTR_REPLACE");
	printf("flags\n");

	CHECK(fsetxattr(fd, "user.renguin.fd", "fd-value", 8, 0) == 0,
		"fsetxattr");
	CHECK(fgetxattr(fd, "user.renguin.fd", buffer, sizeof(buffer)) == 8
		&& memcmp(buffer, "fd-value", 8) == 0, "fgetxattr");
	length = flistxattr(fd, list, sizeof(list));
	CHECK(length > 0 && list_has(list, length, "user.renguin.fd"),
		"flistxattr");
	printf("fd\n");

	CHECK(removexattr(kFile, "user.renguin.test") == 0, "removexattr");
	errno = 0;
	CHECK(getxattr(kFile, "user.renguin.test", buffer, sizeof(buffer)) == -1
		&& errno == ENODATA, "removed attribute");
	errno = 0;
	CHECK(removexattr(kFile, "user.renguin.test") == -1 && errno == ENODATA,
		"removing twice");
	printf("remove\n");

	errno = 0;
	CHECK(getxattr("/tmp/renguin-xattr-missing", "user.x", buffer,
		sizeof(buffer)) == -1 && errno == ENOENT, "missing file");
	errno = 0;
	CHECK(getxattr(kFile, "security.selinux", buffer, sizeof(buffer)) == -1
		&& (errno == EOPNOTSUPP || errno == ENODATA), "security.*");
	errno = 0;
	CHECK(setxattr(kFile, "user.", "x", 1, 0) == -1 && errno == EINVAL,
		"bare user.");
	printf("errors\n");
	close(fd);
	unlink(kFile);

	const char* native = "/tmp/renguin-xattr-native";
	unlink(native);
	fd = open(native, O_CREAT | O_RDWR | O_TRUNC, 0644);
	CHECK(fd >= 0, "create %s", native);
	close(fd);
	CHECK(setxattr(native, "user.BEOS:TYPE", "text/x-renguin",
		sizeof("text/x-renguin"), 0) == 0, "user.BEOS:TYPE");
	CHECK(setxattr(native, "user.renguin.note", "from Linux", 10, 0) == 0,
		"user.renguin.note");
	printf("native\n");
	printf("ok\n");
	return 0;
}
