/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux x86-64 userspace ABI constants and structures used by the syscall
 * handlers. These are stable Linux UAPI numbers (the values a Linux program is
 * compiled against); they are written out here rather than derived from any
 * particular libc so the module has no Linux build dependency. Where FreeBSD's
 * Linuxulator defines the same constant (sys/compat/linux/linux.h,
 * linux_file.h, linux_mmap.h, linux_signal.h ...), the value was checked
 * against it.
 */
#ifndef _RENGUIN_LINUX_DEFS_H
#define _RENGUIN_LINUX_DEFS_H


#include <SupportDefs.h>


// Linux errno values returned directly by handlers. The full native->Linux
// map is in errno.cpp / abi/linux_errno_table.h.
#define LX_EPERM		1
#define LX_ENOENT		2
#define LX_ESRCH		3
#define LX_EINTR		4
#define LX_EIO			5
#define LX_E2BIG		7
#define LX_EBADF		9
#define LX_EAGAIN		11
#define LX_ENOMEM		12
#define LX_EACCES		13
#define LX_EFAULT		14
#define LX_EEXIST		17
#define LX_ENOTDIR		20
#define LX_EISDIR		21
#define LX_EINVAL		22
#define LX_ENFILE		23
#define LX_EMFILE		24
#define LX_ENOTTY		25
#define LX_ESPIPE		29
#define LX_ERANGE		34
#define LX_ENAMETOOLONG	36
#define LX_ENOSYS		38
#define LX_ELOOP		40
#define LX_EOPNOTSUPP	95
#define LX_ETIMEDOUT	110

// open()/openat() flags (Linux x86-64).
#define LX_O_RDONLY		00000000
#define LX_O_WRONLY		00000001
#define LX_O_RDWR		00000002
#define LX_O_ACCMODE	00000003
#define LX_O_CREAT		00000100
#define LX_O_EXCL		00000200
#define LX_O_NOCTTY		00000400
#define LX_O_TRUNC		00001000
#define LX_O_APPEND		00002000
#define LX_O_NONBLOCK	00004000
#define LX_O_DSYNC		00010000
#define LX_O_DIRECT		00040000
#define LX_O_LARGEFILE	00100000
#define LX_O_DIRECTORY	00200000
#define LX_O_NOFOLLOW	00400000
#define LX_O_NOATIME	01000000
#define LX_O_CLOEXEC	02000000
#define LX_O_SYNC		04010000
#define LX_O_PATH		010000000
#define LX_O_TMPFILE	020200000

// *at() special fd and flags.
#define LX_AT_FDCWD				-100
#define LX_AT_SYMLINK_NOFOLLOW	0x100
#define LX_AT_REMOVEDIR			0x200
#define LX_AT_EACCESS			0x200	// faccessat() only
#define LX_AT_SYMLINK_FOLLOW	0x400
#define LX_AT_NO_AUTOMOUNT		0x800
#define LX_AT_EMPTY_PATH		0x1000

// fcntl() commands and flags.
#define LX_F_DUPFD			0
#define LX_F_GETFD			1
#define LX_F_SETFD			2
#define LX_F_GETFL			3
#define LX_F_SETFL			4
#define LX_F_GETLK			5
#define LX_F_SETLK			6
#define LX_F_SETLKW			7
#define LX_F_DUPFD_CLOEXEC	1030
#define LX_F_SETOWN			8
#define LX_F_GETOWN			9
#define LX_F_SETSIG			10
#define LX_F_GETSIG			11
#define LX_F_OFD_GETLK		36
#define LX_F_OFD_SETLK		37
#define LX_F_OFD_SETLKW		38
#define LX_F_SETPIPE_SZ		1031
#define LX_F_GETPIPE_SZ		1032
#define LX_F_ADD_SEALS		1033
#define LX_F_GET_SEALS		1034
#define LX_FD_CLOEXEC		1

#define LX_F_RDLCK			0
#define LX_F_WRLCK			1
#define LX_F_UNLCK			2

// mmap()/mprotect() flags.
#define LX_PROT_READ	0x1
#define LX_PROT_WRITE	0x2
#define LX_PROT_EXEC	0x4
#define LX_PROT_GROWSDOWN	0x01000000
#define LX_PROT_GROWSUP		0x02000000

