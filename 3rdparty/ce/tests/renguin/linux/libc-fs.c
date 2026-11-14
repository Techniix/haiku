/*
 * File system and descriptor calls that terminal programs use, through the
 * C library: statx, statfs, chmod, chown, utimensat, symlink/readlink,
 * link, mkfifo, truncate, sendfile, flock, fadvise, poll, select, ppoll with
 * a signal mask, and the system information calls (sysinfo, times,
 * getrusage, getpriority, sched_*, getgroups). Prints one line per part,
 * then "ok".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond, what) do { \
	if (!(cond)) { \
		printf("FAIL: %s (line %d, errno %d)\n", what, __LINE__, errno); \
		cleanup(); \
		exit(1); \
	} \
} while (0)

static char dir[64];
static char pathA[96], pathB[96], pathL[96], pathF[96], pathH[96];

static void cleanup(void)
{
	unlink(pathA); unlink(pathB); unlink(pathL); unlink(pathF); unlink(pathH);
	rmdir(dir);
}

static volatile sig_atomic_t usr1Count;
static void usr1_handler(int sig) { (void)sig; usr1Count++; }

static pthread_t mainThread;
static void* send_usr1(void* arg)
{
	(void)arg;
	usleep(30000);
	pthread_kill(mainThread, SIGUSR1);
	return NULL;
}

static void write_file(const char* path, const char* text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0, "create");
	CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text), "write");
	close(fd);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	strcpy(dir, "/tmp/renguin-fsXXXXXX");
	CHECK(mkdtemp(dir) != NULL, "mkdtemp");
	snprintf(pathA, sizeof(pathA), "%s/a", dir);
	snprintf(pathB, sizeof(pathB), "%s/b", dir);
	snprintf(pathL, sizeof(pathL), "%s/link", dir);
	snprintf(pathF, sizeof(pathF), "%s/fifo", dir);
	snprintf(pathH, sizeof(pathH), "%s/hard", dir);
	write_file(pathA, "hello, renguin\n");

	// statx agrees with stat
	struct stat st;
	CHECK(stat(pathA, &st) == 0, "stat");
	struct statx sx;
	CHECK(statx(AT_FDCWD, pathA, 0, STATX_BASIC_STATS, &sx) == 0, "statx");
	CHECK(sx.stx_ino == st.st_ino && sx.stx_size == 15
		&& (sx.stx_mode & S_IFMT) == S_IFREG
		&& makedev(sx.stx_dev_major, sx.stx_dev_minor) == st.st_dev
		&& sx.stx_mtime.tv_sec == st.st_mtim.tv_sec, "statx fields");
	printf("statx\n");

	// statfs
	struct statfs sf;
	CHECK(statfs(dir, &sf) == 0 && sf.f_bsize > 0 && sf.f_blocks > 0,
		"statfs");
	int fd = open(pathA, O_RDONLY);
	CHECK(fd >= 0, "open");
	struct statfs sf2;
	CHECK(fstatfs(fd, &sf2) == 0 && sf2.f_type == sf.f_type, "fstatfs");
	close(fd);
	printf("statfs\n");

	// chmod, fchmod, chown to ourselves
	CHECK(chmod(pathA, 0640) == 0 && stat(pathA, &st) == 0
		&& (st.st_mode & 07777) == 0640, "chmod");
	fd = open(pathA, O_RDONLY);
	CHECK(fd >= 0 && fchmod(fd, 0604) == 0 && fstat(fd, &st) == 0
		&& (st.st_mode & 07777) == 0604, "fchmod");
	CHECK(fchown(fd, getuid(), getgid()) == 0, "fchown");
	close(fd);
	CHECK(chown(pathA, -1, -1) == 0 && lchown(pathA, getuid(), -1) == 0,
		"chown");
	CHECK(fchmodat(AT_FDCWD, pathA, 0644, 0) == 0, "fchmodat");
	errno = 0;
	CHECK(chmod("/nonexistent-renguin/x", 0644) == -1 && errno == ENOENT,
		"chmod ENOENT");
	printf("chmod\n");

	// utimensat: explicit times, UTIME_OMIT, futimens. The access time is
	// either the one set (a file system that stores it) or, as on a Linux
	// volume mounted with noatime, the modification time: Renguin shows
	// files on BFS, which stores no access time, that way (renguin's
	// file.cpp, renguin_linux_atime()). What it must never be is "now",
	// which BFS itself reports.
	struct timespec stamps[2] = { { 1000000000, 0 }, { 1234567890, 0 } };
	CHECK(utimensat(AT_FDCWD, pathA, stamps, 0) == 0 && stat(pathA, &st) == 0
		&& st.st_mtim.tv_sec == 1234567890
		&& (st.st_atim.tv_sec == 1000000000
			|| st.st_atim.tv_sec == st.st_mtim.tv_sec), "utimensat");
	stamps[0].tv_nsec = UTIME_OMIT;
	stamps[1].tv_sec = 1300000000;
	CHECK(utimensat(AT_FDCWD, pathA, stamps, 0) == 0 && stat(pathA, &st) == 0
		&& st.st_mtim.tv_sec == 1300000000
		&& (st.st_atim.tv_sec == 1000000000
			|| st.st_atim.tv_sec == st.st_mtim.tv_sec), "UTIME_OMIT");
	struct statx stx;
	CHECK(statx(AT_FDCWD, pathA, 0, STATX_ATIME | STATX_MTIME, &stx) == 0
		&& (stx.stx_atime.tv_sec == 1000000000
			|| stx.stx_atime.tv_sec == stx.stx_mtime.tv_sec),
		"statx atime");
	fd = open(pathA, O_RDONLY);
	CHECK(fd >= 0 && futimens(fd, NULL) == 0 && fstat(fd, &st) == 0
		&& st.st_mtim.tv_sec > 1600000000, "futimens");
	close(fd);
	struct timespec bad[2] = { { 0, 2000000000 }, { 0, 0 } };
	errno = 0;
	CHECK(utimensat(AT_FDCWD, pathA, bad, 0) == -1 && errno == EINVAL,
		"utimensat EINVAL");
	printf("utimensat\n");

	// symlink, readlink, lstat; link (hard links are optional: BFS has
	// none)
	CHECK(symlink("a", pathL) == 0, "symlink");
	char target[64];
	ssize_t length = readlink(pathL, target, sizeof(target));
	CHECK(length == 1 && target[0] == 'a', "readlink");
	CHECK(lstat(pathL, &st) == 0 && S_ISLNK(st.st_mode), "lstat");
	CHECK(stat(pathL, &st) == 0 && S_ISREG(st.st_mode), "stat via link");
	if (link(pathA, pathH) == 0) {
		CHECK(stat(pathA, &st) == 0 && st.st_nlink == 2, "nlink");
	} else {
		CHECK(errno == EPERM || errno == EOPNOTSUPP || errno == EXDEV,
			"link error");
	}
	printf("links\n");

	// mkfifo
	CHECK(mkfifo(pathF, 0600) == 0 && stat(pathF, &st) == 0
		&& S_ISFIFO(st.st_mode), "mkfifo");
	int fifoRead = open(pathF, O_RDONLY | O_NONBLOCK);
	int fifoWrite = open(pathF, O_WRONLY | O_NONBLOCK);
	CHECK(fifoRead >= 0 && fifoWrite >= 0, "open fifo");
	CHECK(write(fifoWrite, "x", 1) == 1, "write fifo");
	char c = 0;
	CHECK(read(fifoRead, &c, 1) == 1 && c == 'x', "read fifo");
	close(fifoRead);
	close(fifoWrite);
	errno = 0;
	CHECK((mknod(pathB, S_IFCHR | 0600, makedev(1, 3)) == -1 && errno == EPERM)
		|| (getuid() == 0 && unlink(pathB) == 0), "mknod chr");
	printf("mkfifo\n");

	// truncate, creat
	CHECK(truncate(pathA, 5) == 0 && stat(pathA, &st) == 0
		&& st.st_size == 5, "truncate");
	fd = creat(pathB, 0644);
	CHECK(fd >= 0 && write(fd, "zz", 2) == 2, "creat");
	close(fd);
	printf("truncate\n");

	// sendfile with and without an offset
	int in = open(pathA, O_RDONLY);
	int out = open(pathB, O_WRONLY | O_TRUNC);
	CHECK(in >= 0 && out >= 0, "open for sendfile");
	off_t offset = 1;
	CHECK(sendfile(out, in, &offset, 100) == 4 && offset == 5
		&& lseek(in, 0, SEEK_CUR) == 0, "sendfile offset");
	CHECK(sendfile(out, in, NULL, 2) == 2 && lseek(in, 0, SEEK_CUR) == 2,
		"sendfile position");
	close(in);
	close(out);
	char buffer[16] = "";
	fd = open(pathB, O_RDONLY);
	CHECK(fd >= 0 && read(fd, buffer, sizeof(buffer)) == 6
		&& memcmp(buffer, "ello", 4) == 0 && memcmp(buffer + 4, "he", 2) == 0,
		"sendfile data");
	close(fd);
	printf("sendfile\n");

	// flock: a second open file description conflicts
	int lock1 = open(pathA, O_RDONLY);
	int lock2 = open(pathA, O_RDONLY);
	CHECK(lock1 >= 0 && lock2 >= 0, "open for flock");
	CHECK(flock(lock1, LOCK_EX | LOCK_NB) == 0, "flock");
	errno = 0;
	CHECK(flock(lock2, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK,
		"flock conflict");
	CHECK(flock(lock1, LOCK_UN) == 0 && flock(lock2, LOCK_SH | LOCK_NB) == 0,
		"flock unlock");
	close(lock1);
	close(lock2);
	printf("flock\n");

	// fadvise: fine on a file, ESPIPE on a pipe
	int pipes[2];
	CHECK(pipe(pipes) == 0, "pipe");
	fd = open(pathA, O_RDONLY);
	CHECK(posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) == 0, "fadvise");
	CHECK(posix_fadvise(pipes[0], 0, 0, POSIX_FADV_NORMAL) == ESPIPE,
		"fadvise ESPIPE");
	close(fd);
	printf("fadvise\n");

	// poll: timeout, readable, writable, invalid descriptor, hangup
	struct pollfd pfd[3] = {
		{ pipes[0], POLLIN, 0 }, { pipes[1], POLLOUT, 0 }, { 999, POLLIN, 0 }
	};
	CHECK(poll(pfd, 1, 20) == 0 && pfd[0].revents == 0, "poll timeout");
	CHECK(poll(pfd + 1, 1, 0) == 1 && pfd[1].revents == POLLOUT,
		"poll writable");
	CHECK(write(pipes[1], "y", 1) == 1, "write pipe");
	CHECK(poll(pfd, 3, 1000) == 3 && pfd[0].revents == POLLIN
		&& pfd[2].revents == POLLNVAL, "poll readable");
	CHECK(read(pipes[0], &c, 1) == 1, "read pipe");
	close(pipes[1]);
	pfd[0].revents = 0;
	CHECK(poll(pfd, 1, 1000) == 1 && (pfd[0].revents & POLLHUP) != 0
		&& read(pipes[0], &c, 1) == 0, "poll hangup");
	close(pipes[0]);
	printf("poll\n");

	// select: timeout written back, readable set
	CHECK(pipe(pipes) == 0, "pipe 2");
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(pipes[0], &readSet);
	struct timeval timeout = { 0, 20000 };
	CHECK(select(pipes[0] + 1, &readSet, NULL, NULL, &timeout) == 0
		&& !FD_ISSET(pipes[0], &readSet) && timeout.tv_sec == 0
		&& timeout.tv_usec == 0, "select timeout");
	CHECK(write(pipes[1], "z", 1) == 1, "write pipe 2");
	FD_SET(pipes[0], &readSet);
	fd_set writeSet;
	FD_ZERO(&writeSet);
	FD_SET(pipes[1], &writeSet);
	timeout.tv_sec = 1;
	CHECK(select(pipes[1] + 1, &readSet, &writeSet, NULL, &timeout) == 2
		&& FD_ISSET(pipes[0], &readSet) && FD_ISSET(pipes[1], &writeSet),
		"select ready");
	int closedFD = dup(0);
	CHECK(closedFD >= 0 && close(closedFD) == 0, "closed fd");
	errno = 0;
	FD_ZERO(&readSet);
	FD_SET(closedFD, &readSet);
	timeout.tv_sec = 0;
	CHECK(select(closedFD + 1, &readSet, NULL, NULL, &timeout) == -1
		&& errno == EBADF, "select EBADF");
	printf("select\n");

	// ppoll with a mask that lets a blocked signal through: the handler
	// runs, ppoll fails with EINTR, and the signal is blocked again after.
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = usr1_handler;
	CHECK(sigaction(SIGUSR1, &action, NULL) == 0, "sigaction");
	sigset_t blocked, waitMask, current;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	CHECK(sigprocmask(SIG_BLOCK, &blocked, NULL) == 0, "block");
	CHECK(sigprocmask(SIG_BLOCK, NULL, &waitMask) == 0, "get mask");
	sigdelset(&waitMask, SIGUSR1);
	CHECK(read(pipes[0], &c, 1) == 1, "drain");
	mainThread = pthread_self();
	pthread_t sender;
	CHECK(pthread_create(&sender, NULL, send_usr1, NULL) == 0, "thread");
	struct pollfd one = { pipes[0], POLLIN, 0 };
	struct timespec ppollTimeout = { 5, 0 };
	errno = 0;
	int result = ppoll(&one, 1, &ppollTimeout, &waitMask);
	CHECK(result == -1 && errno == EINTR && usr1Count == 1, "ppoll EINTR");
	CHECK(sigprocmask(SIG_BLOCK, NULL, &current) == 0
		&& sigismember(&current, SIGUSR1), "mask restored");
	pthread_join(sender, NULL);
	// pselect the same way, with the signal already pending
	CHECK(pthread_kill(pthread_self(), SIGUSR1) == 0 && usr1Count == 1,
		"pending");
	FD_ZERO(&readSet);
	FD_SET(pipes[0], &readSet);
	struct timespec pselectTimeout = { 5, 0 };
	errno = 0;
	result = pselect(pipes[0] + 1, &readSet, NULL, NULL, &pselectTimeout,
		&waitMask);
	CHECK(result == -1 && errno == EINTR && usr1Count == 2, "pselect EINTR");
	CHECK(sigprocmask(SIG_BLOCK, NULL, &current) == 0
		&& sigismember(&current, SIGUSR1), "mask restored 2");
	close(pipes[0]);
	close(pipes[1]);
	printf("ppoll\n");

	// system information
	struct sysinfo info;
	CHECK(sysinfo(&info) == 0 && info.totalram > 0 && info.mem_unit >= 1
		&& info.freeram <= info.totalram, "sysinfo");
	struct tms tms;
	CHECK(times(&tms) != (clock_t)-1, "times");
	struct rusage usage;
	CHECK(getrusage(RUSAGE_SELF, &usage) == 0
		&& getrusage(RUSAGE_CHILDREN, &usage) == 0, "getrusage");
	errno = 0;
	CHECK(getpriority(PRIO_PROCESS, 0) == 0 && errno == 0, "getpriority");
	CHECK(setpriority(PRIO_PROCESS, 0, 0) == 0, "setpriority");
	CHECK(sched_getscheduler(0) == SCHED_OTHER
		&& sched_get_priority_max(SCHED_FIFO) == 99
		&& sched_get_priority_min(SCHED_OTHER) == 0, "sched");
	struct sched_param param;
	CHECK(sched_getparam(0, &param) == 0 && param.sched_priority == 0,
		"sched_getparam");
	int groups = getgroups(0, NULL);
	CHECK(groups >= 0, "getgroups");
	printf("sysinfo\n");

	cleanup();
	printf("ok\n");
	return 0;
}
