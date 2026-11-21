/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux ioctl(), currently the terminal subset.
 *
 * libc calls TCGETS to implement isatty() (and so to decide whether stdout is
 * line buffered), shells use TCGETS/TCSETS* for raw mode and TIOC[GS]PGRP for
 * job control, and most interactive tools ask for TIOCGWINSZ. FreeBSD
 * translates the same requests in sys/compat/linux/linux_ioctl.c
 * (linux_ioctl_termio(), bsd_to_linux_termios()).
 *
 * The two termios layouts differ, but many flag bits line up: c_iflag and
 * c_oflag bits used here are identical, c_lflag bits are identical up to
 * TOSTOP, and c_cflag differs in CSIZE, the high baud rates and CRTSCTS.
 * Haiku, like System V, shares the VMIN/VTIME slots with VEOF/VEOL.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

#include <sys/ioctl.h>
#include <termios.h>

#include <kernel.h>
#include <tty.h>
#include <vfs.h>


// c_iflag/c_oflag bits identical on Linux and Haiku.
static const uint32 kSharedInputFlags = 0x1fff;		// IGNBRK .. IXOFF
static const uint32 kSharedOutputFlags = 0xff;		// OPOST .. OFDEL
static const uint32 kSharedLocalFlags = 0x1ff;		// ISIG .. TOSTOP
static const uint32 kSharedControlFlags = 0x0fc0;	// CSTOPB .. CLOCAL


struct flag_pair {
	uint32	linux_flag;
	uint32	haiku_flag;
};

static const flag_pair kLocalFlags[] = {
	{ LX_IEXTEN,	IEXTEN },
	{ LX_ECHOCTL,	ECHOCTL },
	{ LX_ECHOPRT,	ECHOPRT },
	{ LX_ECHOKE,	ECHOKE },
	{ LX_FLUSHO,	FLUSHO },
	{ LX_PENDIN,	PENDIN },
};


static uint32
baud_to_haiku(uint32 cflag)
{
	uint32 baud = cflag & LX_CBAUD;
	if ((baud & LX_CBAUDEX) == 0)
		return baud;		// B0 .. B38400 share their values (0 .. 017)
	switch (baud) {
		case LX_CBAUDEX | 1:	return B57600;
		case LX_CBAUDEX | 2:	return B115200;
		case LX_CBAUDEX | 3:	return B230400;
		default:				return B115200;
	}
}


static uint32
baud_from_haiku(uint32 cflag)
{
	uint32 baud = cflag & CBAUD;
	switch (baud) {
		case B57600:	return LX_CBAUDEX | 1;
		case B115200:	return LX_CBAUDEX | 2;
		case B230400:	return LX_CBAUDEX | 3;
		case B31250:	return B38400;
		default:		return baud;
	}
}


