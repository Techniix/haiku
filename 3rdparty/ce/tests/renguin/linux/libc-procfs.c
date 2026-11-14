/*
 * /proc as command-line tools read it: the process's own stat, status,
 * cmdline, exe, fd/ and task/ entries, the system files (cpuinfo, meminfo,
 * uptime, loadavg, mounts, sys/kernel/ostype), directory listings,
 * relative lookups from a /proc directory descriptor, and the errors for
 * writing and for missing entries. Prints one line per part, then "ok".
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#define CHECK(cond, what) do { \
	if (!(cond)) { \
		printf("FAIL: %s (line %d, errno %d)\n", what, __LINE__, errno); \
		exit(1); \
	} \
} while (0)

#define PROC_SUPER_MAGIC 0x9fa0

static char content[65536];

//! Reads a whole file the way cat does (read() until 0).
static ssize_t read_file(int dirfd, const char* path)
{
	int fd = openat(dirfd, path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	size_t total = 0;
	for (;;) {
		ssize_t got = read(fd, content + total, sizeof(content) - 1 - total);
		if (got < 0) {
			close(fd);
			return -1;
		}
		if (got == 0)
			break;
		total += got;
	}
	close(fd);
	content[total] = '\0';
	return total;
}

static int dir_has(const char* path, const char* name)
{
	DIR* dir = opendir(path);
	if (dir == NULL)
		return -1;
	int found = 0;
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, name) == 0)
			found = 1;
	}
	closedir(dir);
	return found;
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	char number[32];
	pid_t pid = getpid();

	// /proc/self/stat: pid, (comm), state, ppid
	CHECK(read_file(AT_FDCWD, "/proc/self/stat") > 0, "read stat");
	int statPid = 0, statPpid = 0;
	char comm[64] = "", state = 0;
	CHECK(sscanf(content, "%d (%63[^)]) %c %d", &statPid, comm, &state,
		&statPpid) == 4, "parse stat");
	CHECK(statPid == pid && statPpid == getppid()
		&& strcmp(comm, "libc-procfs") == 0, "stat fields");
	// field 22 (starttime) and 23 (vsize) exist
	int fields = 0;
	for (char* p = strrchr(content, ')'); p != NULL && *p != '\0'; p++) {
		if (*p == ' ')
			fields++;
	}
	CHECK(fields >= 50, "stat field count");
	printf("stat\n");

	// status
	CHECK(read_file(AT_FDCWD, "/proc/self/status") > 0, "read status");
	snprintf(number, sizeof(number), "\nPid:\t%d\n", pid);
	CHECK(strncmp(content, "Name:\tlibc-procfs\n", 18) == 0
		&& strstr(content, number) != NULL
		&& strstr(content, "\nVmRSS:") != NULL, "status fields");
	printf("status\n");

	// cmdline: the arguments, NUL-separated
	ssize_t length = read_file(AT_FDCWD, "/proc/self/cmdline");
	size_t want = 0;
	for (int i = 0; i < argc; i++)
		want += strlen(argv[i]) + 1;
	CHECK(length == (ssize_t)want && strcmp(content, argv[0]) == 0
		&& (argc < 2 || strcmp(content + strlen(argv[0]) + 1, argv[1]) == 0),
		"cmdline");
	CHECK(read_file(AT_FDCWD, "/proc/self/environ") >= 0, "environ");
	printf("cmdline\n");

	// exe and the self link
	char link[256];
	length = readlink("/proc/self/exe", link, sizeof(link) - 1);
	CHECK(length > 12, "readlink exe");
	link[length] = '\0';
	CHECK(strcmp(link + length - 12, "/libc-procfs") == 0, "exe name");
	length = readlink("/proc/self", link, sizeof(link) - 1);
	CHECK(length > 0, "readlink self");
	link[length] = '\0';
	snprintf(number, sizeof(number), "%d", pid);
	CHECK(strcmp(link, number) == 0, "self target");
	struct stat st;
	CHECK(lstat("/proc/self", &st) == 0 && S_ISLNK(st.st_mode), "lstat self");
	CHECK(stat("/proc/self", &st) == 0 && S_ISDIR(st.st_mode), "stat self");
	CHECK(stat("/proc/self/exe", &st) == 0 && S_ISREG(st.st_mode),
		"stat exe");
	printf("exe\n");

	// fd/: the open descriptors (a high number: opendir() takes the
	// lowest free one)
	int fd = open("/proc/self/status", O_RDONLY);
	CHECK(fd >= 0 && dup2(fd, 50) == 50, "open status");
	close(fd);
	CHECK(dir_has("/proc/self/fd", "0") == 1
		&& dir_has("/proc/self/fd", "50") == 1, "fd list");
	CHECK(readlink("/proc/self/fd/50", link, sizeof(link)) > 0,
		"readlink fd");
	close(50);
	CHECK(dir_has("/proc/self/fd", "50") == 0, "fd closed");
	printf("fd\n");

	// /proc lists us, self and the system files; task/ lists our thread
	snprintf(number, sizeof(number), "%d", pid);
	CHECK(dir_has("/proc", number) == 1 && dir_has("/proc", "self") == 1
		&& dir_has("/proc", "cpuinfo") == 1, "proc list");
	snprintf(number, sizeof(number), "%ld", (long)syscall(SYS_gettid));
	CHECK(dir_has("/proc/self/task", number) == 1, "task list");
	printf("listing\n");

	// system files
	CHECK(read_file(AT_FDCWD, "/proc/cpuinfo") > 0
		&& strncmp(content, "processor\t: 0\n", 14) == 0
		&& strstr(content, "\nflags\t\t: ") != NULL, "cpuinfo");
	unsigned long memTotal = 0;
	CHECK(read_file(AT_FDCWD, "/proc/meminfo") > 0
		&& sscanf(content, "MemTotal: %lu kB", &memTotal) == 1
		&& memTotal > 0, "meminfo");
	double uptime = 0, idle = 0;
	CHECK(read_file(AT_FDCWD, "/proc/uptime") > 0
		&& sscanf(content, "%lf %lf", &uptime, &idle) == 2 && uptime > 0,
		"uptime");
	double load[3];
	int running = 0, total = 0, last = 0;
	CHECK(read_file(AT_FDCWD, "/proc/loadavg") > 0
		&& sscanf(content, "%lf %lf %lf %d/%d %d", &load[0], &load[1],
			&load[2], &running, &total, &last) == 6, "loadavg");
	char device[128], mountPoint[128], type[64];
	CHECK(read_file(AT_FDCWD, "/proc/mounts") > 0
		&& sscanf(content, "%127s %127s %63s", device, mountPoint, type) == 3,
		"mounts");
	CHECK(read_file(AT_FDCWD, "/proc/sys/kernel/ostype") == 6
		&& strcmp(content, "Linux\n") == 0, "ostype");
	CHECK(read_file(AT_FDCWD, "/proc/self/maps") > 0
		&& strchr(content, '-') != NULL, "maps");
	printf("system\n");

	// relative lookups from a /proc directory descriptor
	int dirfd = open("/proc/self", O_RDONLY | O_DIRECTORY);
	CHECK(dirfd >= 0, "open dir");
	CHECK(fstat(dirfd, &st) == 0 && S_ISDIR(st.st_mode), "fstat dir");
	CHECK(read_file(dirfd, "status") > 0
		&& strncmp(content, "Name:", 5) == 0, "openat relative");
	CHECK(fstatat(dirfd, "stat", &st, 0) == 0 && S_ISREG(st.st_mode),
		"fstatat relative");
	CHECK(readlinkat(dirfd, "exe", link, sizeof(link)) > 0,
		"readlinkat relative");
	struct statfs sf;
	CHECK(fstatfs(dirfd, &sf) == 0 && sf.f_type == PROC_SUPER_MAGIC,
		"fstatfs");
	errno = 0;
	CHECK(fstatat(dirfd, "", &st, 0) == -1 && errno == ENOENT, "empty path");
	close(dirfd);
	CHECK(statfs("/proc", &sf) == 0 && sf.f_type == PROC_SUPER_MAGIC,
		"statfs");
	printf("relative\n");

	// A trailing slash looks through the last link
	CHECK(lstat("/proc/self/", &st) == 0 && S_ISDIR(st.st_mode),
		"lstat self/");
	errno = 0;
	CHECK(readlink("/proc/self/", link, sizeof(link)) == -1 && errno == EINVAL,
		"readlink self/");
	int nofollow = open("/proc/self/", O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
	CHECK(nofollow >= 0, "open self/ nofollow");
	close(nofollow);
	errno = 0;
	CHECK(open("/proc/cpuinfo/", O_RDONLY) == -1
		&& (errno == ENOTDIR || errno == ENOENT), "file with slash");
	// A link into /proc, like /etc/mtab -> ../proc/self/mounts. Renguin
	// finds such links through its Linux root; skip this without one.
	struct utsname name;
	int haveRoot = uname(&name) != 0 || strstr(name.release, "renguin") == NULL
		|| access("/etc/alpine-release", F_OK) == 0
		|| access("/etc/debian_version", F_OK) == 0;
	if (haveRoot) {
		char linkPath[64];
		snprintf(linkPath, sizeof(linkPath), "/tmp/renguin-mtab-%d", (int)pid);
		CHECK(symlink("/proc/self/mounts", linkPath) == 0, "symlink");
		ssize_t got = read_file(AT_FDCWD, linkPath);
		unlink(linkPath);
		CHECK(got > 0 && strchr(content, ' ') != NULL, "read through link");
	}
	printf("links\n");

	// errors
	// Linux lets root open the file for writing (the write then fails);
	// Renguin refuses the open. Either way nothing is written.
	errno = 0;
	int writeFD = open("/proc/self/stat", O_WRONLY);
	CHECK(writeFD == -1 ? errno == EACCES
		: (write(writeFD, "x", 1) == -1 && close(writeFD) == 0),
		"write refused");
	errno = 0;
	CHECK(open("/proc/self/no-such-entry", O_RDONLY) == -1 && errno == ENOENT,
		"ENOENT");
	errno = 0;
	CHECK(open("/proc/999999999/stat", O_RDONLY) == -1 && errno == ENOENT,
		"no such process");
	CHECK(access("/proc/self/status", R_OK) == 0, "access");
	errno = 0;
	CHECK(geteuid() == 0 || (access("/proc/self/status", W_OK) == -1
		&& errno == EACCES), "access W_OK");
	printf("errors\n");

	printf("ok\n");
	return 0;
}