#define LX_MAP_SHARED			0x01
#define LX_MAP_PRIVATE			0x02
#define LX_MAP_SHARED_VALIDATE	0x03
#define LX_MAP_TYPE				0x0f
#define LX_MAP_FIXED			0x10
#define LX_MAP_ANONYMOUS		0x20
#define LX_MAP_32BIT			0x40
#define LX_MAP_GROWSDOWN		0x0100
#define LX_MAP_DENYWRITE		0x0800
#define LX_MAP_EXECUTABLE		0x1000
#define LX_MAP_LOCKED			0x2000
#define LX_MAP_NORESERVE		0x4000
#define LX_MAP_POPULATE			0x8000
#define LX_MAP_NONBLOCK			0x10000
#define LX_MAP_STACK			0x20000
#define LX_MAP_HUGETLB			0x40000
#define LX_MAP_FIXED_NOREPLACE	0x100000

#define LX_MREMAP_MAYMOVE	1
#define LX_MREMAP_FIXED		2

#define LX_MS_ASYNC			1
#define LX_MS_INVALIDATE	2
#define LX_MS_SYNC			4

#define LX_MADV_DONTNEED	4

// arch_prctl() subfunctions.
#define LX_ARCH_SET_GS	0x1001
#define LX_ARCH_SET_FS	0x1002
#define LX_ARCH_GET_FS	0x1003
#define LX_ARCH_GET_GS	0x1004

// prctl() options.
#define LX_PR_SET_NAME	15
#define LX_PR_GET_NAME	16

// clock ids.
#define LX_CLOCK_REALTIME			0
#define LX_CLOCK_MONOTONIC			1
#define LX_CLOCK_PROCESS_CPUTIME_ID	2
#define LX_CLOCK_THREAD_CPUTIME_ID	3
#define LX_CLOCK_MONOTONIC_RAW		4
#define LX_CLOCK_REALTIME_COARSE	5
#define LX_CLOCK_MONOTONIC_COARSE	6
#define LX_CLOCK_BOOTTIME			7

#define LX_TIMER_ABSTIME	1

// getrandom() flags.
#define LX_GRND_NONBLOCK	0x0001
#define LX_GRND_RANDOM		0x0002
#define LX_GRND_INSECURE	0x0004

// Resource limits (Linux order differs from Haiku's).
#define LX_RLIMIT_CPU		0
#define LX_RLIMIT_FSIZE		1
#define LX_RLIMIT_DATA		2
#define LX_RLIMIT_STACK		3
#define LX_RLIMIT_CORE		4
#define LX_RLIMIT_RSS		5
#define LX_RLIMIT_NPROC		6
#define LX_RLIMIT_NOFILE	7
#define LX_RLIMIT_MEMLOCK	8
#define LX_RLIMIT_AS		9
#define LX_RLIMIT_NLIMITS	16
#define LX_RLIM_INFINITY	(~(uint64)0)

// Signals. Linux numbering; translated to Haiku's in process.cpp.
#define LX_SIGHUP		1
#define LX_SIGINT		2
#define LX_SIGQUIT		3
#define LX_SIGILL		4
#define LX_SIGTRAP		5
#define LX_SIGABRT		6
#define LX_SIGBUS		7
#define LX_SIGFPE		8
#define LX_SIGKILL		9
#define LX_SIGUSR1		10
#define LX_SIGSEGV		11
#define LX_SIGUSR2		12
#define LX_SIGPIPE		13
#define LX_SIGALRM		14
#define LX_SIGTERM		15
#define LX_SIGSTKFLT	16
#define LX_SIGCHLD		17
#define LX_SIGCONT		18
#define LX_SIGSTOP		19
#define LX_SIGTSTP		20
#define LX_SIGTTIN		21
#define LX_SIGTTOU		22
#define LX_SIGURG		23
#define LX_SIGXCPU		24
#define LX_SIGXFSZ		25
#define LX_SIGVTALRM	26
#define LX_SIGPROF		27
#define LX_SIGWINCH		28
#define LX_SIGIO		29
#define LX_SIGPWR		30
#define LX_SIGSYS		31
#define LX_SIGRTMIN		32
#define LX_NSIG			64

// Terminal ioctls (asm-generic/ioctls.h).
#define LX_TCGETS		0x5401
#define LX_TCSETS		0x5402
#define LX_TCSETSW		0x5403
#define LX_TCSETSF		0x5404
#define LX_TIOCSCTTY	0x540E
#define LX_TIOCGPGRP	0x540F
#define LX_TIOCSPGRP	0x5410
#define LX_TIOCGWINSZ	0x5413
#define LX_TIOCSWINSZ	0x5414
#define LX_FIONREAD		0x541B
#define LX_FIONBIO		0x5421
#define LX_TIOCGSID		0x5429
#define LX_FIOCLEX		0x5451
#define LX_FIONCLEX		0x5450