static void
termios_from_haiku(const struct termios& haiku, linux_termios& linux)
{
	memset(&linux, 0, sizeof(linux));
	linux.c_iflag = haiku.c_iflag & kSharedInputFlags;
	linux.c_oflag = haiku.c_oflag & kSharedOutputFlags;

	linux.c_cflag = (haiku.c_cflag & kSharedControlFlags)
		| baud_from_haiku(haiku.c_cflag);
	linux.c_cflag |= (haiku.c_cflag & CS8) != 0 ? LX_CS8 : LX_CS7;
	if ((haiku.c_cflag & CRTSCTS) == CRTSCTS)
		linux.c_cflag |= LX_CRTSCTS;

	linux.c_lflag = haiku.c_lflag & kSharedLocalFlags;
	for (size_t i = 0; i < B_COUNT_OF(kLocalFlags); i++) {
		if ((haiku.c_lflag & kLocalFlags[i].haiku_flag) != 0)
			linux.c_lflag |= kLocalFlags[i].linux_flag;
	}

	linux.c_line = haiku.c_line;
	linux.c_cc[LX_VINTR] = haiku.c_cc[VINTR];
	linux.c_cc[LX_VQUIT] = haiku.c_cc[VQUIT];
	linux.c_cc[LX_VERASE] = haiku.c_cc[VERASE];
	linux.c_cc[LX_VKILL] = haiku.c_cc[VKILL];
	linux.c_cc[LX_VEOL2] = haiku.c_cc[VEOL2];
	linux.c_cc[LX_VSWTC] = haiku.c_cc[VSWTCH];
	linux.c_cc[LX_VSTART] = haiku.c_cc[VSTART];
	linux.c_cc[LX_VSTOP] = haiku.c_cc[VSTOP];
	linux.c_cc[LX_VSUSP] = haiku.c_cc[VSUSP];
	if ((haiku.c_lflag & ICANON) != 0) {
		linux.c_cc[LX_VEOF] = haiku.c_cc[VEOF];
		linux.c_cc[LX_VEOL] = haiku.c_cc[VEOL];
		linux.c_cc[LX_VMIN] = 1;
		linux.c_cc[LX_VTIME] = 0;
	} else {
		linux.c_cc[LX_VMIN] = haiku.c_cc[VMIN];
		linux.c_cc[LX_VTIME] = haiku.c_cc[VTIME];
		linux.c_cc[LX_VEOF] = 4;		// ^D
		linux.c_cc[LX_VEOL] = 0;
	}
}


static void
termios_to_haiku(const linux_termios& linux, struct termios& haiku)
{
	// Start from the current Haiku state, so fields Linux cannot express
	// (the split input/output speeds) are kept.
	haiku.c_iflag = (haiku.c_iflag & ~kSharedInputFlags)
		| (linux.c_iflag & kSharedInputFlags);
	haiku.c_oflag = (haiku.c_oflag & ~kSharedOutputFlags)
		| (linux.c_oflag & kSharedOutputFlags);

	uint32 cflag = (linux.c_cflag & kSharedControlFlags)
		| baud_to_haiku(linux.c_cflag);
	if ((linux.c_cflag & LX_CSIZE) == LX_CS8)
		cflag |= CS8;
	if ((linux.c_cflag & LX_CRTSCTS) != 0)
		cflag |= CRTSCTS;
	haiku.c_cflag = cflag;

	uint32 lflag = linux.c_lflag & kSharedLocalFlags;
	for (size_t i = 0; i < B_COUNT_OF(kLocalFlags); i++) {
		if ((linux.c_lflag & kLocalFlags[i].linux_flag) != 0)
			lflag |= kLocalFlags[i].haiku_flag;
	}
	haiku.c_lflag = lflag;

	haiku.c_line = linux.c_line;
	haiku.c_cc[VINTR] = linux.c_cc[LX_VINTR];
	haiku.c_cc[VQUIT] = linux.c_cc[LX_VQUIT];
	haiku.c_cc[VERASE] = linux.c_cc[LX_VERASE];
	haiku.c_cc[VKILL] = linux.c_cc[LX_VKILL];
	haiku.c_cc[VEOL2] = linux.c_cc[LX_VEOL2];
	haiku.c_cc[VSWTCH] = linux.c_cc[LX_VSWTC];
	haiku.c_cc[VSTART] = linux.c_cc[LX_VSTART];
	haiku.c_cc[VSTOP] = linux.c_cc[LX_VSTOP];
	haiku.c_cc[VSUSP] = linux.c_cc[LX_VSUSP];
	if ((linux.c_lflag & LX_ICANON) != 0) {
		haiku.c_cc[VEOF] = linux.c_cc[LX_VEOF];
		haiku.c_cc[VEOL] = linux.c_cc[LX_VEOL];
	} else {
		haiku.c_cc[VMIN] = linux.c_cc[LX_VMIN];
		haiku.c_cc[VTIME] = linux.c_cc[LX_VTIME];
	}
}


