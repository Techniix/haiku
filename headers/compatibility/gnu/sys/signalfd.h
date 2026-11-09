/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * The Linux signalfd interface: a file descriptor from which the calling
 * thread reads its pending signals (of a given set) as struct
 * signalfd_siginfo records, instead of having handlers run. The signals
 * have to be blocked with sigprocmask()/pthread_sigmask(). The descriptor
 * works with poll(), select() and epoll. Names, flags and the 128-byte
 * record layout are those of Linux; signal numbers are Haiku's. Link with
 * -lgnu.
 *
 * Differences from Linux (see 3rdparty/ce/PORTING.md, "signalfd"):
 * ssi_utime, ssi_stime, ssi_trapno, ssi_fd, ssi_tid, ssi_overrun,
 * ssi_addr_lsb, ssi_syscall, ssi_call_addr and ssi_arch are 0.
 * As elsewhere on Haiku, SIGCHLD interrupts blocking calls with EINTR even
 * while it is blocked; retry, the signal stays pending for the signalfd.
 */
#ifndef _GNU_SYS_SIGNALFD_H
#define _GNU_SYS_SIGNALFD_H


#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/cdefs.h>


/* flags for signalfd() */
#define SFD_CLOEXEC		O_CLOEXEC
#define SFD_NONBLOCK	O_NONBLOCK
#define SFD_CLOFORK		O_CLOFORK	/* Haiku extension */


struct signalfd_siginfo {
	uint32_t	ssi_signo;		/* signal number */
	int32_t		ssi_errno;		/* error number (unused) */
	int32_t		ssi_code;		/* signal code (si_code) */
	uint32_t	ssi_pid;		/* sending process */
	uint32_t	ssi_uid;		/* real user ID of the sender */
	int32_t		ssi_fd;			/* (0) */
	uint32_t	ssi_tid;		/* (0) */
	uint32_t	ssi_band;		/* band event (SIGPOLL) */
	uint32_t	ssi_overrun;	/* (0) */
	uint32_t	ssi_trapno;		/* (0) */
	int32_t		ssi_status;		/* exit status or signal (SIGCHLD) */
	int32_t		ssi_int;		/* sigqueue() value, as int */
	uint64_t	ssi_ptr;		/* sigqueue() value, as pointer */
	uint64_t	ssi_utime;		/* (0) */
	uint64_t	ssi_stime;		/* (0) */
	uint64_t	ssi_addr;		/* faulting address */
	uint16_t	ssi_addr_lsb;	/* (0) */
	uint16_t	__pad2;
	int32_t		ssi_syscall;	/* (0) */
	uint64_t	ssi_call_addr;	/* (0) */
	uint32_t	ssi_arch;		/* (0) */
	uint8_t		__pad[28];
};


__BEGIN_DECLS


int		signalfd(int fd, const sigset_t* mask, int flags);


__END_DECLS


#endif	/* _GNU_SYS_SIGNALFD_H */