// termios flags whose bit values differ between Linux and Haiku. The low
// iflag/oflag bits, and lflag bits up to TOSTOP, are identical.
#define LX_CBAUD		0010017
#define LX_CBAUDEX		0010000
#define LX_CSIZE		0000060
#define LX_CS5			0000000
#define LX_CS6			0000020
#define LX_CS7			0000040
#define LX_CS8			0000060
#define LX_CSTOPB		0000100
#define LX_CREAD		0000200
#define LX_PARENB		0000400
#define LX_PARODD		0001000
#define LX_HUPCL		0002000
#define LX_CLOCAL		0004000
#define LX_CRTSCTS		020000000000

#define LX_ECHOCTL		0001000
#define LX_ECHOPRT		0002000
#define LX_ECHOKE		0004000
#define LX_FLUSHO		0010000
#define LX_PENDIN		0040000
#define LX_IEXTEN		0100000

#define LX_ICANON		0000002

// c_cc indices.
#define LX_VINTR	0
#define LX_VQUIT	1
#define LX_VERASE	2
#define LX_VKILL	3
#define LX_VEOF		4
#define LX_VTIME	5
#define LX_VMIN		6
#define LX_VSWTC	7
#define LX_VSTART	8
#define LX_VSTOP	9
#define LX_VSUSP	10
#define LX_VEOL		11
#define LX_VREPRINT	12
#define LX_VDISCARD	13
#define LX_VWERASE	14
#define LX_VLNEXT	15
#define LX_VEOL2	16
#define LX_NCCS		19

// Linux d_type values for getdents64.
#define LX_DT_UNKNOWN	0
#define LX_DT_FIFO		1
#define LX_DT_CHR		2
#define LX_DT_DIR		4
#define LX_DT_BLK		6
#define LX_DT_REG		8
#define LX_DT_LNK		10
#define LX_DT_SOCK		12


// Linux x86-64 struct stat. Field order and padding differ from Haiku's, so
// fstat()/newfstatat() translate into this explicitly.
struct linux_stat {
	uint64	st_dev;
	uint64	st_ino;
	uint64	st_nlink;
	uint32	st_mode;
	uint32	st_uid;
	uint32	st_gid;
	uint32	__pad0;
	uint64	st_rdev;
	int64	st_size;
	int64	st_blksize;
	int64	st_blocks;
	int64	st_atime_sec;
	int64	st_atime_nsec;
	int64	st_mtime_sec;
	int64	st_mtime_nsec;
	int64	st_ctime_sec;
	int64	st_ctime_nsec;
	int64	__unused[3];
};

// Linux struct new_utsname: six fixed 65-byte fields.
struct linux_utsname {
	char	sysname[65];
	char	nodename[65];
	char	release[65];
	char	version[65];
	char	machine[65];
	char	domainname[65];
};

// Linux struct timespec / timeval (64-bit time_t on x86-64).
struct linux_timespec {
	int64	tv_sec;
	int64	tv_nsec;
};

struct linux_timeval {
	int64	tv_sec;
	int64	tv_usec;
};

// Linux struct pollfd (same layout as Haiku's; the event bits differ).
struct linux_pollfd {
	int32	fd;
	int16	events;
	int16	revents;
};

// Linux struct iovec.
struct linux_iovec {
	uint64	iov_base;
	uint64	iov_len;
};

// Linux struct rlimit / rlimit64 (identical on x86-64).
struct linux_rlimit {
	uint64	rlim_cur;
	uint64	rlim_max;
};

// Linux struct flock (x86-64).
struct linux_flock {
	int16	l_type;
	int16	l_whence;
	int32	__pad;
	int64	l_start;
	int64	l_len;
	int32	l_pid;
	int32	__pad2;
};

// Linux kernel struct termios (what TCGETS/TCSETS transfer; not glibc's).
struct linux_termios {
	uint32	c_iflag;
	uint32	c_oflag;
	uint32	c_cflag;
	uint32	c_lflag;
	uint8	c_line;
	uint8	c_cc[LX_NCCS];
};

// Linux struct linux_dirent64 (getdents64). d_name follows the header.
struct linux_dirent64 {
	uint64	d_ino;
	int64	d_off;
	uint16	d_reclen;
	uint8	d_type;
	char	d_name[];
};

// Linux kernel struct sigaction for rt_sigaction (x86-64).
struct linux_sigaction {
	uint64	handler;
	uint64	flags;
	uint64	restorer;
	uint64	mask;
};

// Linux stack_t for sigaltstack.
struct linux_stack {
	uint64	ss_sp;
	int32	ss_flags;
	int32	__pad;
	uint64	ss_size;
};


#endif	// _RENGUIN_LINUX_DEFS_H