//! Runs a Haiku ioctl whose argument has to be a struct in user memory.
static status_t
haiku_ioctl_struct(int fd, uint32 command, void* data, size_t size,
	bool copyBack)
{
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userData = renguin_scratch_put(&scratch, data, size);
	if (userData == 0)
		return B_BAD_ADDRESS;

	status_t status = _user_ioctl(fd, command, (void*)(addr_t)userData, size);
	if (status == B_OK && copyBack
		&& user_memcpy(data, (void*)(addr_t)userData, size) != B_OK) {
		return B_BAD_ADDRESS;
	}
	return status;
}


static int64
tty_status(status_t status)
{
	// Haiku's non-tty devices answer terminal requests with B_BAD_VALUE or
	// B_DEV_INVALID_IOCTL; Linux programs expect ENOTTY.
	if (status == B_OK)
		return 0;
	if (status == B_BAD_VALUE || status == B_DEV_INVALID_IOCTL
		|| status == B_UNSUPPORTED || status == B_NOT_SUPPORTED) {
		return -LX_ENOTTY;
	}
	return renguin_encode(status);
}


int64
renguin_sys_ioctl(int fd, uint64 cmd, uint64 arg)
{
	switch ((uint32)cmd) {
		case LX_TCGETS:
		{
			struct termios haiku;
			memset(&haiku, 0, sizeof(haiku));
			status_t status = haiku_ioctl_struct(fd, TCGETA, &haiku,
				sizeof(haiku), true);
			if (status != B_OK)
				return tty_status(status);
			linux_termios linux;
			termios_from_haiku(haiku, linux);
			if (renguin_copy_out(arg, &linux, sizeof(linux)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}

		case LX_TCSETS:
		case LX_TCSETSW:
		case LX_TCSETSF:
		{
			linux_termios linux;
			if (renguin_copy_in(&linux, arg, sizeof(linux)) != B_OK)
				return -LX_EFAULT;
			struct termios haiku;
			memset(&haiku, 0, sizeof(haiku));
			status_t status = haiku_ioctl_struct(fd, TCGETA, &haiku,
				sizeof(haiku), true);
			if (status != B_OK)
				return tty_status(status);
			termios_to_haiku(linux, haiku);
			uint32 command = cmd == LX_TCSETS ? TCSETA
				: cmd == LX_TCSETSW ? TCSETAW : TCSETAF;
			return tty_status(haiku_ioctl_struct(fd, command, &haiku,
				sizeof(haiku), false));
		}

		// struct winsize and pid_t are laid out the same on both systems,
		// so the program's own buffer is passed through.
		case LX_TIOCGWINSZ:
			return tty_status(_user_ioctl(fd, TIOCGWINSZ,
				(void*)(addr_t)arg, sizeof(struct winsize)));
		case LX_TIOCSWINSZ:
			return tty_status(_user_ioctl(fd, TIOCSWINSZ,
				(void*)(addr_t)arg, sizeof(struct winsize)));
		case LX_TIOCGPGRP:
			return tty_status(_user_ioctl(fd, TIOCGPGRP,
				(void*)(addr_t)arg, sizeof(pid_t)));
		case LX_TIOCSPGRP:
			return tty_status(_user_ioctl(fd, TIOCSPGRP,
				(void*)(addr_t)arg, sizeof(pid_t)));
		case LX_TIOCGSID:
			return tty_status(_user_ioctl(fd, TIOCGSID,
				(void*)(addr_t)arg, sizeof(pid_t)));
		case LX_TIOCSCTTY:
			return tty_status(_user_ioctl(fd, TIOCSCTTY, NULL, 0));

		// Renga (2026-11-14): Linux pseudo-terminals on Haiku's pty driver
		// (the /dev/pts names: path.cpp). Haiku's slaves need no unlocking
		// and no grantpt() permission change for the owner.
		case LX_TIOCGPTN:
		{
			// the pty's number: /dev/pts/<n>
			int32 index;
			status_t status = haiku_ioctl_struct(fd, B_IOCTL_GET_TTY_INDEX,
				&index, sizeof(index), true);
			if (status != B_OK)
				return tty_status(status);
			uint32 number = (uint32)index;
			if (renguin_copy_out(arg, &number, sizeof(number)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}
		case LX_TIOCSPTLCK:
		{
			// (un)lock: nothing to do, but only a master answers
			int32 index;
			status_t status = haiku_ioctl_struct(fd, B_IOCTL_GET_TTY_INDEX,
				&index, sizeof(index), true);
			return tty_status(status);
		}
		case LX_TIOCGPTPEER:
		{
			// opens the slave of this master; arg holds the open() flags
			int32 index;
			status_t status = haiku_ioctl_struct(fd, B_IOCTL_GET_TTY_INDEX,
				&index, sizeof(index), true);
			if (status != B_OK)
				return tty_status(status);
			char path[32];
			snprintf(path, sizeof(path), "/dev/tt/%c%" B_PRIx32,
				(char)('p' + index / 16), (uint32)index % 16);
			renguin_scratch scratch;
			renguin_scratch_init(&scratch);
			uint64 user = renguin_scratch_put(&scratch, path,
				strlen(path) + 1);
			if (user == 0)
				return -LX_EFAULT;
			int slave = _user_open(-1, (const char*)(addr_t)user,
				open_flags_to_haiku(arg), 0);
			return slave >= 0 ? slave : renguin_encode(slave);
		}
		case LX_TIOCNOTTY:
			// drop the controlling terminal: a session leader's own
			// business on Haiku; accepted
			return 0;

		case LX_FIONREAD:
			return renguin_encode(_user_ioctl(fd, FIONREAD,
				(void*)(addr_t)arg, sizeof(int)));
		case LX_FIONBIO:
			return renguin_encode(_user_ioctl(fd, FIONBIO,
				(void*)(addr_t)arg, sizeof(int)));

		case LX_FIOCLEX:
			return renguin_encode(_user_fcntl(fd, F_SETFD, FD_CLOEXEC));
		case LX_FIONCLEX:
			return renguin_encode(_user_fcntl(fd, F_SETFD, 0));

		default:
		{
			// GPU synchronisation (Quasar R-18): DRM sync objects (type
			// 'd', the DRM_IOCTL_SYNCOBJ_* numbers), sync_file ('>') and
			// sw_sync ('W') requests. Renga's syncobj module uses Linux's
			// layouts and numbers, so the program's own buffer goes
			// through unchanged, whatever node or descriptor it is on.
			uint32 type = ((uint32)cmd >> 8) & 0xff;
			uint32 nr = (uint32)cmd & 0xff;
			bool syncRequest = (type == 'd' && nr >= 0xbf && nr <= 0xcf
					&& nr != 0xc6 && nr != 0xc7 && nr != 0xc8 && nr != 0xc9
					&& nr != 0xce)
				|| (type == '>' && nr >= 3 && nr <= 5)
				|| (type == 'W' && nr <= 1);
			// Input devices (Renga 2026-11-21, F-42 for Renguin): evdev
			// ('E', /dev/input/event*), uinput ('U', /dev/input/uinput)
			// and hidraw ('H', /dev/hidraw*). Renga's gamepad core speaks
			// Linux's numbers and layouts (<linux/input.h>), including
			// requests that pass a value instead of a pointer (EVIOCGRAB,
			// EVIOCRMFF, UI_SET_EVBIT ...), so they go through unchanged.
			bool inputRequest = type == 'E' || type == 'U' || type == 'H';
			if (syncRequest || inputRequest) {
				size_t size = ((uint32)cmd >> 16) & 0x3fff;
				return renguin_encode(_user_ioctl(fd, (uint32)cmd,
					(void*)(addr_t)arg, size));
			}

			// Socket requests (interfaces, SIOCATMARK): net.cpp.
			if (((uint32)cmd & 0xff00) == 0x8900) {
				bool handled;
				int64 result = renguin_net_ioctl(fd, (uint32)cmd, arg,
					&handled);
				if (handled)
					return result;
			}
		}
			// Unknown requests: ENOTTY is what Linux returns for a request
			// the device does not understand.
			return -LX_ENOTTY;
	}
}
